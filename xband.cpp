/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * XBAND modem peripheral emulation.
 *
 * The XBAND was a pass-through cartridge modem by Catapult Entertainment
 * (1994-1997). It contained:
 *   - 1MB firmware ROM (XBAND BIOS)
 *   - 64KB SRAM (profiles, patches, mail, news, icons)
 *   - Rockwell RC2324DP 2400-baud modem (UART-style, 16550-compatible)
 *   - "Fred" chip — up to ~16 patch vectors that intercept game ROM reads
 *     and substitute replacement bytes. This was how XBAND redirected
 *     controller polls to inject network-received inputs.
 *
 * This file emulates the hardware side of XBAND. The original network
 * protocol (a modified early ADSP) is handled by the XBAND firmware
 * itself — we only shuttle raw bytes between the modem's RX/TX FIFOs
 * and a TCP socket connected to a replacement XBAND server (such as
 * xband.retrocomputing.network).
 *
 * References:
 *   - https://github.com/Cinghialotto/xband  (Catapult source dump)
 *   - https://fresh-eggs.github.io/xband_post.html
 *   - https://xbandwiki.retrocomputing.network/
 *   - bsnes-plus xband_support branch (prior art)
 */

// Winsock2 MUST be included before <windows.h> (which snes9x.h pulls in
// transitively), otherwise the old <winsock.h> gets included first and we
// get a cascade of redefinition errors.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  typedef SOCKET xband_sock_t;
  #define XBAND_CLOSESOCKET(s)	closesocket((SOCKET)(s))
  #define XBAND_INVALID_SOCKET	((intptr_t)INVALID_SOCKET)
  #define XBAND_SOCKET_ERROR	SOCKET_ERROR
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int xband_sock_t;
  #define XBAND_CLOSESOCKET(s)	close((int)(s))
  #define XBAND_INVALID_SOCKET	(-1)
  #define XBAND_SOCKET_ERROR	(-1)
#endif

#include "snes9x.h"
#include "memmap.h"
#include "fscompat.h"
#include "xband.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Global instance, referenced by memory dispatch.
struct SXBAND XBand;

#ifdef _WIN32
static bool s_winsock_inited = false;
#endif

// -----------------------------------------------------------------------
// Access trace buffer — tiny ring of recent MMIO accesses, used for
// debugging unexplained deadlocks / hangs while iterating on the modem
// register behaviour.
// -----------------------------------------------------------------------

// XBAND_TRACE_SIZE and struct XBandTraceEntry are defined in xband.h
// so the deadlock handler in cpuexec.cpp can read trace entries.

static XBandTraceEntry xband_trace[XBAND_TRACE_SIZE];
static int  xband_trace_head    = 0;
static int  xband_trace_count   = 0;
static bool xband_trace_suppress = false;

void S9xXBandTraceSuppress (bool on)
{
	xband_trace_suppress = on;
}

bool S9xXBandGetTraceEntry (int index, struct XBandTraceEntry *out)
{
	if (!out) return false;
	if (index < 0 || index >= xband_trace_count) return false;
	int idx = (xband_trace_head - xband_trace_count + index + XBAND_TRACE_SIZE)
	          % XBAND_TRACE_SIZE;
	*out = xband_trace[idx];
	return true;
}

static void xband_trace_log (uint32 address, uint8 value, bool is_write)
{
	if (xband_trace_suppress) return;
	xband_trace[xband_trace_head].address  = address;
	xband_trace[xband_trace_head].caller_pc =
		(uint32)((Registers.PBPC & 0xffffff));
	xband_trace[xband_trace_head].value    = value;
	xband_trace[xband_trace_head].is_write = is_write;
	xband_trace_head = (xband_trace_head + 1) % XBAND_TRACE_SIZE;
	if (xband_trace_count < XBAND_TRACE_SIZE)
		xband_trace_count++;
}

// -----------------------------------------------------------------------
// Kill / control register trace — see xband.h for the rationale.
// -----------------------------------------------------------------------

static XBandKCtlEntry xband_kctl_trace[XBAND_KCTL_TRACE_SIZE];
static int            xband_kctl_head  = 0;
static int            xband_kctl_count = 0;

// Per-address-class counters so we can tell at a glance which candidate
// register window the BIOS is actually using, even if the ring overflows.
static uint64 xband_kctl_fb_fe01_writes = 0;
static uint64 xband_kctl_fb_fe01_reads  = 0;
static uint64 xband_kctl_fb_fe03_writes = 0;
static uint64 xband_kctl_fb_fe03_reads  = 0;
static uint64 xband_kctl_61_7000_writes = 0;
static uint64 xband_kctl_61_7000_reads  = 0;
static uint64 xband_kctl_61_7001_writes = 0;
static uint64 xband_kctl_61_7001_reads  = 0;

// "Silent write" trap state. Same ring storage style — separate ring
// so it doesn't get evicted by kill/control accesses (and vice versa).
#define XBAND_SILENT_TRACE_SIZE 128
static XBandKCtlEntry xband_silent_trace[XBAND_SILENT_TRACE_SIZE];
static int            xband_silent_head    = 0;
static int            xband_silent_count   = 0;
static uint64         xband_silent_total   = 0;
// Per-bank counter (256 buckets) so totals survive ring overflow.
static uint64         xband_silent_perbank[256] = {0};

// Per-Fred-register write counter. 224 general regs ($00..$BF) plus the
// 32 modem regs ($C0..$FF), in flat indexing matching how S9xSetXBand
// computes `reg`. Updated from S9xSetXBand on every Fred reg write.
static uint64 xband_fred_writes_by_reg[256] = {0};
static uint64 xband_fred_writes_total       = 0;

// Cross-bank read trap state: data reads from BIOS code (PB in
// $D0-$DF / $50-$5F) into cart-side HiROM banks ($00-$3F:$8000+,
// $40-$7D, $80-$BF:$8000+, $C0-$CF). These are the "smoking gun"
// reads — the BIOS expects cart bytes there, but our map gives it
// BIOS bytes. We log them so we can see WHERE the BIOS thinks the
// cart should be.
#define XBAND_SREAD_TRACE_SIZE 128
static XBandKCtlEntry xband_sread_trace[XBAND_SREAD_TRACE_SIZE];
static int            xband_sread_head    = 0;
static int            xband_sread_count   = 0;
static uint64         xband_sread_total   = 0;
// Per-target-bank counter so totals survive ring overflow.
static uint64         xband_sread_perbank[256] = {0};

// ADSP handshake counter — bumped each time the identity prefix string
// is shipped to the server. Defined here (early in the file) so the
// kctl dump above can reference it; the actual send happens in
// S9xXBandPoll near the bottom of the file.
static uint64 xband_identity_sends = 0;

// Socket-level byte counters and EOF detection. These survive across
// disconnect/reconnect within a session so we can see total volume in
// the kctl dump even after the BIOS tears down the connection. The
// `eof_seen` flag is set when recv() returns 0 (clean server-side
// close) — that's how we tell "server hung up on us" apart from "BIOS
// hung up on the server".
static uint64 xband_sock_rx_bytes = 0;
static uint64 xband_sock_tx_bytes = 0;
static bool   xband_sock_eof_seen = false;
static int    xband_sock_disconnects_by_bios = 0;

// HELO-probe filter: the 16bit.retrocomputing.network:56969 endpoint
// turns out to be a softmodem-bridge, not a raw ADSP server. It sends
// "HELO\n" repeatedly while waiting for a dialup modem to answer. If
// we forward those bytes to the BIOS rxbuf the firmware sees garbage
// and gives up. Strip them at the socket-read layer; only "real"
// (non-HELO\n) bytes get pushed into the BIOS.
//
// State machine: tiny 5-byte "HELO\n" matcher. We accumulate bytes
// and only commit them to the BIOS rxbuf once we know they're not
// part of a HELO probe. If we match a full HELO\n, increment the
// discard counter and forget the bytes. If we drift off-pattern,
// flush the partial match into rxbuf and continue.
static uint64 xband_helo_discarded = 0;
static int    xband_helo_match_pos = 0;
static const char xband_helo_signature[5] = {'H','E','L','O','\n'};

// ADSP frame detector state. Runs over the raw RX byte stream from
// the server (BEFORE the HELO filter) and tracks `\x00...\x10\x03`
// frame boundaries with byte-stuffing decode (`\x10\x10` → `\x10`)
// and CCITT-16 CRC validation. Tells us the instant the server
// transitions from HELO probes to real ADSP frames.
//
// Frame format per xband_post.txt:
//   `\x00` + escape(data + crc16-be(data)) + `\x10\x03`
// Where escape() doubles `\x10` to `\x10\x10`.
enum {
	XBAND_ADSP_WAITING  = 0,   // looking for frame start (\x00)
	XBAND_ADSP_IN_FRAME = 1,   // inside a frame, accumulating
	XBAND_ADSP_ESCAPE   = 2,   // just saw \x10 inside frame
};

#define XBAND_ADSP_FRAME_BUF_SIZE 2048
static uint8 xband_adsp_frame_buf[XBAND_ADSP_FRAME_BUF_SIZE];
static int   xband_adsp_frame_pos = 0;
static int   xband_adsp_state     = XBAND_ADSP_WAITING;

static uint64 xband_adsp_frames_total    = 0;
static uint64 xband_adsp_frames_good_crc = 0;
static uint64 xband_adsp_frames_bad_crc  = 0;
static uint64 xband_adsp_frames_aborted  = 0;
static uint64 xband_adsp_frames_control  = 0;  // tiny <4-byte (no CRC)

// First N captured RX frames, for inspection in the kctl dump. Store
// up to 4 so we can compare structures and CRC values side-by-side.
#define XBAND_ADSP_FIRST_FRAMES 4
static uint8  xband_adsp_first_frame[XBAND_ADSP_FIRST_FRAMES][256];
static int    xband_adsp_first_frame_len[XBAND_ADSP_FIRST_FRAMES] = {0};
static uint16 xband_adsp_first_frame_crc_expected[XBAND_ADSP_FIRST_FRAMES] = {0};
// Multiple CRC variants computed per frame so we can identify the
// right formula by hand. Index meanings (also see the labels in
// xband_crc_variant_names below):
//   0: CCITT-16-FALSE       (init $FFFF, xor $FFFF)
//   1: CCITT-16-FALSE-noxor (init $FFFF, no xor)
//   2: CCITT-16-XMODEM      (init $0000, no xor)
//   3: CCITT-16-AUG         (init $1D0F, no xor)
//   4: CRC-16-Kermit        (init $0000, reflected, no xor)
//   5: same as 0 but data INCLUDES the leading \x00 encap byte
//   6: same as 0 but byte order swapped (little-endian compare)
#define XBAND_CRC_VARIANTS 7
static uint16 xband_adsp_first_frame_crcs[XBAND_ADSP_FIRST_FRAMES][XBAND_CRC_VARIANTS] = {{0}};
static const char *xband_crc_variant_names[XBAND_CRC_VARIANTS] = {
	"CCITT-FALSE     ", // 0
	"CCITT-FALSE-noxor", // 1
	"CCITT-XMODEM    ", // 2
	"CCITT-AUG ($1D0F)", // 3
	"CRC-16-Kermit   ", // 4
	"FALSE+leading\\x00", // 5
	"FALSE byte-swap ", // 6
};
static int    xband_adsp_first_frame_count = 0;

// TX frame counter — bumped each time the TX accumulator successfully
// sends a complete `\x10\x03`-terminated packet to the socket.
// (Distinct from xband_sock_tx_bytes which counts individual bytes.)
static uint64 xband_tx_frames_sent = 0;

// Remembered host / port from the last successful Connect click. Lets
// the BIOS-driven retry loop (which asserts RTS in modem reg $08
// every time it wants to "dial") auto-reconnect without a fresh menu
// click. Also bumped on each auto-reconnect so we can see in the
// kctl dump how many times the BIOS retried.
static char   xband_last_host[256] = {0};
static int    xband_last_port      = 0;
static int    xband_auto_reconnects = 0;

// Runtime toggle for the HELO\n filter. DEFAULT IS OFF — the dreampi
// reference implementation (netlink.py xband_server) forwards every
// server byte raw to the modem without filtering, and that's the only
// known-working client against this same server backend. Our earlier
// HELO filter was a workaround that turned out to be unnecessary. Kept
// as a runtime toggle so we can re-enable for A/B testing if needed.
static bool xband_helo_filter_enabled = false;

// Forward declaration: xband_try_auto_reconnect is defined down in
// the network bridging section, but called from S9xSetXBand (modem
// reg $08 RTS handler) which lives much earlier in the file.
static void xband_try_auto_reconnect (void);

// First-N capture buffers for both directions. These let us hex/ASCII
// dump the start of each conversation in the kctl popup so we can
// identify the on-the-wire protocol (ADSP framing, server banner,
// etc.). Both fill once and stop — they hold the FIRST bytes only,
// not the most recent, so we always see the protocol opening.
#define XBAND_SOCK_FIRST_SIZE 256
static uint8  xband_sock_rx_first[XBAND_SOCK_FIRST_SIZE];
static int    xband_sock_rx_first_used = 0;
static uint8  xband_sock_tx_first[XBAND_SOCK_FIRST_SIZE];
static int    xband_sock_tx_first_used = 0;

// First-N capture of BIOS TX events with the calling PC. Reuses the
// XBandKCtlEntry struct for storage convenience even though these
// aren't kctl events. Captured at S9xSetXBand fred-reg-$90 write
// time, NOT at socket-flush time, so the PC is the BIOS instruction
// that pushed the byte (which can then be cross-referenced against
// the symbol list from xband_post.txt: _SendMessage, TNetIdle,
// _PUProcessSTIdle, etc.).
#define XBAND_BIOS_TX_FIRST_SIZE 64
static XBandKCtlEntry xband_bios_tx_first[XBAND_BIOS_TX_FIRST_SIZE];
static int            xband_bios_tx_first_used = 0;

// Per-trap immediate caller return address. JSL pushes (PB, PCH, PCL)
// on the 65C816, so the top 3 bytes of the stack at trap time encode
// the JSL site that called send-byte. Captured for each $90 write so
// we can tell whether all 19 calls have the same caller (single loop)
// or different callers (multiple call sites). Read via xband_peek_byte
// to avoid CPU.Cycles pollution that would otherwise shift modem
// timing forward and break the BIOS.
static uint32 xband_bios_tx_first_ret[XBAND_BIOS_TX_FIRST_SIZE];

// Stack + buffer + DBR snapshot taken at the moment of the FIRST $90
// write event. Used to:
//  - hand-trace the call chain (top 64 bytes of stack)
//  - resolve absolute-mode operands in the loop body (DBR)
//  - identify the source buffer the loop is reading from (long
//    pointer at DBR:$3DC3..$3DC5)
static uint8  xband_bios_tx_first_stack[64];
static uint16 xband_bios_tx_first_s  = 0;
static uint8  xband_bios_tx_first_db = 0;
static bool   xband_bios_tx_first_stack_captured = false;
static uint8  xband_bios_tx_first_buffer[64];
static uint32 xband_bios_tx_first_buffer_addr = 0;

void S9xXBandKCtlLog (uint32 address, uint8 value, bool is_write)
{
	uint32 a = address & 0xFFFFFF;

	// Bump per-class counter so totals survive ring overflow.
	if (a == 0xFBFE01)
	{
		if (is_write) xband_kctl_fb_fe01_writes++;
		else          xband_kctl_fb_fe01_reads++;
	}
	else if (a == 0xFBFE03)
	{
		if (is_write) xband_kctl_fb_fe03_writes++;
		else          xband_kctl_fb_fe03_reads++;
	}
	else if (a == 0x617000)
	{
		if (is_write) xband_kctl_61_7000_writes++;
		else          xband_kctl_61_7000_reads++;
	}
	else if (a == 0x617001)
	{
		if (is_write) xband_kctl_61_7001_writes++;
		else          xband_kctl_61_7001_reads++;
	}

	xband_kctl_trace[xband_kctl_head].pc       =
		(uint32)(Registers.PBPC & 0xFFFFFF);
	xband_kctl_trace[xband_kctl_head].address  = a;
	xband_kctl_trace[xband_kctl_head].value    = value;
	xband_kctl_trace[xband_kctl_head].is_write = is_write;
	xband_kctl_head = (xband_kctl_head + 1) % XBAND_KCTL_TRACE_SIZE;
	if (xband_kctl_count < XBAND_KCTL_TRACE_SIZE)
		xband_kctl_count++;
}

// Hex/ASCII dumper for the captured first-N socket bytes. 16 bytes per
// row, address column on the left, ASCII column on the right. Used by
// the kctl dump to print the start of both directions of the
// conversation so we can identify the protocol.
static size_t xband_hex_ascii_dump (char *out, size_t out_size,
                                    const uint8 *data, int n)
{
	size_t pos = 0;
	for (int row = 0; row < n && pos + 80 < out_size; row += 16)
	{
		pos += snprintf(out + pos, out_size - pos, "  %04X: ", row);
		for (int col = 0; col < 16; col++)
		{
			if (row + col < n)
				pos += snprintf(out + pos, out_size - pos, "%02X ",
					(unsigned)data[row + col]);
			else
				pos += snprintf(out + pos, out_size - pos, "   ");
		}
		pos += snprintf(out + pos, out_size - pos, " |");
		for (int col = 0; col < 16 && row + col < n; col++)
		{
			uint8 c = data[row + col];
			pos += snprintf(out + pos, out_size - pos, "%c",
				(c >= 0x20 && c < 0x7F) ? c : '.');
		}
		pos += snprintf(out + pos, out_size - pos, "|\n");
	}
	return pos;
}

// Side-effect-free memory peek for debug reads from inside the trap
// handler. Calling S9xGetByte from inside S9xSetXBand bumps CPU.Cycles
// (via addCyclesInMemoryAccess) and can trigger S9xDoHEventProcessing
// — which shifts H-blank / VBlank / IRQ timing forward and breaks the
// modem driver. This helper reads memory directly through Memory.Map[]
// without touching CPU.Cycles. Linear-mapped regions return the byte
// directly; the MAP_XBAND case (XBAND SRAM mirror window) reads from
// XBand.sram[] directly. MMIO ($FB:$C000-$FFFF) is stateful so we
// return 0 there. Other non-linear maps fall back to 0.
static uint8 xband_peek_byte (uint32 address)
{
	int block = (address & 0xFFFFFF) >> MEMMAP_SHIFT;
	uint8 *p = Memory.Map[block];
	if (p >= (uint8 *) CMemory::MAP_LAST)
		return *(p + (address & 0xFFFF));
	if ((pint) p == (pint) CMemory::MAP_XBAND)
	{
		// XBAND SRAM mirror window — bank $E0-$FA, $FB:$0000-$BFFF,
		// $FC-$FF, or $60-$7D mirror. Read XBand.sram[] directly.
		uint8  bank = (uint8)((address >> 16) & 0xFF);
		uint16 off  = (uint16)(address & 0xFFFF);
		bool sram =
			(bank >= 0xE0 && bank <= 0xFA) ||
			(bank == 0xFB && off <= 0xBFFF) ||
			(bank >= 0xFC && bank <= 0xFF) ||
			(bank >= 0x60 && bank <= 0x7D);
		if (sram)
			return XBand.sram[off & (XBAND_SRAM_SIZE - 1)];
		return 0; // MMIO ($FB:$C000-$FFFF) — stateful, don't poke it
	}
	return 0;
}

// CRC variants to test against the captured frames. The protocol uses
// CCITT-16 family but the exact init/xor/byte-order is unconfirmed.
// We compute several variants per frame and the dump shows all of
// them so we can hand-pick which one the server is actually using.
//
// All variants use poly $1021 (no reflection). Differences are in
// init value, final XOR, and whether they reflect bits. The
// "_FALSE", "_XMODEM", "_AUG" suffixes are standard CCITT-16 names.

// CCITT-16-FALSE: init $FFFF, xor $FFFF (IBM original)
static uint16 xband_crc_false (const uint8 *data, int len)
{
	uint16 crc = 0xFFFF;
	for (int i = 0; i < len; i++)
	{
		crc ^= ((uint16)data[i]) << 8;
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000)
				? (uint16)((crc << 1) ^ 0x1021)
				: (uint16)(crc << 1);
	}
	return crc ^ 0xFFFF;
}

// CCITT-16-FALSE without final xor (init $FFFF, no xor — what some
// references call "CCITT FALSE residue form")
static uint16 xband_crc_false_noxor (const uint8 *data, int len)
{
	uint16 crc = 0xFFFF;
	for (int i = 0; i < len; i++)
	{
		crc ^= ((uint16)data[i]) << 8;
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000)
				? (uint16)((crc << 1) ^ 0x1021)
				: (uint16)(crc << 1);
	}
	return crc;
}

// CCITT-16-XMODEM: init $0000, no xor
static uint16 xband_crc_xmodem (const uint8 *data, int len)
{
	uint16 crc = 0x0000;
	for (int i = 0; i < len; i++)
	{
		crc ^= ((uint16)data[i]) << 8;
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000)
				? (uint16)((crc << 1) ^ 0x1021)
				: (uint16)(crc << 1);
	}
	return crc;
}

// CCITT-16-AUG: init $1D0F, no xor
static uint16 xband_crc_aug (const uint8 *data, int len)
{
	uint16 crc = 0x1D0F;
	for (int i = 0; i < len; i++)
	{
		crc ^= ((uint16)data[i]) << 8;
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000)
				? (uint16)((crc << 1) ^ 0x1021)
				: (uint16)(crc << 1);
	}
	return crc;
}

// CRC-16-Kermit / CCITT-TRUE: init $0000, REFLECTED bits, no xor
static uint16 xband_crc_kermit (const uint8 *data, int len)
{
	uint16 crc = 0x0000;
	for (int i = 0; i < len; i++)
	{
		crc ^= (uint16)data[i];
		for (int j = 0; j < 8; j++)
			crc = (crc & 1)
				? (uint16)((crc >> 1) ^ 0x8408)
				: (uint16)(crc >> 1);
	}
	return crc;
}

// Canonical XBAND CRC: CCITT-16-FALSE computed over the encapsulated
// body (the deframed bytes WITH a synthetic leading `\x00`). Confirmed
// against live server frames where this variant matches the expected
// CRC at the tail. NOTE: this contradicts the PHP framer in
// xband_post.txt (which computes CRC before encapsulation), but the
// wire bytes are authoritative — the live server includes the
// encapsulation byte in the CRC.
static uint16 xband_ccitt_crc16 (const uint8 *data, int len)
{
	uint8 tmp[1024];
	if (len + 1 > (int)sizeof(tmp))
		len = (int)sizeof(tmp) - 1;
	tmp[0] = 0x00;
	memcpy(tmp + 1, data, len);
	return xband_crc_false(tmp, len + 1);
}

// Compute all 7 CRC variants for a frame and store them in the slot.
// Variants 5 and 6 are special cases that don't match the standard
// "data is body[0..n-2], crc is body[n-2..n]" model — they test
// alternate interpretations.
static void xband_compute_all_crcs (int slot, int data_len)
{
	const uint8 *data = xband_adsp_frame_buf;
	uint16 *out = xband_adsp_first_frame_crcs[slot];

	out[0] = xband_crc_false      (data, data_len);
	out[1] = xband_crc_false_noxor(data, data_len);
	out[2] = xband_crc_xmodem     (data, data_len);
	out[3] = xband_crc_aug        (data, data_len);
	out[4] = xband_crc_kermit     (data, data_len);

	// Variant 5: include a synthetic leading \x00 (the encapsulation
	// byte that gets stripped by the deframer). Compute over a
	// temporary buffer with the prefix.
	{
		uint8 tmp[257];
		tmp[0] = 0x00;
		int n = (data_len < 256) ? data_len : 256;
		memcpy(tmp + 1, data, n);
		out[5] = xband_crc_false(tmp, n + 1);
	}

	// Variant 6: same algorithm as #0, but the EXPECTED CRC at the
	// end of the frame is interpreted as little-endian instead of
	// big-endian. Stored as the same computed value as #0; the dump
	// reader compares against the byte-swapped expected.
	out[6] = out[0];
}

// Validate the buffered frame and capture for inspection.
static void xband_adsp_validate_frame (void)
{
	xband_adsp_frames_total++;
	if (xband_adsp_frame_pos < 4)
	{
		// Too short to have data + CRC. ADSP control frames (e.g.
		// the 2-byte $1E0F probe) — no CRC to validate.
		xband_adsp_frames_control++;
	}
	else
	{
		int data_len = xband_adsp_frame_pos - 2;
		uint16 expected =
			((uint16)xband_adsp_frame_buf[data_len] << 8) |
			xband_adsp_frame_buf[data_len + 1];
		uint16 computed = xband_ccitt_crc16(xband_adsp_frame_buf, data_len);

		bool good = (expected == computed);
		if (good)
			xband_adsp_frames_good_crc++;
		else
			xband_adsp_frames_bad_crc++;

		if (xband_adsp_first_frame_count < XBAND_ADSP_FIRST_FRAMES)
		{
			int slot = xband_adsp_first_frame_count++;
			int n = (xband_adsp_frame_pos < 256) ? xband_adsp_frame_pos : 256;
			memcpy(xband_adsp_first_frame[slot], xband_adsp_frame_buf, n);
			xband_adsp_first_frame_len[slot] = n;
			xband_adsp_first_frame_crc_expected[slot] = expected;
			xband_compute_all_crcs(slot, data_len);
		}
		return;
	}

	// Tiny control frame fallthrough — still capture for inspection.
	if (xband_adsp_first_frame_count < XBAND_ADSP_FIRST_FRAMES)
	{
		int slot = xband_adsp_first_frame_count++;
		int n = (xband_adsp_frame_pos < 256) ? xband_adsp_frame_pos : 256;
		memcpy(xband_adsp_first_frame[slot], xband_adsp_frame_buf, n);
		xband_adsp_first_frame_len[slot] = n;
		xband_adsp_first_frame_crc_expected[slot] = 0;
		// Don't compute variants for tiny frames — no data to CRC over.
		for (int v = 0; v < XBAND_CRC_VARIANTS; v++)
			xband_adsp_first_frame_crcs[slot][v] = 0;
	}
}

// Feed one RX byte to the ADSP frame detector state machine.
static void xband_adsp_feed_byte (uint8 b)
{
	switch (xband_adsp_state)
	{
	case XBAND_ADSP_WAITING:
		if (b == 0x00)
		{
			xband_adsp_state     = XBAND_ADSP_IN_FRAME;
			xband_adsp_frame_pos = 0;
		}
		break;

	case XBAND_ADSP_IN_FRAME:
		if (b == 0x10)
		{
			xband_adsp_state = XBAND_ADSP_ESCAPE;
		}
		else
		{
			if (xband_adsp_frame_pos < XBAND_ADSP_FRAME_BUF_SIZE)
				xband_adsp_frame_buf[xband_adsp_frame_pos++] = b;
		}
		break;

	case XBAND_ADSP_ESCAPE:
		if (b == 0x10)
		{
			// Escaped \x10\x10 → emit single \x10 to data
			if (xband_adsp_frame_pos < XBAND_ADSP_FRAME_BUF_SIZE)
				xband_adsp_frame_buf[xband_adsp_frame_pos++] = 0x10;
			xband_adsp_state = XBAND_ADSP_IN_FRAME;
		}
		else if (b == 0x03)
		{
			// End of frame
			xband_adsp_validate_frame();
			xband_adsp_state     = XBAND_ADSP_WAITING;
			xband_adsp_frame_pos = 0;
		}
		else
		{
			// Invalid escape — abort the frame
			xband_adsp_frames_aborted++;
			xband_adsp_state     = XBAND_ADSP_WAITING;
			xband_adsp_frame_pos = 0;
		}
		break;
	}
}

void S9xXBandKCtlReset (void)
{
	xband_kctl_head  = 0;
	xband_kctl_count = 0;
	xband_kctl_fb_fe01_writes = xband_kctl_fb_fe01_reads = 0;
	xband_kctl_fb_fe03_writes = xband_kctl_fb_fe03_reads = 0;
	xband_kctl_61_7000_writes = xband_kctl_61_7000_reads = 0;
	xband_kctl_61_7001_writes = xband_kctl_61_7001_reads = 0;

	xband_silent_head  = 0;
	xband_silent_count = 0;
	xband_silent_total = 0;
	memset(xband_silent_perbank,    0, sizeof(xband_silent_perbank));
	memset(xband_fred_writes_by_reg, 0, sizeof(xband_fred_writes_by_reg));
	xband_fred_writes_total = 0;

	xband_sread_head  = 0;
	xband_sread_count = 0;
	xband_sread_total = 0;
	memset(xband_sread_perbank, 0, sizeof(xband_sread_perbank));

	// Also clear the existing $XX:$FFB0-$FFDF cart-header read counter
	// (defined in cpuexec.cpp) so all the dump's totals belong to the
	// same sampling window.
	{
		extern uint64 XBandHdrReadBank[256];
		extern uint64 XBandHdrReadTotal;
		memset(XBandHdrReadBank, 0, sizeof(XBandHdrReadBank));
		XBandHdrReadTotal = 0;
	}

	// ADSP / network counters — same model: each dump starts a fresh
	// observation window. Don't touch live state (XBand.net_step,
	// XBand.socket_fd) — those reflect the current connection.
	xband_identity_sends           = 0;
	xband_sock_rx_bytes            = 0;
	xband_sock_tx_bytes            = 0;
	xband_sock_eof_seen            = false;
	xband_sock_disconnects_by_bios = 0;
	xband_sock_rx_first_used       = 0;
	xband_sock_tx_first_used       = 0;
	memset(xband_sock_rx_first, 0, sizeof(xband_sock_rx_first));
	memset(xband_sock_tx_first, 0, sizeof(xband_sock_tx_first));
	xband_bios_tx_first_used       = 0;
	memset(xband_bios_tx_first, 0, sizeof(xband_bios_tx_first));
	memset(xband_bios_tx_first_ret, 0, sizeof(xband_bios_tx_first_ret));
	xband_bios_tx_first_stack_captured = false;
	xband_bios_tx_first_s              = 0;
	xband_bios_tx_first_db             = 0;
	memset(xband_bios_tx_first_stack, 0, sizeof(xband_bios_tx_first_stack));
	xband_bios_tx_first_buffer_addr    = 0;
	memset(xband_bios_tx_first_buffer, 0, sizeof(xband_bios_tx_first_buffer));

	// HELO filter counter — also reset per snapshot. Don't reset
	// xband_helo_match_pos: the live filter state machine belongs to
	// the active connection, not the snapshot window.
	xband_helo_discarded = 0;

	// Auto-reconnect counter — same per-snapshot model. Don't clear
	// xband_last_host / xband_last_port: those are session-lifetime
	// state, not snapshot-window observations.
	xband_auto_reconnects = 0;

	// ADSP detector counters — also per-snapshot. Live state
	// (xband_adsp_state, xband_adsp_frame_pos) is NOT reset since
	// it belongs to the active connection, not the dump window.
	xband_adsp_frames_total    = 0;
	xband_adsp_frames_good_crc = 0;
	xband_adsp_frames_bad_crc  = 0;
	xband_adsp_frames_control  = 0;
	xband_adsp_frames_aborted  = 0;
	xband_adsp_first_frame_count = 0;
	memset(xband_adsp_first_frame,        0, sizeof(xband_adsp_first_frame));
	memset(xband_adsp_first_frame_len,    0, sizeof(xband_adsp_first_frame_len));
	memset(xband_adsp_first_frame_crc_expected, 0, sizeof(xband_adsp_first_frame_crc_expected));
	memset(xband_adsp_first_frame_crcs, 0, sizeof(xband_adsp_first_frame_crcs));
	xband_tx_frames_sent       = 0;
}

void S9xXBandSilentWriteLog (uint32 address, uint8 value)
{
	uint32 a = address & 0xFFFFFF;
	xband_silent_total++;
	xband_silent_perbank[(a >> 16) & 0xFF]++;

	xband_silent_trace[xband_silent_head].pc       =
		(uint32)(Registers.PBPC & 0xFFFFFF);
	xband_silent_trace[xband_silent_head].address  = a;
	xband_silent_trace[xband_silent_head].value    = value;
	xband_silent_trace[xband_silent_head].is_write = true;
	xband_silent_head = (xband_silent_head + 1) % XBAND_SILENT_TRACE_SIZE;
	if (xband_silent_count < XBAND_SILENT_TRACE_SIZE)
		xband_silent_count++;
}

void S9xXBandFredRegWriteBump (uint8 reg, uint8 /*value*/)
{
	xband_fred_writes_by_reg[reg]++;
	xband_fred_writes_total++;
}

void S9xXBandCrossBankReadLog (uint32 address, uint8 value)
{
	uint32 a = address & 0xFFFFFF;
	xband_sread_total++;
	xband_sread_perbank[(a >> 16) & 0xFF]++;

	xband_sread_trace[xband_sread_head].pc       =
		(uint32)(Registers.PBPC & 0xFFFFFF);
	xband_sread_trace[xband_sread_head].address  = a;
	xband_sread_trace[xband_sread_head].value    = value;
	xband_sread_trace[xband_sread_head].is_write = false;
	xband_sread_head = (xband_sread_head + 1) % XBAND_SREAD_TRACE_SIZE;
	if (xband_sread_count < XBAND_SREAD_TRACE_SIZE)
		xband_sread_count++;
}

void S9xXBandKCtlDump (char *out, size_t out_size)
{
	if (!out || out_size == 0) return;
	size_t pos = 0;

	const char *netstep_name =
		(XBand.net_step == XBAND_NET_IDLE)      ? "IDLE" :
		(XBand.net_step == XBAND_NET_HANDSHAKE) ? "HANDSHAKE (waiting for server)" :
		(XBand.net_step == XBAND_NET_CONNECTED) ? "CONNECTED (identity sent)" :
		                                          "?";

	pos += snprintf(out + pos, out_size - pos,
		"XBAND kill/control register access trace\n"
		"=========================================\n"
		"\n"
		"Per-address totals (since last reset):\n"
		"  $FB:FE01 (bsnes-plus kill)    : %llu writes, %llu reads\n"
		"  $FB:FE03 (bsnes-plus control) : %llu writes, %llu reads\n"
		"  $61:7000 (catapult kSNESKill) : %llu writes, %llu reads\n"
		"  $61:7001 (catapult kSNESCtl)  : %llu writes, %llu reads\n"
		"\n"
		"Current emulated state:\n"
		"  XBand.kill    = $%02X\n"
		"  XBand.control = $%02X\n"
		"\n"
		"ADSP handshake state:\n"
		"  net_step          = %d (%s)\n"
		"  identity sent     = %llu\n"
		"  socket            = %s\n"
		"  socket RX bytes   = %llu (from server, raw)\n"
		"  socket TX bytes   = %llu (to server)\n"
		"  HELO filter       = %s\n"
		"  HELO\\n discarded  = %llu (probes filtered out)\n"
		"  server EOF seen   = %s\n"
		"  BIOS hangups      = %d (BIOS dropped line relay)\n"
		"  auto-reconnects   = %d (BIOS-driven retries)\n"
		"  remembered host   = %s:%d\n"
		"\n"
		"ADSP frame detector:\n"
		"  RX frames total   = %llu\n"
		"  RX frames good CRC= %llu\n"
		"  RX frames bad CRC = %llu\n"
		"  RX frames control = %llu (tiny <4-byte, no CRC field)\n"
		"  RX frames aborted = %llu (invalid escape sequence)\n"
		"  TX frames sent    = %llu (\\x10\\x03-terminated packets)\n"
		"\n",
		(unsigned long long)xband_kctl_fb_fe01_writes,
		(unsigned long long)xband_kctl_fb_fe01_reads,
		(unsigned long long)xband_kctl_fb_fe03_writes,
		(unsigned long long)xband_kctl_fb_fe03_reads,
		(unsigned long long)xband_kctl_61_7000_writes,
		(unsigned long long)xband_kctl_61_7000_reads,
		(unsigned long long)xband_kctl_61_7001_writes,
		(unsigned long long)xband_kctl_61_7001_reads,
		(unsigned)XBand.kill,
		(unsigned)XBand.control,
		(int)XBand.net_step, netstep_name,
		(unsigned long long)xband_identity_sends,
		(XBand.socket_fd != XBAND_INVALID_SOCKET) ? "open" : "closed",
		(unsigned long long)xband_sock_rx_bytes,
		(unsigned long long)xband_sock_tx_bytes,
		(xband_helo_filter_enabled ? "ON (filtering)" : "OFF (passthrough)"),
		(unsigned long long)xband_helo_discarded,
		xband_sock_eof_seen ? "yes" : "no",
		xband_sock_disconnects_by_bios,
		xband_auto_reconnects,
		(xband_last_host[0] ? xband_last_host : "(none)"),
		xband_last_port,
		(unsigned long long)xband_adsp_frames_total,
		(unsigned long long)xband_adsp_frames_good_crc,
		(unsigned long long)xband_adsp_frames_bad_crc,
		(unsigned long long)xband_adsp_frames_control,
		(unsigned long long)xband_adsp_frames_aborted,
		(unsigned long long)xband_tx_frames_sent);

	// First N captured ADSP frames with CRC details for all variants.
	// The variant whose computed CRC matches the expected value is
	// the one the protocol actually uses.
	if (xband_adsp_first_frame_count > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"First %d ADSP frame(s) captured (deframed body, last 2 bytes are CRC):\n",
			xband_adsp_first_frame_count);
		for (int i = 0; i < xband_adsp_first_frame_count && pos + 400 < out_size; i++)
		{
			pos += snprintf(out + pos, out_size - pos,
				"\nFrame #%d (%d bytes):\n",
				i, xband_adsp_first_frame_len[i]);
			pos += xband_hex_ascii_dump(out + pos, out_size - pos,
				xband_adsp_first_frame[i], xband_adsp_first_frame_len[i]);
			if (xband_adsp_first_frame_len[i] >= 4)
			{
				uint16 expected = xband_adsp_first_frame_crc_expected[i];
				uint16 expected_swap = (uint16)((expected >> 8) | (expected << 8));
				pos += snprintf(out + pos, out_size - pos,
					"  CRC expected (BE) = $%04X  (LE = $%04X)\n",
					(unsigned)expected, (unsigned)expected_swap);
				for (int v = 0; v < XBAND_CRC_VARIANTS; v++)
				{
					uint16 c = xband_adsp_first_frame_crcs[i][v];
					// For variant 6 (byte-swap), compare against swapped expected.
					uint16 cmp = (v == 6) ? expected_swap : expected;
					const char *match = (c == cmp) ? "  <-- MATCH!" : "";
					pos += snprintf(out + pos, out_size - pos,
						"    [%d] %s = $%04X%s\n",
						v, xband_crc_variant_names[v],
						(unsigned)c, match);
				}
			}
		}
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	// Hex/ASCII dump of the first bytes in each direction. The RX dump
	// is the most informative — that's the server's actual response to
	// our identity. If we can read the protocol bytes here we can pick
	// the right ADSP framing or banner format.
	if (xband_sock_rx_first_used > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"First %d RX bytes from server:\n", xband_sock_rx_first_used);
		pos += xband_hex_ascii_dump(out + pos, out_size - pos,
			xband_sock_rx_first, xband_sock_rx_first_used);
		pos += snprintf(out + pos, out_size - pos, "\n");
	}
	if (xband_sock_tx_first_used > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"First %d TX bytes from BIOS (after identity):\n",
			xband_sock_tx_first_used);
		pos += xband_hex_ascii_dump(out + pos, out_size - pos,
			xband_sock_tx_first, xband_sock_tx_first_used);
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	// BIOS-side fred reg $90 write events with the calling PC.
	// Cross-reference PC against the symbol table in xband_post.txt:
	//   _SendMessage           0xd5cba6
	//   _PUProcessSTIdle       0xd51988
	//   PUProcessIdle          0xd518d8
	//   PNetIdle               0xd51220
	//   ProcessServerData      0xd5cc99
	//   FifoRead               0xd56177
	//   TNetIdle               0xd4d3f2
	//   TIndication            0xd4cc5f
	//   TReadBytesReady        0xd4ba0b
	//   ...etc
	if (xband_bios_tx_first_used > 0)
	{
		// Identify the dominant write PC and dump a wider 128-byte
		// window of surrounding BIOS code so we can hand-disassemble
		// the loop without leaving the GUI. -64..+63 from the hot
		// PC, which gives us context both above and below.
		uint32 hot_pc = xband_bios_tx_first[0].pc;
		pos += snprintf(out + pos, out_size - pos,
			"Hot BIOS TX PC: $%06X (window: -64..+63)\n", hot_pc);
		for (int row = 0; row < 8 && pos + 80 < out_size; row++)
		{
			uint32 base = (hot_pc - 64) + row * 16;
			pos += snprintf(out + pos, out_size - pos, "  %06X: ", base);
			for (int col = 0; col < 16; col++)
			{
				uint32 a = (base & 0xFF0000) | ((base + col) & 0xFFFF);
				uint8 b = S9xGetByte(a);
				bool here = (a == hot_pc);
				pos += snprintf(out + pos, out_size - pos,
					"%s%02X%s",
					here ? "[" : "",
					(unsigned)b,
					here ? "]" : " ");
			}
			pos += snprintf(out + pos, out_size - pos, "\n");
		}
		pos += snprintf(out + pos, out_size - pos, "\n");

		// DBR + buffer snapshot at trap time. The send-buffer loop at
		// $D5:24DC reads bytes via long-indirect from DBR:$3DC3..$3DC5.
		// xband_peek_byte avoids cycle pollution; the captured snapshot
		// is the buffer state at the moment of the FIRST $90 write.
		if (xband_bios_tx_first_stack_captured)
		{
			pos += snprintf(out + pos, out_size - pos,
				"DBR at first $90 write = $%02X\n",
				(unsigned)xband_bios_tx_first_db);
			pos += snprintf(out + pos, out_size - pos,
				"Buffer base resolved at TRAP TIME = $%06X\n",
				(unsigned)xband_bios_tx_first_buffer_addr);
			pos += snprintf(out + pos, out_size - pos,
				"  -> first 64 bytes of buffer (snapshot):\n");
			for (int row = 0; row < 4 && pos + 80 < out_size; row++)
			{
				uint32 base =
					(xband_bios_tx_first_buffer_addr & 0xFF0000) |
					((xband_bios_tx_first_buffer_addr + row * 16) & 0xFFFF);
				pos += snprintf(out + pos, out_size - pos,
					"    %06X: ", base);
				for (int col = 0; col < 16; col++)
					pos += snprintf(out + pos, out_size - pos,
						"%02X ",
						(unsigned)xband_bios_tx_first_buffer[row * 16 + col]);
				pos += snprintf(out + pos, out_size - pos, " |");
				for (int col = 0; col < 16; col++)
				{
					uint8 b = xband_bios_tx_first_buffer[row * 16 + col];
					pos += snprintf(out + pos, out_size - pos, "%c",
						(b >= 0x20 && b < 0x7F) ? b : '.');
				}
				pos += snprintf(out + pos, out_size - pos, "|\n");
			}
			pos += snprintf(out + pos, out_size - pos, "\n");

			// Stack snapshot (top 64 bytes from S+1).
			pos += snprintf(out + pos, out_size - pos,
				"Stack at first $90 write (S=$%04X, top 64 bytes):\n",
				(unsigned)xband_bios_tx_first_s);
			for (int row = 0; row < 4 && pos + 80 < out_size; row++)
			{
				uint16 base =
					(uint16)(xband_bios_tx_first_s + 1 + row * 16);
				pos += snprintf(out + pos, out_size - pos,
					"  S+%02X (%04X): ", row * 16 + 1, base);
				for (int col = 0; col < 16; col++)
					pos += snprintf(out + pos, out_size - pos, "%02X ",
						xband_bios_tx_first_stack[row * 16 + col]);
				pos += snprintf(out + pos, out_size - pos, "\n");
			}
			pos += snprintf(out + pos, out_size - pos, "\n");

			// Scan the stack for plausible JSL return-address triples
			// — any (PCL, PCH, PB) where PB lands in a known BIOS code
			// bank ($D0..$DF or its $50..$5F mirror). For each hit,
			// dump 96 bytes of surrounding code so we can disassemble
			// the caller without leaving the GUI.
			pos += snprintf(out + pos, out_size - pos,
				"Plausible JSL return addresses found in stack:\n");
			int found = 0;
			for (int i = 0; i + 2 < 64 && found < 4 && pos + 200 < out_size; i++)
			{
				uint8 pcl = xband_bios_tx_first_stack[i];
				uint8 pch = xband_bios_tx_first_stack[i + 1];
				uint8 pb  = xband_bios_tx_first_stack[i + 2];
				bool plausible =
					(pb >= 0xD0 && pb <= 0xDF) ||
					(pb >= 0x50 && pb <= 0x5F);
				if (!plausible) continue;

				uint32 ret_addr = ((uint32)pb << 16)
				                | ((uint32)pch << 8)
				                | (uint32)pcl;
				uint32 jsl_addr = (ret_addr - 3) & 0xFFFFFF;
				uint32 resume_addr = (ret_addr + 1) & 0xFFFFFF;

				// Skip candidates whose marked byte isn't actually a
				// JSL opcode ($22) — those are stack noise that
				// happened to match the (PCL, PCH, PB) pattern.
				uint8 opcode_at_jsl = S9xGetByte(jsl_addr);
				bool is_real_jsl = (opcode_at_jsl == 0x22);

				pos += snprintf(out + pos, out_size - pos,
					"\n  [stack offset %d] return=$%06X (JSL @ $%06X, resume @ $%06X) %s\n",
					i, (unsigned)ret_addr,
					(unsigned)jsl_addr, (unsigned)resume_addr,
					is_real_jsl ? "<-- REAL JSL" : "(noise; not 0x22)");

				if (!is_real_jsl)
				{
					found++;
					continue;
				}

				// Hex dump 128 bytes around the JSL opcode: -96..+31.
				for (int row = 0; row < 8 && pos + 80 < out_size; row++)
				{
					uint32 base = (jsl_addr & 0xFF0000)
					            | ((jsl_addr - 96 + row * 16) & 0xFFFF);
					pos += snprintf(out + pos, out_size - pos,
						"    %06X: ", base);
					for (int col = 0; col < 16; col++)
					{
						uint32 a = (base & 0xFF0000)
						         | ((base + col) & 0xFFFF);
						uint8 b = S9xGetByte(a);
						bool here = (a == jsl_addr);
						pos += snprintf(out + pos, out_size - pos,
							"%s%02X%s",
							here ? "[" : "",
							(unsigned)b,
							here ? "]" : " ");
					}
					pos += snprintf(out + pos, out_size - pos, "\n");
				}
				found++;
			}
			if (found == 0)
			{
				pos += snprintf(out + pos, out_size - pos,
					"  (none found — stack may be deeper than 64 bytes)\n");
			}
			pos += snprintf(out + pos, out_size - pos, "\n");
		}

		pos += snprintf(out + pos, out_size - pos,
			"BIOS fred reg $90 write events (first %d, with PC + caller ret):\n",
			xband_bios_tx_first_used);
		// Compress runs of identical (pc, byte, ret) entries.
		int i = 0;
		while (i < xband_bios_tx_first_used && pos + 80 < out_size)
		{
			const XBandKCtlEntry &e = xband_bios_tx_first[i];
			uint32 ret = xband_bios_tx_first_ret[i];
			int run = 1;
			while (i + run < xband_bios_tx_first_used)
			{
				const XBandKCtlEntry &f = xband_bios_tx_first[i + run];
				if (f.pc != e.pc || f.value != e.value ||
				    xband_bios_tx_first_ret[i + run] != ret) break;
				run++;
			}
			if (run > 1)
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X ret=%06X] $90 W $%02X  x%d\n",
					(unsigned)e.pc, (unsigned)ret,
					(unsigned)e.value, run);
			else
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X ret=%06X] $90 W $%02X\n",
					(unsigned)e.pc, (unsigned)ret,
					(unsigned)e.value);
			i += run;
		}
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	if (xband_kctl_count == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"(no kill/control accesses observed)\n");
	}
	else
	{
		pos += snprintf(out + pos, out_size - pos,
			"Recent kill/ctrl entries (%d, oldest first):\n",
			xband_kctl_count);

		int n = xband_kctl_count;
		int idx = (xband_kctl_head - n + XBAND_KCTL_TRACE_SIZE)
		          % XBAND_KCTL_TRACE_SIZE;
		int i = 0;
		while (i < n && pos + 80 < out_size)
		{
			const XBandKCtlEntry &e = xband_kctl_trace[idx];
			int run = 1;
			int j = (idx + 1) % XBAND_KCTL_TRACE_SIZE;
			while (i + run < n)
			{
				const XBandKCtlEntry &f = xband_kctl_trace[j];
				if (f.address != e.address || f.value != e.value ||
				    f.is_write != e.is_write || f.pc != e.pc)
					break;
				run++;
				j = (j + 1) % XBAND_KCTL_TRACE_SIZE;
			}
			if (run > 1)
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] %s $%06X = %02X  x%d\n",
					(unsigned)e.pc,
					e.is_write ? "W" : "R",
					(unsigned)e.address,
					(unsigned)e.value, run);
			else
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] %s $%06X = %02X\n",
					(unsigned)e.pc,
					e.is_write ? "W" : "R",
					(unsigned)e.address,
					(unsigned)e.value);
			i += run;
			idx = (idx + run) % XBAND_KCTL_TRACE_SIZE;
		}
	}

	// -------------------------------------------------------------------
	// Silent-write trap section: writes to addresses where WriteMap[]
	// is MAP_NONE (i.e. the byte vanishes into thin air). The BIOS
	// shouldn't be doing this in normal operation, so any stray store
	// is a strong hint there's an unknown MMIO register at that address.
	// -------------------------------------------------------------------
	pos += snprintf(out + pos, out_size - pos,
		"\n--------------------------------------------------\n"
		"Silent writes to MAP_NONE space (total: %llu)\n"
		"--------------------------------------------------\n",
		(unsigned long long)xband_silent_total);

	if (xband_silent_total == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"(no silent writes observed -- BIOS isn't writing\n"
			" to any unmapped HiROM cart-side address)\n");
	}
	else
	{
		// Per-bank summary first.
		pos += snprintf(out + pos, out_size - pos,
			"Per-bank totals:\n");
		for (int b = 0; b < 256 && pos + 60 < out_size; b++)
		{
			if (xband_silent_perbank[b] == 0) continue;
			pos += snprintf(out + pos, out_size - pos,
				"  bank %02X: %12llu\n",
				(unsigned)b,
				(unsigned long long)xband_silent_perbank[b]);
		}

		// Then the recent ring contents (compressed runs).
		pos += snprintf(out + pos, out_size - pos,
			"Recent silent writes (%d, oldest first):\n",
			xband_silent_count);
		int n = xband_silent_count;
		int idx = (xband_silent_head - n + XBAND_SILENT_TRACE_SIZE)
		          % XBAND_SILENT_TRACE_SIZE;
		int i = 0;
		while (i < n && pos + 80 < out_size)
		{
			const XBandKCtlEntry &e = xband_silent_trace[idx];
			int run = 1;
			int j = (idx + 1) % XBAND_SILENT_TRACE_SIZE;
			while (i + run < n)
			{
				const XBandKCtlEntry &f = xband_silent_trace[j];
				if (f.address != e.address || f.value != e.value ||
				    f.pc != e.pc)
					break;
				run++;
				j = (j + 1) % XBAND_SILENT_TRACE_SIZE;
			}
			if (run > 1)
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] W $%06X = %02X  x%d\n",
					(unsigned)e.pc,
					(unsigned)e.address,
					(unsigned)e.value, run);
			else
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] W $%06X = %02X\n",
					(unsigned)e.pc,
					(unsigned)e.address,
					(unsigned)e.value);
			i += run;
			idx = (idx + run) % XBAND_SILENT_TRACE_SIZE;
		}
	}

	// -------------------------------------------------------------------
	// Cart-header read counter ($XX:$FFB0-$FFDF, broken down by bank).
	// Bumped from S9xGetByte (getset.h). Tells us whether the BIOS is
	// looking at the SNES cart header at all.
	// -------------------------------------------------------------------
	{
		extern uint64 XBandHdrReadBank[256];
		extern uint64 XBandHdrReadTotal;
		pos += snprintf(out + pos, out_size - pos,
			"\n--------------------------------------------------\n"
			"Cart-header reads $XX:$FFB0-$FFDF (total: %llu)\n"
			"--------------------------------------------------\n",
			(unsigned long long)XBandHdrReadTotal);
		if (XBandHdrReadTotal == 0)
		{
			pos += snprintf(out + pos, out_size - pos,
				"(no reads -- BIOS is NOT looking at the cart header)\n");
		}
		else
		{
			for (int b = 0; b < 256 && pos + 60 < out_size; b++)
			{
				if (XBandHdrReadBank[b] == 0) continue;
				pos += snprintf(out + pos, out_size - pos,
					"  bank %02X: %12llu reads\n",
					(unsigned)b,
					(unsigned long long)XBandHdrReadBank[b]);
			}
		}
	}

	// -------------------------------------------------------------------
	// Cross-bank reads from BIOS code into cart-side HiROM space. The
	// BIOS code lives at $D0-$DF (and the $50-$5F mirror); when it does
	// a long read into $00-$3F:$8000+, $40-$7D, $80-$BF:$8000+, or
	// $C0-$CF, it expects to see GAME bytes (which our map doesn't
	// currently expose). These are the smoking-gun reads that prove
	// where the BIOS thinks the cart should appear.
	// -------------------------------------------------------------------
	pos += snprintf(out + pos, out_size - pos,
		"\n--------------------------------------------------\n"
		"Cross-bank reads (BIOS->cart, total: %llu)\n"
		"--------------------------------------------------\n",
		(unsigned long long)xband_sread_total);

	if (xband_sread_total == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"(no cross-bank reads -- BIOS code never reaches\n"
			" into cart-side HiROM banks)\n");
	}
	else
	{
		pos += snprintf(out + pos, out_size - pos,
			"Per-target-bank totals:\n");
		for (int b = 0; b < 256 && pos + 60 < out_size; b++)
		{
			if (xband_sread_perbank[b] == 0) continue;
			pos += snprintf(out + pos, out_size - pos,
				"  bank %02X: %12llu\n",
				(unsigned)b,
				(unsigned long long)xband_sread_perbank[b]);
		}

		pos += snprintf(out + pos, out_size - pos,
			"Recent cross-bank reads (%d, oldest first):\n",
			xband_sread_count);
		int n = xband_sread_count;
		int idx = (xband_sread_head - n + XBAND_SREAD_TRACE_SIZE)
		          % XBAND_SREAD_TRACE_SIZE;
		int i = 0;
		while (i < n && pos + 80 < out_size)
		{
			const XBandKCtlEntry &e = xband_sread_trace[idx];
			int run = 1;
			int j = (idx + 1) % XBAND_SREAD_TRACE_SIZE;
			while (i + run < n)
			{
				const XBandKCtlEntry &f = xband_sread_trace[j];
				if (f.address != e.address || f.value != e.value ||
				    f.pc != e.pc)
					break;
				run++;
				j = (j + 1) % XBAND_SREAD_TRACE_SIZE;
			}
			if (run > 1)
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] R $%06X = %02X  x%d\n",
					(unsigned)e.pc,
					(unsigned)e.address,
					(unsigned)e.value, run);
			else
				pos += snprintf(out + pos, out_size - pos,
					"  [pc=%06X] R $%06X = %02X\n",
					(unsigned)e.pc,
					(unsigned)e.address,
					(unsigned)e.value);
			i += run;
			idx = (idx + run) % XBAND_SREAD_TRACE_SIZE;
		}
	}

	// -------------------------------------------------------------------
	// Per-Fred-register write counter. Lets us see WHICH Fred general
	// register or modem register the BIOS is touching, even though our
	// xband_trace ring would have evicted them by now. Reg index 0..0xBF
	// = Fred general regs, 0xC0..0xFF = Rockwell modem regs.
	// -------------------------------------------------------------------
	pos += snprintf(out + pos, out_size - pos,
		"\n--------------------------------------------------\n"
		"Fred / modem reg writes (total: %llu)\n"
		"--------------------------------------------------\n",
		(unsigned long long)xband_fred_writes_total);

	if (xband_fred_writes_total == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"(no Fred reg writes observed)\n");
	}
	else
	{
		// Show only nonzero buckets, in two columns to save space.
		int col = 0;
		for (int r = 0; r < 256 && pos + 80 < out_size; r++)
		{
			if (xband_fred_writes_by_reg[r] == 0) continue;
			const char *cls = (r < 0xC0) ? "fred" : "modm";
			unsigned mr = (r < 0xC0) ? (unsigned)r : (unsigned)(r - 0xC0);
			pos += snprintf(out + pos, out_size - pos,
				"  %s reg $%02X: %10llu%s",
				cls, mr,
				(unsigned long long)xband_fred_writes_by_reg[r],
				(col == 0) ? "    " : "\n");
			col = 1 - col;
		}
		if (col == 1)
			pos += snprintf(out + pos, out_size - pos, "\n");
	}
}

void S9xXBandDumpTrace (char *out, size_t out_size)
{
	if (!out || out_size == 0) return;
	size_t pos = 0;
	int n = xband_trace_count;
	int idx = (xband_trace_head - n + XBAND_TRACE_SIZE) % XBAND_TRACE_SIZE;

	// Compress runs of identical access+PC entries, but print the
	// calling PC so we can correlate each MMIO hit with the ROM
	// instruction that issued it.
	pos += snprintf(out + pos, out_size - pos,
		"XBAND MMIO trace (last %d accesses, oldest first):\n", n);
	int i = 0;
	while (i < n && pos + 60 < out_size)
	{
		const XBandTraceEntry &e = xband_trace[idx];
		int run = 1;
		int j = (idx + 1) % XBAND_TRACE_SIZE;
		while (i + run < n)
		{
			const XBandTraceEntry &f = xband_trace[j];
			if (f.address != e.address || f.value != e.value ||
			    f.is_write != e.is_write || f.caller_pc != e.caller_pc)
				break;
			run++;
			j = (j + 1) % XBAND_TRACE_SIZE;
		}
		if (run > 1)
			pos += snprintf(out + pos, out_size - pos,
				"  [pc=%06X] %s $%06X = %02X  x%d\n",
				(unsigned)e.caller_pc,
				e.is_write ? "W" : "R",
				(unsigned)(e.address & 0xFFFFFF),
				(unsigned)e.value, run);
		else
			pos += snprintf(out + pos, out_size - pos,
				"  [pc=%06X] %s $%06X = %02X\n",
				(unsigned)e.caller_pc,
				e.is_write ? "W" : "R",
				(unsigned)(e.address & 0xFFFFFF),
				(unsigned)e.value);
		i += run;
		idx = (idx + run) % XBAND_TRACE_SIZE;
	}
}

// -----------------------------------------------------------------------
// Internal RX/TX buffer helpers
// -----------------------------------------------------------------------
//
// We follow bsnes-plus's model: linear rxbuf/txbuf with write-position
// (rxbufpos/txbufpos) and read-position (rxbufused/txbufused) indexes.
// When the read position catches up, both get reset to zero.

static bool xband_rxbuf_has_data (void)
{
	return XBand.rxbufused < XBand.rxbufpos;
}

static uint8 xband_rxbuf_pop (void)
{
	if (!xband_rxbuf_has_data()) return 0;
	uint8 r = XBand.rxbuf[XBand.rxbufused++];
	if (XBand.rxbufused == XBand.rxbufpos)
		XBand.rxbufused = XBand.rxbufpos = 0;
	return r;
}

static void xband_txbuf_push (uint8 byte)
{
	if (XBand.txbufpos >= XBAND_TXBUF_SIZE) return; // overflow; drop
	XBand.txbuf[XBand.txbufpos++] = byte;
}

// -----------------------------------------------------------------------
// Public memory dispatch — called from getset.h via MAP_XBAND
//
// Register layout (bsnes-plus xband_support branch, xband_base.cpp):
//   addr in $FB:$C000..$FBFDFF  — Fred + modem register file, 2-byte stride
//     reg = (addr - $FBC000) / 2
//     reg $00..$BF  -> Fred general registers (regs[reg])
//     reg $C0..$FF  -> Rockwell modem (modem_regs[reg - $C0])
//   addr = $FBFE01  -> kill register
//   addr = $FBFE03  -> control register
//
// Bank $E0 is the 64KB XBAND SRAM, linear.
// -----------------------------------------------------------------------

// Return true if `addr` falls inside the XBAND SRAM mirror window
// (matches bsnes-plus's xband_base.cpp::read region match).
static inline bool xband_in_sram (uint32 addr)
{
	uint8  bank   = (addr >> 16) & 0xFF;
	uint16 offset =  addr        & 0xFFFF;
	if (bank >= 0xE0 && bank <= 0xFA) return true;
	if (bank == 0xFB && offset <= 0xBFFF) return true;
	if (bank >= 0xFC && bank <= 0xFF) return true;
	if (bank >= 0x60 && bank <= 0x7D) return true;
	return false;
}

uint8 S9xGetXBand (uint32 address)
{
	uint32 addr   = address & 0xFFFFFF;
	uint8  bank   = (addr >> 16) & 0xFF;
	uint16 offset =  addr        & 0xFFFF;
	uint8  result = 0x00;

	// XBAND SRAM mirror window (banks $E0-$FA, $FB:$0000-$BFFF,
	// $FC-$FF, $60-$7D — all aliasing the same 64KB).
	if (xband_in_sram(addr))
	{
		result = XBand.sram[offset & (XBAND_SRAM_SIZE - 1)];
	}
	// Fred + modem MMIO window $FB:$C000-$FDFF
	else if (bank == XBAND_MMIO_BANK && offset >= 0xC000 && offset < 0xFE00)
	{
		uint8 reg = (uint8)((offset - 0xC000) >> 1);

		// Fred magic constants that make the USA BIOS boot — straight
		// from bsnes-plus reset()/read():
		//   reg $7D ($FBC0FA) must return $80
		//   reg $B4 ($FBC168) must return $7F (kLEDData)
		if (reg == 0x7D)
			result = 0x80;
		else if (reg == 0xB4)
			result = 0x7F;
		else if (reg == 0x94)
		{
			// krxbuff — pop one byte from the network RX buffer
			result = xband_rxbuf_pop();
		}
		else if (reg == 0x98)
		{
			// kreadmstatus2 — "is there RX data in the Fred FIFO?"
			// Polled in tight loops inside _PUVBLCallback. bsnes-plus
			// caps consecutive "yes" responses at 127 to break infinite
			// poll loops (fixes a kFifoOverflowErr panic).
			if (XBand.net_step && xband_rxbuf_has_data())
			{
				XBand.consecutive_reads++;
				if (XBand.consecutive_reads >= 127)
				{
					XBand.consecutive_reads = 0;
					result = 0;
				}
				else
				{
					result = 1;
				}
			}
			else
			{
				XBand.consecutive_reads = 0;
				result = 0;
			}
		}
		else if (reg == 0xA0)
		{
			// Fred modem status 1 — bsnes-plus returns 0
			result = 0;
		}
		else if (reg == 0x84)
		{
			// kSStatus — smart card status. Return "card present"
			// (bit 0 = 1) so the firmware doesn't loop waiting for
			// a card insertion. Catapult's earlier xband_cart.cpp
			// dead code returned 0x01 here for the same reason.
			result = 0x01;
		}
		else if (reg >= 0xC0)
		{
			// Rockwell modem register file at modem_reg = reg - $C0
			uint8 modemreg = (uint8)(reg - 0xC0);
			uint8 ret = 0;
			switch (modemreg)
			{
				case 0x09:
					ret = XBand.modem_regs[modemreg];
					break;
				case 0x0B:
					if (XBand.modem_line_relay) ret |= (1 << 7); // TONEA
					if (XBand.modem_set_ATV25)
					{
						ret |= (1 << 4); // ATV25
						XBand.modem_set_ATV25 = 0;
					}
					break;
				case 0x0D:
					ret |= (1 << 3); // U1DET
					break;
				case 0x0E:
					ret |= 3; // k2400Baud
					break;
				case 0x0F:
					ret |= (1 << 7) | (1 << 5); // RLSD + CTS — "modem alive"
					break;
				case 0x19: // X-RAM Data
					ret = XBand.modem_regs[modemreg];
					break;
				case 0x1C:
					ret = XBand.modem_regs[0x1C];
					break;
				case 0x1D:
					ret = XBand.modem_regs[0x1D];
					break;
				case 0x1E:
					ret = XBand.modem_regs[0x1E] | (1 << 3); // TDBE (TX always empty)
					break;
				case 0x1F:
					ret = XBand.modem_regs[0x1F];
					break;
				default:
					break;
			}
			result = ret;
		}
		else
		{
			// Generic Fred register — read-as-last-written.
			result = XBand.regs[reg];
		}
	}
	// Kill / control at $FBFE01 / $FBFE03
	else if (bank == XBAND_MMIO_BANK && offset == 0xFE01)
	{
		result = XBand.kill;
		S9xXBandKCtlLog(address, result, false);
	}
	else if (bank == XBAND_MMIO_BANK && offset == 0xFE03)
	{
		result = XBand.control;
		S9xXBandKCtlLog(address, result, false);
	}

	// Everything else in the MAP_XBAND range falls through as 0. This
	// matches bsnes-plus's default behaviour for addresses outside the
	// registered ranges.

	xband_trace_log(address, result, false);
	return result;
}

void S9xSetXBand (uint8 byte, uint32 address)
{
	uint32 addr   = address & 0xFFFFFF;
	uint8  bank   = (addr >> 16) & 0xFF;
	uint16 offset =  addr        & 0xFFFF;

	xband_trace_log(address, byte, true);

	// XBAND SRAM mirror window (banks $E0-$FA, $FB:$0000-$BFFF,
	// $FC-$FF, $60-$7D — all aliasing the same 64KB).
	if (xband_in_sram(addr))
	{
		uint16 sram_off = offset & (XBAND_SRAM_SIZE - 1);
		if (XBand.sram[sram_off] != byte)
		{
			XBand.sram[sram_off] = byte;
			XBand.sram_dirty     = TRUE;
			CPU.SRAMModified     = TRUE;
		}
		return;
	}

	// Fred + modem MMIO window $FB:$C000-$FDFF
	if (bank == XBAND_MMIO_BANK && offset >= 0xC000 && offset < 0xFE00)
	{
		uint8 reg = (uint8)((offset - 0xC000) >> 1);

		// Per-Fred-register write counter for the kctl trace dump.
		// Bumped on every Fred reg write so we can see exactly which
		// registers the BIOS is touching during cart-detection time.
		S9xXBandFredRegWriteBump(reg, byte);

		// Modem TX FIFO write at Fred reg $90 ($FBC120)
		if (reg == 0x90)
		{
			if (XBand.net_step == XBAND_NET_CONNECTED ||
			    XBand.net_step == XBAND_NET_HANDSHAKE)
			{
				xband_txbuf_push(byte);
			}
			// Capture (PC, byte, caller-return) for the first N writes
			// regardless of connection state — we want to see WHICH
			// BIOS function is generating these TX bytes even if the
			// modem path isn't actively forwarding them. Memory reads
			// go through xband_peek_byte (NOT S9xGetByte) so we don't
			// add cycles to CPU.Cycles inside the trap handler.
			if (xband_bios_tx_first_used < XBAND_BIOS_TX_FIRST_SIZE)
			{
				int slot = xband_bios_tx_first_used++;
				XBandKCtlEntry &e = xband_bios_tx_first[slot];
				e.pc       = (uint32)(Registers.PBPC & 0xFFFFFF);
				e.address  = address & 0xFFFFFF;
				e.value    = byte;
				e.is_write = true;

				// Per-trap immediate caller return address. JSL pushes
				// (PB, PCH, PCL) — bytes at S+1, S+2, S+3 form the
				// return PC of the JSL that called the send-byte
				// function. Same call site → same triple every trap;
				// different call sites → different triples.
				{
					uint32 sp1 = (uint32)(Registers.S.W + 1);
					uint32 sp2 = (uint32)(Registers.S.W + 2);
					uint32 sp3 = (uint32)(Registers.S.W + 3);
					uint8 pcl = xband_peek_byte(sp1);
					uint8 pch = xband_peek_byte(sp2);
					uint8 pbr = xband_peek_byte(sp3);
					xband_bios_tx_first_ret[slot] =
						((uint32)pbr << 16)
						| ((uint32)pch << 8)
						| (uint32)pcl;
				}

				// On the very first capture, snapshot the top of the
				// stack, the data bank register, and the source buffer
				// the loop is reading from. All reads use xband_peek_byte
				// so they don't disturb CPU.Cycles or modem timing.
				if (!xband_bios_tx_first_stack_captured)
				{
					xband_bios_tx_first_s  = Registers.S.W;
					xband_bios_tx_first_db = Registers.DB;
					for (int i = 0; i < 64; i++)
					{
						uint32 sp = (uint32)(Registers.S.W + 1 + i);
						xband_bios_tx_first_stack[i] = xband_peek_byte(sp);
					}
					// Snapshot the source buffer the loop is pulling
					// bytes from. Buffer base = long pointer at
					// DBR:$3DC3..$3DC5.
					{
						uint32 ptr_addr =
							((uint32)Registers.DB << 16) | 0x3DC3;
						uint8 plo  = xband_peek_byte(ptr_addr);
						uint8 pmid = xband_peek_byte(ptr_addr + 1);
						uint8 phi  = xband_peek_byte(ptr_addr + 2);
						uint32 buf_base =
							((uint32)phi << 16)
							| ((uint32)pmid << 8)
							| plo;
						xband_bios_tx_first_buffer_addr = buf_base;
						for (int i = 0; i < 64; i++)
						{
							uint32 a = (buf_base & 0xFF0000)
							         | ((buf_base + i) & 0xFFFF);
							xband_bios_tx_first_buffer[i] = xband_peek_byte(a);
						}
					}
					xband_bios_tx_first_stack_captured = true;
				}
			}
		}

		// Rockwell modem register writes at reg $C0..$FF
		if (reg >= 0xC0)
		{
			uint8 modemreg = (uint8)(reg - 0xC0);
			switch (modemreg)
			{
				case 0x07:
					XBand.modem_line_relay = byte & 0x02;
					// If the firmware drops the line relay mid-session,
					// bsnes-plus tears down the TCP socket. We do the
					// same so a "hang up" in the UI stops the modem.
					if (XBand.modem_line_relay == 0 && XBand.net_step)
					{
						xband_sock_disconnects_by_bios++;
						S9xXBandDisconnect();
						XBand.net_step = XBAND_NET_IDLE;
						XBand.txbufpos = XBand.txbufused = 0;
						XBand.rxbufpos = XBand.rxbufused = 0;
					}
					break;
				case 0x08:
					if ((byte & 1) && XBand.net_step < XBAND_NET_HANDSHAKE)
					{
						// The firmware is raising RTS to ask the modem
						// to initiate a call. Mark the state machine
						// as pending and (if the user has previously
						// opened a connection in this session) auto-
						// reconnect using the remembered host/port
						// so the BIOS retry loop works without forcing
						// the user to re-click the Netplay menu item
						// after every "Translation problem".
						XBand.net_step = XBAND_NET_HANDSHAKE;
						xband_try_auto_reconnect();
					}
					break;
				case 0x09:
					XBand.modem_regs[modemreg] = byte;
					break;
				case 0x12:
					if (byte == 0x84)   // kV22bisMode
						XBand.modem_set_ATV25 = 1;
					break;
				case 0x1E:
					XBand.modem_regs[0x1E] = byte & 0x24; // TDBIE + RDBIE
					break;
				case 0x1F:
					XBand.modem_regs[0x1F] = byte & 0x14; // NSIE + NCIE
					break;
				default:
					XBand.modem_regs[modemreg] = byte;
					break;
			}
			return;
		}

		// Fred writes land on odd addresses only; even-address writes
		// are ignored ("event/strobe" half in the 2-byte stride).
		if (!(addr & 1))
			return;

		// Fred general register write.
		switch (reg)
		{
			case 219: // MORE_MYSTERY
			case 221: // UNKNOWN_REG
				byte &= 0x7F;
				break;
			case 223: // UNKNOWN_REG3
				byte &= 0xFE;
				break;
			default:
				break;
		}
		XBand.regs[reg] = byte;
		return;
	}

	// Kill / control at $FBFE01 / $FBFE03
	if (bank == XBAND_MMIO_BANK && offset == 0xFE01)
	{
		S9xXBandKCtlLog(address, byte, true);
		XBand.kill = byte;
		return;
	}
	if (bank == XBAND_MMIO_BANK && offset == 0xFE03)
	{
		S9xXBandKCtlLog(address, byte, true);
		XBand.control = byte;
		return;
	}
}

uint8 *S9xGetBasePointerXBand (uint32 address)
{
	if (xband_in_sram(address))
	{
		// Convention: callers compute byte = base[Address & 0xFFFF],
		// so the base pointer must equal the start of the bank's data.
		// All SRAM mirror banks alias the same 64KB.
		return XBand.sram;
	}
	// MMIO is I/O with no linear base pointer
	return NULL;
}

// -----------------------------------------------------------------------
// Fred chip patch vector application
// -----------------------------------------------------------------------
//
// bsnes-plus keeps the patch-slot fields inside the flat `regs[]` array
// at offsets 0-41 (11 slots × 4 bytes each) plus auxiliary ranges at 44+.
// We implement a thin read-side helper here for when pass-through game
// ROM mode eventually lands — for now nothing calls this because we're
// booting the BIOS standalone.

bool8 S9xXBandTryPatch (uint32 address, uint8 *out_byte)
{
	(void)address;
	(void)out_byte;
	return FALSE;
}

// -----------------------------------------------------------------------
// BIOS loading
// -----------------------------------------------------------------------

bool8 S9xLoadXBandBIOS (void)
{
	const char *candidates[] = {
		"XBAND.bios",
		"XBAND.bin",
		"xband.bios",
		"xband.bin",
		NULL
	};

	for (int i = 0; candidates[i] != NULL; i++)
	{
		std::string path = S9xGetDirectory(BIOS_DIR);
		path += SLASH_STR;
		path += candidates[i];

		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			continue;

		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (size == XBAND_ROM_SIZE)
		{
			// Load into the BIOSROM buffer maintained by the core.
			size_t r = fread(Memory.BIOSROM, 1, XBAND_ROM_SIZE, f);
			fclose(f);
			if (r == XBAND_ROM_SIZE)
			{
				XBand.bios_loaded = TRUE;
				return TRUE;
			}
			return FALSE;
		}

		fclose(f);
	}

	XBand.bios_loaded = FALSE;
	return FALSE;
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void S9xInitXBand (void)
{
	// This is called from InitROM before header parsing, so don't assume
	// Settings.XBAND is set yet. Just clear state and leave detection
	// to the ROM loader.
	memset(&XBand, 0, sizeof(XBand));
	XBand.socket_fd = XBAND_INVALID_SOCKET;
}

// External hooks into cpuexec.cpp's BRK detector so a fresh power-on
// gives us a fresh debugging snapshot.
extern uint8  XBandFirstBrkOp;
extern uint32 XBandFirstBrkPC;
extern uint16 XBandFirstBrkS;
extern bool   XBandFirstBrkSeen;

// Pre-populate XBand.sram with a saved SRAM image so the BIOS doesn't
// hang in its "first-time setup" loop on a fresh empty SRAM.
//
// We load from BIOS_DIR (the snes9x BIOS folder) rather than SRAM_DIR
// because snes9x never writes to BIOS_DIR — that means a hand-curated
// SRAM dump can sit there permanently and never get clobbered by
// snes9x's auto-save / oops-save / shutdown-save paths. The user
// drops one of the preserved Cinghialotto SNES-XBandSRAMs files into
// the BIOS dir and the BIOS picks it up on every boot.
static bool xband_load_sram_image (void)
{
	const char *candidates[] = {
		"XBAND.srm",
		"xband.srm",
		"XBAND.bin",
		"xband.bin",
		"Benner.1.SRM",
		"XBand_luke2.srm",
		"SF2DXB.S04.srm",
		NULL
	};
	FILE *f = NULL;
	for (int i = 0; candidates[i] != NULL && !f; i++)
	{
		std::string p = S9xGetDirectory(BIOS_DIR);
		p += SLASH_STR;
		p += candidates[i];
		f = fopen(p.c_str(), "rb");
	}
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	long offset = 0;
	if (sz == XBAND_SRAM_SIZE + 512) offset = 512; // strip copier header
	else if (sz != XBAND_SRAM_SIZE)
	{
		fclose(f);
		return false;
	}
	if (offset) fseek(f, offset, SEEK_SET);
	size_t r = fread(XBand.sram, 1, XBAND_SRAM_SIZE, f);
	fclose(f);
	return (r == XBAND_SRAM_SIZE);
}

void S9xXBandSyncSRAMOut (void)
{
	// Mirror our XBand.sram[] back into Memory.SRAM[] so snes9x's
	// standard SaveSRAM picks up the latest XBAND SRAM contents when
	// the user closes the emulator or auto-saves.
	if (Settings.XBAND)
		memcpy(Memory.SRAM, XBand.sram, XBAND_SRAM_SIZE);
}

void S9xResetXBand (void)
{
	// Reset the BRK / COP debugging flag so each power-on captures a
	// fresh first-trap snapshot.
	XBandFirstBrkOp   = 0;
	XBandFirstBrkPC   = 0;
	XBandFirstBrkS    = 0;
	XBandFirstBrkSeen = false;

	// Power-on values for the Fred + modem register files, copied from
	// bsnes-plus xband_base.cpp reset(). Without these, the XBAND USA
	// BIOS triggers a BRK panic handler during init.
	memset(XBand.regs, 0, sizeof(XBand.regs));
	memset(XBand.modem_regs, 0, sizeof(XBand.modem_regs));

	XBand.regs[0x7C] = 0;      // kAddrStatus
	XBand.regs[0x7D] = 0x80;   // read-constant, also seeded here
	XBand.regs[0xB4] = 0x7F;   // kLEDData
	XBand.regs[222]  = 8;      // UNKNOWN_REG2

	// From the Catapult _PUResetModem routine.
	XBand.modem_regs[0x19] = 0x46;

	XBand.kill    = 0;
	XBand.control = 0;

	XBand.modem_line_relay  = 0;
	XBand.modem_set_ATV25   = 0;
	XBand.net_step          = XBAND_NET_IDLE;
	XBand.consecutive_reads = 0;

	XBand.rxbufpos = XBand.rxbufused = 0;
	XBand.txbufpos = XBand.txbufused = 0;

	// Try to load a real XBAND SRAM dump (e.g. one of the dumps in the
	// Cinghialotto repo's SNES-XBandSRAMs.rar). On a fresh "first-time
	// setup" boot, the BIOS spins forever in init because nothing in
	// zeroed SRAM matches the magic boot vector / box ID it expects.
	// A real SRAM dump bypasses that hang.
	if (xband_load_sram_image())
		XBand.sram_dirty = FALSE;
}

void S9xXBandPostLoadState (void)
{
	// Nothing to re-derive; the register files live entirely inside
	// the serialized struct. Don't touch the socket — a save state
	// load implicitly "hangs up" the modem, same as loading a state
	// during snes9x netplay.
	XBand.socket_fd = XBAND_INVALID_SOCKET;
	XBand.connected = FALSE;
}

// -----------------------------------------------------------------------
// Network bridging — TCP socket shuttle
// -----------------------------------------------------------------------

// The XBAND server expects emulator clients to identify themselves with
// a magic prefix string before any ADSP traffic. Without this, the
// server doesn't know how to talk to us and the BIOS sees garbage,
// triggering the "Translation problem. Dialing XBAND again..." retry
// loop. The exact string is the one bsnes-plus sends in its
// xband_base.cpp::xband_send_identity (see fresh-eggs/bsnes-plus
// xband_support branch). The fixed ID `Waj04qaASNfmaRNw` is what
// bsnes-plus's release branch ships — the server is happy with it.
// xband_identity_sends counter is declared above near the kctl trace
// state so the dump function can reference it.
static void xband_send_identity (xband_sock_t fd)
{
	static const char IDENTITY[] = "///////EMU-Waj04qaASNfmaRNw\x0a";
	const int len = (int)(sizeof(IDENTITY) - 1);
	int sent = (int)send(fd, IDENTITY, len, 0);
	if (sent == len)
		xband_identity_sends++;
	// If the send failed (e.g. socket buffer full on first hit),
	// we'll retry on the next poll cycle since the state hasn't
	// advanced yet.
}

static bool xband_set_nonblocking (xband_sock_t fd)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return false;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool8 S9xXBandConnect (const char *host, int port)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
		S9xXBandDisconnect();

#ifdef _WIN32
	if (!s_winsock_inited)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return FALSE;
		s_winsock_inited = true;
	}
#endif

	struct addrinfo hints;
	struct addrinfo *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	if (getaddrinfo(host, port_str, &hints, &result) != 0 || !result)
		return FALSE;

	xband_sock_t fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if ((intptr_t)fd == XBAND_INVALID_SOCKET)
	{
		freeaddrinfo(result);
		return FALSE;
	}

	if (connect(fd, result->ai_addr, (socklen_t)result->ai_addrlen) == XBAND_SOCKET_ERROR)
	{
		XBAND_CLOSESOCKET(fd);
		freeaddrinfo(result);
		return FALSE;
	}

	freeaddrinfo(result);

	// Disable Nagle — low-latency input exchange matters
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	xband_set_nonblocking(fd);

	XBand.socket_fd = (intptr_t)fd;
	XBand.connected = TRUE;

	// Send identity proactively immediately after connect, then drop
	// straight to CONNECTED so BIOS TX bytes flush on the very next
	// poll cycle. We CAN'T stay in HANDSHAKE waiting for server data
	// to "prove" the link is up, because the HELO\n filter discards
	// every probe byte before it reaches XBand.rxbuf — that means
	// the on-first-RX transition in S9xXBandPoll would never fire
	// against a pure-HELO server, and BIOS TX bytes would be stuck
	// in xband_txbuf forever.
	xband_send_identity(fd);
	XBand.net_step = XBAND_NET_CONNECTED;

	// Reset the HELO filter state so a previous attempt's partial
	// match doesn't bleed into this connection.
	xband_helo_match_pos = 0;

	// Remember host/port so the BIOS retry loop can auto-reconnect.
	strncpy(xband_last_host, host, sizeof(xband_last_host) - 1);
	xband_last_host[sizeof(xband_last_host) - 1] = 0;
	xband_last_port = port;

	return TRUE;
}

void S9xXBandSetHeloFilter (bool on)
{
	xband_helo_filter_enabled = on;
	xband_helo_match_pos = 0;
}

bool S9xXBandGetHeloFilter (void)
{
	return xband_helo_filter_enabled;
}

// Auto-reconnect: re-open the socket using the host/port the user
// originally clicked Connect with. Called from the modem reg $07/$08
// write path when the BIOS asserts RTS again after a previous hangup.
// No-op if there's no remembered host (user never clicked Connect)
// or the socket is already open.
static void xband_try_auto_reconnect (void)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET) return;
	if (xband_last_host[0] == 0)                 return;
	if (S9xXBandConnect(xband_last_host, xband_last_port))
		xband_auto_reconnects++;
}

void S9xXBandDisconnect (void)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
	{
		XBAND_CLOSESOCKET(XBand.socket_fd);
		XBand.socket_fd = XBAND_INVALID_SOCKET;
	}
	XBand.connected = FALSE;
	XBand.net_step  = XBAND_NET_IDLE;
	XBand.rxbufpos  = XBand.rxbufused = 0;
	XBand.txbufpos  = XBand.txbufused = 0;
}

void S9xXBandPoll (void)
{
	if (XBand.socket_fd == XBAND_INVALID_SOCKET)
		return;

	xband_sock_t fd = (xband_sock_t)XBand.socket_fd;

	// ---- ADSP handshake ----
	//
	// The XBAND server expects an emulator-client to identify itself
	// with a fixed magic string immediately after the first server
	// bytes arrive. Without that, the server doesn't know what to do
	// with the connection and the BIOS shows "Translation problem.
	// Dialing XBAND again..." in a retry loop.
	//
	// We mirror bsnes-plus's flow exactly:
	//   1. user clicks Connect → S9xXBandConnect sets net_step = HANDSHAKE
	//   2. server eventually sends a banner / prompt → we read it
	//   3. on the very first poll where bytes arrive AND we're still in
	//      HANDSHAKE, we ship the identity string, flush any buffered
	//      BIOS TX (the firmware may have written some bytes before
	//      the socket was usable), and advance to CONNECTED
	//   4. from then on it's a straight byte pipe
	//
	// Importantly: BIOS TX bytes that arrive while the state is still
	// HANDSHAKE are HELD (not flushed) so they go out AFTER identity,
	// matching bsnes-plus's order. Otherwise the server would see the
	// BIOS's first ADSP packet before our identity prefix and treat
	// the connection as garbage.

	// 1) Drain any bytes the server has sent into the RX buffer.
	//    HELO\n softmodem probes are stripped here so the BIOS only
	//    sees "real" payload bytes — see comment on xband_helo_*.
	uint32 rx_before = XBand.rxbufpos;
	while (XBand.rxbufpos < XBAND_RXBUF_SIZE)
	{
		uint8 b;
		int got = (int)recv(fd, (char *)&b, 1, 0);
		if (got == 1)
		{
			xband_sock_rx_bytes++;
			if (xband_sock_rx_first_used < XBAND_SOCK_FIRST_SIZE)
				xband_sock_rx_first[xband_sock_rx_first_used++] = b;

			// ADSP frame detector — runs on every raw RX byte BEFORE
			// the HELO filter and BEFORE pushing to BIOS rxbuf. Tells
			// us the moment the server transitions from HELO probes
			// to real ADSP frames.
			xband_adsp_feed_byte(b);

			// HELO\n filter (can be bypassed at runtime). When the
			// filter is disabled the byte goes straight into rxbuf,
			// just like any other unrecognized byte.
			if (xband_helo_filter_enabled)
			{
				if (b == (uint8)xband_helo_signature[xband_helo_match_pos])
				{
					xband_helo_match_pos++;
					if (xband_helo_match_pos == (int)sizeof(xband_helo_signature))
					{
						// Full HELO\n match — discard all 5 bytes.
						xband_helo_discarded++;
						xband_helo_match_pos = 0;
					}
					continue;
				}

				// Partial match followed by a non-matching byte —
				// flush what we provisionally absorbed back into
				// rxbuf, then handle the current byte normally.
				for (int i = 0; i < xband_helo_match_pos &&
				                XBand.rxbufpos < XBAND_RXBUF_SIZE; i++)
					XBand.rxbuf[XBand.rxbufpos++] = (uint8)xband_helo_signature[i];
				xband_helo_match_pos = 0;
			}

			if (XBand.rxbufpos < XBAND_RXBUF_SIZE)
				XBand.rxbuf[XBand.rxbufpos++] = b;
		}
		else if (got == 0)
		{
			// Clean server-side close. Latch the flag so the kctl
			// dump shows it; the BIOS will eventually notice the
			// modem is dead and drop the line relay on its own.
			xband_sock_eof_seen = true;
			break;
		}
		else
		{
			break; // EAGAIN / would-block
		}
	}

	// 2) First server bytes have arrived while still in handshake —
	//    send the identity prefix and advance the state machine.
	if (XBand.net_step == XBAND_NET_HANDSHAKE && XBand.rxbufpos > rx_before)
	{
		xband_send_identity(fd);
		XBand.net_step = XBAND_NET_CONNECTED;
	}

	// 3) Flush BIOS TX bytes per dreampi-style framing.
	//
	// dreampi (netlink.py xband_server) accumulates modem-side bytes
	// into a `line` buffer until it sees the ADSP frame end marker
	// `\x10\x03`, then sends the entire frame as one socket write.
	// Bytes after the last `\x10\x03` stay buffered for the next
	// frame. This atomic per-packet send is what the server expects;
	// fragmented byte-by-byte sends apparently confuse it.
	//
	// Safety valve: if the txbuf grows past XBAND_TXBUF_SIZE/2 with
	// no frame end marker in sight, fall back to a raw byte flush.
	// That keeps unframed pre-ADSP bytes (e.g. modem auto-baud `\r`
	// sequences the BIOS sends before entering data mode) from
	// accumulating forever in the buffer.
	if (XBand.net_step == XBAND_NET_CONNECTED)
	{
		while (XBand.txbufused < XBand.txbufpos)
		{
			// Find the next \x10\x03 frame end starting from
			// txbufused. Substring search like dreampi does.
			uint32 start = XBand.txbufused;
			uint32 end   = 0;  // 0 == "no end marker found"
			for (uint32 i = start; i + 1 < XBand.txbufpos; i++)
			{
				if (XBand.txbuf[i] == 0x10 && XBand.txbuf[i + 1] == 0x03)
				{
					end = i + 2;  // include the \x10\x03 in the send
					break;
				}
			}

			uint32 unsent = XBand.txbufpos - start;

			if (end == 0)
			{
				// No complete frame in the buffer. If we're past
				// the safety threshold, flush whatever's there
				// raw — better to send something than block the
				// BIOS forever waiting for a marker that may
				// never come.
				if (unsent < XBAND_TXBUF_SIZE / 2)
					break;
				end = XBand.txbufpos;  // flush all
			}

			uint32 frame_len = end - start;
			bool   was_real_frame = (XBand.txbuf[end - 2] == 0x10 &&
			                          XBand.txbuf[end - 1] == 0x03);
			int sent = (int)send(fd,
				(const char *)(XBand.txbuf + start),
				(int)frame_len, 0);
			if (sent <= 0)
				break;  // socket would block or broken

			// Capture into the TX-first ring (up to limit).
			for (int i = 0;
			     i < sent && xband_sock_tx_first_used < XBAND_SOCK_FIRST_SIZE;
			     i++)
				xband_sock_tx_first[xband_sock_tx_first_used++] =
					XBand.txbuf[start + i];

			XBand.txbufused += sent;
			xband_sock_tx_bytes += sent;

			// Partial send? Stop here and retry on next poll.
			if ((uint32)sent < frame_len)
				break;

			// Only count "real" \x10\x03-terminated frames; the
			// safety-valve raw flushes don't have that suffix and
			// shouldn't inflate the frame count.
			if (was_real_frame)
				xband_tx_frames_sent++;
		}
		// If buffer is fully drained, reset positions to start.
		if (XBand.txbufused >= XBand.txbufpos)
			XBand.txbufpos = XBand.txbufused = 0;
	}
}
