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
 * xbserver.retrocomputing.network).
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
  #include <sys/file.h>
  typedef int xband_sock_t;
  #define XBAND_CLOSESOCKET(s)	close((int)(s))
  #define XBAND_INVALID_SOCKET	(-1)
  #define XBAND_SOCKET_ERROR	(-1)
#endif

#include "snes9x.h"
#include "memmap.h"
#include "dsp.h"
#include "fxemu.h"
#include "fscompat.h"
#include "xband.h"
#include "ppu.h"
#include "display.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <chrono>
#include <ctime>

// A hung-up peer must not kill the process with SIGPIPE where that exists.
#ifdef MSG_NOSIGNAL
  #define XBAND_SEND_FLAGS	MSG_NOSIGNAL
#else
  #define XBAND_SEND_FLAGS	0
#endif

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

// BIOS RX consumption counter — bumped when xband_rxbuf_pop returns
// a real byte (i.e., the BIOS read a non-empty rxbuf via fred $94).
// Tells us whether the BIOS is actually receiving the bytes we forward.
static uint64 xband_rxbuf_bytes_consumed = 0;

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

// First-N captured TX frames — parallel to xband_adsp_first_frame[]
// but for the BIOS-side stream. Captured at TX-flush time when we
// successfully send a complete \x10\x03 frame to the socket.
static uint8  xband_adsp_first_tx_frame[XBAND_ADSP_FIRST_FRAMES][256];
static int    xband_adsp_first_tx_frame_len[XBAND_ADSP_FIRST_FRAMES] = {0};
static int    xband_adsp_first_tx_frame_count = 0;

// TX frame counter — bumped each time the TX accumulator successfully
// sends a complete `\x10\x03`-terminated packet to the socket.
// (Distinct from xband_sock_tx_bytes which counts individual bytes.)
static uint64 xband_tx_frames_sent = 0;

// ----------------------------------------------------------------------
// ServerTalk message dispatcher state
// ----------------------------------------------------------------------
//
// ADSP frame layout (after deframing — leading \x00 stripped, trailing
// \x10\x03 stripped, byte-stuffing decoded). Per the bsnes-plus debug
// dumper (xband_base_GAMEPLAY.cpp print_adsp_debug_in):
//
//   bytes [0..1]    Source ConnID         (uint16, big-endian)
//   bytes [2..5]    PktFirstByteSeq       (uint32, big-endian)
//   bytes [6..9]    PktNextRecvSeq        (uint32, big-endian)
//   bytes [10..11]  PktRecvWindow         (uint16, big-endian)
//   byte  [12]      ADSP descriptor       (control flags / ack-req etc.)
//   byte  [13]      ServerTalk opcode     (first data byte, if any)
//   bytes [14..n-3] ServerTalk payload    (variable)
//   bytes [n-2..n-1] CRC-16               (CCITT-FALSE w/ leading \x00)
//
// Minimum data-carrying frame size is 16 bytes (13 header + 1 opcode +
// 0 data + 2 CRC). A frame of exactly 15 bytes is a header-only ADSP
// control segment with no ServerTalk payload (e.g. an ack of a sequence
// number, or a window update). Less than 15 bytes is malformed and
// shouldn't reach the validate-frame path.
#define XBAND_ADSP_HEADER_LEN 13

struct XBandParsedFrame
{
	uint16 source_conn_id;
	uint32 first_byte_seq;
	uint32 next_recv_seq;
	uint16 recv_window;
	uint8  descriptor;
	bool   has_opcode;        // false for header-only ADSP control frames
	uint8  opcode;            // ServerTalk opcode (only if has_opcode)
	const uint8 *payload;     // ServerTalk payload bytes (after opcode)
	int    payload_len;       // bytes available between opcode and CRC
};

// Per-opcode RX/TX counters. Index 0 covers "no opcode" (header-only
// ADSP control frames); indices 1..255 are direct opcode lookups.
// Bumped from xband_servertalk_dispatch_rx / _tx for every good frame.
#define XBAND_SERVERTALK_OPCODES 256
static uint64 xband_servertalk_rx_count[XBAND_SERVERTALK_OPCODES] = {0};
static uint64 xband_servertalk_tx_count[XBAND_SERVERTALK_OPCODES] = {0};
// Distinct count of header-only RX frames (ADSP acks / window updates).
// These don't carry a ServerTalk opcode but tell us the server is still
// driving the session forward, so they're worth counting separately.
static uint64 xband_servertalk_rx_headeronly = 0;
static uint64 xband_servertalk_tx_headeronly = 0;

// Last decoded ServerTalk message text. Captured by the dispatcher for
// known opcodes (msNewNGPList, msSetDateAndTime, msSetCurrentUserName,
// etc.). The dump prints these alongside the per-frame hex view so we
// can read off what the server is actually sending. Most recent N
// messages, ring buffer.
#define XBAND_SERVERTALK_DECODED_LOG 16
struct XBandDecodedMessage
{
	uint8  opcode;
	bool   is_tx;             // direction
	int    payload_len;       // bytes excluding opcode itself
	char   text[160];         // human-readable summary
};
static XBandDecodedMessage xband_servertalk_decoded[XBAND_SERVERTALK_DECODED_LOG];
static int xband_servertalk_decoded_head  = 0;
static int xband_servertalk_decoded_count = 0;

// ----------------------------------------------------------------------
// ServerTalk stream reassembly
// ----------------------------------------------------------------------
//
// Per-direction byte stream accumulator. Every good-CRC ADSP segment
// appends its data section (offsets 13..len-3 of the deframed body) to
// the stream. This gives us a contiguous view of the ServerTalk byte
// stream as the BIOS / server sees it -- the same view xbsega.go's
// `bytes.IndexByte(rx_buffer, msBoxType)` operates on. From here we
// can scan for opcodes by byte search and decode known message types
// without worrying about ADSP segment boundaries.
//
// Buffers are 8KB each, ring-style: once full, the oldest bytes get
// overwritten and the read offset advances. The dump shows the most
// recent N bytes plus a list of opcode positions found via byte
// search.
#define XBAND_STREAM_BUF_SIZE 8192
static uint8  xband_rx_stream[XBAND_STREAM_BUF_SIZE];
static uint32 xband_rx_stream_pos    = 0;   // total bytes ever appended
static uint32 xband_rx_stream_dropped = 0;  // bytes evicted by ring wrap
static uint8  xband_tx_stream[XBAND_STREAM_BUF_SIZE];
static uint32 xband_tx_stream_pos    = 0;
static uint32 xband_tx_stream_dropped = 0;

// ----------------------------------------------------------------------
// Sniffed ADSP connection state -- used by the fake-server injector
// ----------------------------------------------------------------------
//
// To inject a valid server reply we need ADSP fields the BIOS will
// accept: the ConnID it's expecting from the server, the next sequence
// number it expects to receive (its "PktNextRecvSeq"), and the most
// recent send-seq the BIOS used (so our reply can ack it). All of
// these are taken from the actual frames flowing across the live
// connection -- the dispatcher updates these every time it parses a
// good frame.
//
// "tx_*" tracks the BIOS's outgoing frames: their connID is the BOX
// connID, their first_byte_seq is the box's send sequence (the byte
// position of the first data byte in the segment), their next_recv_seq
// is the byte the box expects next from the server.
//
// "rx_*" tracks the SERVER's outgoing frames: connID is the SERVER
// connID, first_byte_seq is the server's send sequence.
//
// When we inject a fake reply we use:
//   source_conn_id = sniffed_server_conn_id  (so it looks like the
//                    same server connection)
//   first_byte_seq = sniffed_server_send_seq + sniffed_server_data_so_far
//                    (where the server would be in its send stream)
//   next_recv_seq  = sniffed_box_send_seq + sniffed_box_data_so_far
//                    (acknowledging everything the box has sent)
//   recv_window    = a generous value, e.g. 0x0400 = 1024 bytes
//   descriptor     = $20 (EOM bit set) so the BIOS treats the message
//                    as complete
static uint16 xband_sniff_box_conn_id    = 0;
static uint32 xband_sniff_box_first_seq  = 0;
static uint32 xband_sniff_box_next_recv  = 0;
static uint16 xband_sniff_box_recv_win   = 0;
static uint32 xband_sniff_box_data_total = 0; // sum of all data byte lengths sent
static bool   xband_sniff_box_seen       = false;

static uint16 xband_sniff_srv_conn_id    = 0;
static uint32 xband_sniff_srv_first_seq  = 0;
static uint32 xband_sniff_srv_next_recv  = 0;
static uint16 xband_sniff_srv_recv_win   = 0;
static uint32 xband_sniff_srv_data_total = 0;
static bool   xband_sniff_srv_seen       = false;

// Counter / status for the injected fakes (for the kctl dump).
static uint32 xband_fake_inject_count = 0;
static char   xband_fake_inject_last[128] = "(none)";

// Running send_seq for our fake server output. Initialized lazily on
// the first inject from the box's next_recv_seq (which tells us
// authoritatively where the box thinks the server is in the byte
// stream). Each subsequent inject advances this by the data length
// it sent. Reset on connect/disconnect/SNES reset.
static uint32 xband_fake_send_seq         = 0;
static bool   xband_fake_send_seq_primed  = false;

// Fred mode switches since power-on, and the last one, for the trace dump.
static uint32 xband_fred_switches = 0;
static char   xband_fred_last[64] = "(none)";

// BIOS firmware scan results. Populated at multi-cart load time by
// kDispatcherVector logger. Every XBAND OS function call goes through
// `JSL $E0:$0040` with the function ID in the X register. Per the
// fresh-eggs xband_post writeup:
//   #define kDispatcherVector at $E0:$0040
//   LDX #funcID ; JSL $E0:$0040
// We trap CPU execution at PBPC == $00E00040 in cpuexec.cpp's main
// loop and call S9xXBandLogDispatcherCall() with the caller PC (read
// from the return address on the stack), the function ID (X), and the
// accumulator (A). This lets us see exactly which OS functions get
// invoked when the user clicks "Challenge" -- including DBGetItem
// ($D0:$7145) which is our prime suspect for the game-check call.
struct XBandDispatchEntry
{
	uint32 caller;          // 24-bit address of last byte of the JSL
	uint16 func_id;         // X register (function ID)
	uint16 a_first;         // A register at FIRST occurrence
	uint16 a_last;          // A register at MOST RECENT occurrence
	uint32 hits;            // total times this (caller, funcID) seen
	uint64 first_seen_call; // global call sequence number when first seen
};
// 1024 unique call sites should cover the entire BIOS boot. Each
// (caller, funcID) is stored ONCE; subsequent calls bump the hit
// counter. This way idle loops don't flood the buffer -- we see one
// entry per unique call site in chronological order of first
// occurrence.
#define XBAND_DISPATCH_LOG_SIZE 1024
static XBandDispatchEntry xband_dispatch_log[XBAND_DISPATCH_LOG_SIZE];
static uint32 xband_dispatch_log_count = 0;   // unique entries stored
static uint64 xband_dispatch_total_calls = 0; // total raw calls
static uint32 xband_dispatch_overflow = 0;    // unique sites we couldn't store
// Per-funcID counter for the most-called function IDs.
#define XBAND_DISPATCH_FUNCID_BUCKETS 0x400
static uint64 xband_dispatch_funcid_count[XBAND_DISPATCH_FUNCID_BUCKETS] = {0};

void S9xXBandResetDispatcherLog ()
{
	xband_dispatch_log_count = 0;
	xband_dispatch_total_calls = 0;
	xband_dispatch_overflow = 0;
	memset(xband_dispatch_funcid_count, 0,
	       sizeof(xband_dispatch_funcid_count));
}

void S9xXBandLogDispatcherCall (uint32 caller, uint16 func_id, uint16 a)
{
	xband_dispatch_total_calls++;
	if (func_id < XBAND_DISPATCH_FUNCID_BUCKETS)
		xband_dispatch_funcid_count[func_id]++;

	// Linear search for an existing matching (caller, funcID) entry.
	// 1024 entries * 57k calls = ~58M comparisons; well under a
	// second. We could use a hash table for speed but boot completes
	// quickly enough that this is acceptable for diagnostic use.
	for (uint32 i = 0; i < xband_dispatch_log_count; i++)
	{
		XBandDispatchEntry *e = &xband_dispatch_log[i];
		if (e->caller == caller && e->func_id == func_id)
		{
			if (e->hits < 0xFFFFFFFFu)
				e->hits++;
			e->a_last = a;
			return;
		}
	}

	// New unique call site -- append in first-seen order.
	if (xband_dispatch_log_count >= XBAND_DISPATCH_LOG_SIZE)
	{
		xband_dispatch_overflow++;
		return;
	}
	XBandDispatchEntry *e = &xband_dispatch_log[xband_dispatch_log_count++];
	e->caller = caller;
	e->func_id = func_id;
	e->a_first = a;
	e->a_last = a;
	e->hits = 1;
	e->first_seen_call = xband_dispatch_total_calls;
}

// Read interceptor PC log. When the BIOS reads from $7F:$0C8B-$0C8E
// and the spoofer is enabled, the read interceptor in getset.h logs
// the calling PC here so we can see WHICH BIOS instructions read the
// cart-id cache. Once we have those PCs, we can disassemble the
// surrounding bytes in the BIOS image to find the supported-games
// table comparison and patch it.
//
// The trap also snapshots a 32-byte window of memory around the PC
// at the moment of the trap (so the snapshot reflects the actual
// instruction bytes, not whatever WRAM looks like later when the
// user runs the search) plus the A/X/Y/DBR/D registers (so we know
// what data the instruction was processing).
#define XBAND_CARTID_PC_LOG_SIZE 32
struct XBandCartIDReadEntry {
	uint32 pc;        // PB:PC of the instruction reading the cart-id
	uint8  byte_off;  // 0..3 — which byte of the cart-id was read
	uint8  byte_val;  // value the interceptor returned

	// Snapshot of CPU state at trap time -- captures the in-flight
	// data so we can identify what the instruction is doing.
	uint16 reg_a;
	uint16 reg_x;
	uint16 reg_y;
	uint16 reg_d;
	uint8  reg_db;
	uint8  reg_p;     // M/X/I/C/etc. flags

	// 32-byte snapshot of memory around the PC. The byte AT the PC
	// is at offset 16 in the array (so we capture 16 bytes BEFORE
	// the PC and 16 bytes AFTER). Lets us reconstruct the actual
	// instruction stream regardless of later WRAM reuse.
	uint8  pc_bytes[32];
};
static XBandCartIDReadEntry xband_cartid_read_pc_log[XBAND_CARTID_PC_LOG_SIZE];
static uint32 xband_cartid_read_pc_count = 0;

// Snapshot the trap-time CPU state and 32 bytes of memory around the
// PC into a log entry. We read directly from Memory.RAM (for the
// low WRAM mirror at bank $00:0000-$1FFF) or from Memory.ROM (for
// BIOS bank $D0-$DF in multicart mode), bypassing the memory map
// dispatch entirely so we don't disturb CPU.Cycles or take a
// dependency on the function definition order.
static void xband_cartid_snapshot (XBandCartIDReadEntry *e, uint32 pc)
{
	e->reg_a  = (uint16)(Registers.A.W);
	e->reg_x  = (uint16)(Registers.X.W);
	e->reg_y  = (uint16)(Registers.Y.W);
	e->reg_d  = (uint16)(Registers.D.W);
	e->reg_db = Registers.DB;
	e->reg_p  = Registers.PL;

	// Capture 32 bytes centered at PC: 16 before, 16 after.
	uint16 lo = (uint16)(pc & 0xFFFF);
	uint8  pb = (uint8)((pc >> 16) & 0xFF);
	int center = 16;
	for (int i = 0; i < 32; i++)
	{
		int off = (int)lo - center + i;
		if (off < 0 || off > 0xFFFF)
		{
			e->pc_bytes[i] = 0;
			continue;
		}
		// Bank 0 low addresses ($0000-$1FFF) mirror $7E:$0000-$1FFF
		// in WRAM. Read directly from Memory.RAM.
		if (pb == 0 && off <= 0x1FFF && Memory.RAM)
		{
			e->pc_bytes[i] = Memory.RAM[off];
		}
		// Bank $7E or $7F: direct WRAM access.
		else if ((pb == 0x7E || pb == 0x7F) && Memory.RAM)
		{
			int linear = ((pb - 0x7E) << 16) | off;
			if (linear < 0x20000)
				e->pc_bytes[i] = Memory.RAM[linear];
			else
				e->pc_bytes[i] = 0;
		}
		// Multicart BIOS bank ($D0-$DF): firmware lives at
		// Memory.ROM + Multi.cartOffsetA, mapped HiROM-style with
		// 1MB mirror.
		else if (pb >= 0xD0 && pb <= 0xDF &&
		         Multi.cartType == 6 && Memory.ROM)
		{
			uint32 bios_off = ((uint32)(pb - 0xD0) << 16) | off;
			bios_off &= (XBAND_ROM_SIZE - 1);
			e->pc_bytes[i] = Memory.ROM[Multi.cartOffsetA + bios_off];
		}
		else
		{
			e->pc_bytes[i] = 0;
		}
	}
}

// Called from getset.h S9xGetByte when the read interceptor fires.
// Records the PC + offset + returned value + snapshot of CPU state
// and surrounding memory into the ring buffer.
void S9xXBandLogCartIDRead (uint32 pc, int byte_off, uint8 byte_val)
{
	if (xband_cartid_read_pc_count >= XBAND_CARTID_PC_LOG_SIZE)
	{
		xband_cartid_read_pc_count++;
		return;
	}
	XBandCartIDReadEntry *e =
		&xband_cartid_read_pc_log[xband_cartid_read_pc_count++];
	e->pc       = pc & 0xFFFFFF;
	e->byte_off = (uint8)byte_off;
	e->byte_val = byte_val;
	xband_cartid_snapshot(e, pc);
}

// Separate ring for cart-id WRITES. The writer is whoever computes
// the cart-id and stores it at $7F:$0C8B. Finding this PC gives us
// the cart-id computation function in the BIOS, which is much more
// useful than the consumer PCs we already capture.
static XBandCartIDReadEntry xband_cartid_write_pc_log[XBAND_CARTID_PC_LOG_SIZE];
static uint32 xband_cartid_write_pc_count = 0;

void S9xXBandLogCartIDWrite (uint32 pc, int byte_off, uint8 byte_val)
{
	if (xband_cartid_write_pc_count >= XBAND_CARTID_PC_LOG_SIZE)
	{
		xband_cartid_write_pc_count++;
		return;
	}
	XBandCartIDReadEntry *e =
		&xband_cartid_write_pc_log[xband_cartid_write_pc_count++];
	e->pc       = pc & 0xFFFFFF;
	e->byte_off = (uint8)byte_off;
	e->byte_val = byte_val;
	xband_cartid_snapshot(e, pc);
}

// Runtime spoof value for the cart-id read interceptor. Can be
// changed at runtime via S9xXBandCycleSpoofValue() so the user can
// brute-force through candidate cart-ids without rebuilding.
//
// Default = D8 22 21 03 (xbsega SSF2 Japan). Cycling advances
// through a list of known/candidate values from the patches we
// have access to plus a few computed guesses.
static uint8 xband_spoof_value[4] = { 0xD8, 0x22, 0x21, 0x03 };
static int   xband_spoof_value_idx = 0;

struct XBandSpoofCandidate {
	uint8 bytes[4];
	const char *label;
};

// Candidate cart-ids to try. Include the values we have from
// xbsega.go and a few likely guesses.
static const XBandSpoofCandidate xband_spoof_candidates[] = {
	{ { 0xD8, 0x22, 0x21, 0x03 }, "SSF2 Japan (xbsega JSNES)" },
	{ { 0xEF, 0x12, 0x0A, 0x61 }, "SSF2 (xbsega comment)" },
	{ { 0x4D, 0x1C, 0x4E, 0x1D }, "SSF2 Sega" },
	{ { 0xC4, 0xCD, 0xDF, 0x0C }, "MK2 Sega" },
	{ { 0xC0, 0x43, 0x21, 0x72 }, "MK2 SNES (commented)" },
	{ { 0xE3, 0x0C, 0x29, 0x6E }, "NBA JAM Sega" },
	{ { 0x8F, 0x6B, 0x9F, 0x70 }, "NHL95 Sega" },
	{ { 0xAB, 0x63, 0x48, 0xE9 }, "MK Sega" },
	{ { 0x31, 0xED, 0x81, 0x23 }, "Madden95 Sega" },
	{ { 0x12, 0x7E, 0x81, 0x81 }, "NHL95 SNES (commented)" },
	{ { 0x19, 0x69, 0xD2, 0xAF }, "NBA JAM TE SNES" },
	{ { 0x3D, 0x1C, 0x44, 0xEB }, "Super Mario Kart SNES" },
	{ { 0x05, 0x48, 0x49, 0x71 }, "MK3 SNES" },
	{ { 0x94, 0xB5, 0x64, 0xB5 }, "DOOM SNES" },
	{ { 0x2D, 0x17, 0xC0, 0x45 }, "Killer Instinct SNES" },
	{ { 0x83, 0xE6, 0x27, 0xEF }, "Kirby SNES" },
	{ { 0xA8, 0x97, 0x3C, 0x8C }, "Ken Griffey SNES" },
	{ { 0x00, 0x00, 0x00, 0x00 }, "all zeros" },
	{ { 0xFF, 0xFF, 0xFF, 0xFF }, "all ones" },
};

#define XBAND_SPOOF_CANDIDATES_COUNT \
	(sizeof(xband_spoof_candidates) / sizeof(xband_spoof_candidates[0]))

void S9xXBandCycleSpoofValue (void)
{
	xband_spoof_value_idx =
		(xband_spoof_value_idx + 1) % XBAND_SPOOF_CANDIDATES_COUNT;
	memcpy(xband_spoof_value,
	       xband_spoof_candidates[xband_spoof_value_idx].bytes, 4);
}

bool S9xXBandSetSpoofValueByIndex (int idx)
{
	if (idx < 0 || idx >= (int)XBAND_SPOOF_CANDIDATES_COUNT)
		return false;
	xband_spoof_value_idx = idx;
	memcpy(xband_spoof_value,
	       xband_spoof_candidates[idx].bytes, 4);
	return true;
}

int S9xXBandGetSpoofValueCount (void)
{
	return (int)XBAND_SPOOF_CANDIDATES_COUNT;
}

const char *S9xXBandGetSpoofValueLabelAt (int idx)
{
	if (idx < 0 || idx >= (int)XBAND_SPOOF_CANDIDATES_COUNT)
		return "?";
	return xband_spoof_candidates[idx].label;
}

const uint8 *S9xXBandGetSpoofValueBytesAt (int idx)
{
	if (idx < 0 || idx >= (int)XBAND_SPOOF_CANDIDATES_COUNT)
		return NULL;
	return xband_spoof_candidates[idx].bytes;
}

const char *S9xXBandSpoofValueLabel (void)
{
	return xband_spoof_candidates[xband_spoof_value_idx].label;
}

const uint8 *S9xXBandSpoofValueBytes (void)
{
	return xband_spoof_value;
}

// Runtime toggle for the TX GameID spoofer. When ON, every outgoing
// ADSP frame is parsed in xband_tx_rewrite_gameid and any byte
// sequence matching `$0C $F7 $2B $5D $1A` (msGAMEIDAndPatchVersion
// followed by the BIOS's broken default cart hash) is replaced with
// `$0C $d8 $22 $21 $03` (SSF2 Japan's expected GameID per
// xbsega.go). The frame's CRC is recomputed over the modified body
// before sending. This makes the server (and our fake-server
// injects) think the box has SSF2 Japan loaded even though the
// BIOS's own cart-detection produces garbage.
//
// OFF by default. Enable from the menu after the BIOS has reached
// the main menu and you're ready to click Challenge.
static bool   xband_tx_gameid_spoof = false;
static uint32 xband_tx_gameid_spoof_count = 0;
static char   xband_tx_gameid_spoof_last[96] = "(none)";

// ConnID source for the fake-server injector. There are two possible
// interpretations of the SNES XBAND ADSP layer:
//   - "live server" mode: server-originated frames carry the SERVER's
//     source connID ($0539 in the test session). This is what real
//     Apple ADSP does -- each end has its own connID.
//   - "echo box" mode: the open-connection-ack echoes the BOX's frame
//     back with only the descriptor flipped (per the xbsega.go
//     reference, line 234-238), preserving the BOX's source connID.
//     If the SNES BIOS expects all server frames to use the BOX's
//     connID (because that's what was in the open-conn-ack), our
//     "live server" connID will be rejected.
// Toggleable via menu so we can A/B test which one the BIOS accepts.
// Default = LIVE_SRV per Apple ADSP standard.
enum {
	XBAND_FAKE_CONNID_LIVE_SRV = 0,  // sniffed server source connID
	XBAND_FAKE_CONNID_BOX      = 1,  // sniffed box source connID
};
static int xband_fake_connid_source = XBAND_FAKE_CONNID_LIVE_SRV;

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

// Local switchboard (Emulation -> XBAND -> Local Server): between calls each
// window keeps an idle line to the server so an opponent's call can ring it
// (RI, modem $0F bit 3); answering adopts that line, which the server relays.
static intptr_t xband_ring_fd      = XBAND_INVALID_SOCKET;
static bool     xband_ringing      = false;
static bool     xband_answered     = false;	// this call was answered, not dialed
static bool     xband_far_end_up   = true;	// a local dial rings out until the far end speaks
static bool     xband_call_local   = false;	// the call up now went to the local server
static bool     xband_dial_done    = true;	// the answer tone ended; until then HELO\n probes are dropped
static char     xband_ring_line[16];
static int      xband_ring_len     = 0;
static uint32   xband_ring_retry   = 0;		// frames until the next ring-line attempt
static bool     xband_ring_connecting = false;	// the ring line's connect is still in progress
static bool     xband_connecting   = false;	// the dial's connect is still in progress
static uint32   xband_frame        = 0;		// S9xXBandPoll calls (one per frame)
static uint32   xband_atv25_until  = 0;		// the answer tone sounds until this frame; 0 = not started

// The answer tone is a level held this long, as the real V.25 tone lasts ~3 s: the dialing BIOS reads
// $0B for ATV25 and again for TONEA each pass, and a one-shot taken by the TONEA read was lost.
#define XBAND_ATV25_FRAMES 30

static bool xband_local_switch (void)
{
	return Settings.XBANDLocalServer && Settings.XBANDServerHost[0] && Settings.XBANDServerPort;
}

// The server the next dial goes to.
static const char *xband_server (int *port)
{
	if (xband_local_switch())
	{
		*port = (int) Settings.XBANDServerPort;
		return Settings.XBANDServerHost;
	}
	*port = 56969;
	return "xbserver.retrocomputing.network";
}

static bool xband_ring_answer (void);
static void xband_hang_up (void);
static bool xband_console_reset = false;	// S9xResetXBand from the box's own /RESET

static bool xband_seed_sram     = false;	// load the SRAM dump on the next reset
static bool xband_reset_pending = false;	// the BIOS pulled /RESET via the LEDs
static uint32 xband_bios_resets = 0, xband_bios_reset_pc = 0;

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
//
// `addr_base` is added to each row label so callers can show absolute
// stream offsets when only dumping a slice. Pass 0 if dumping from
// the beginning of `data`.
static size_t xband_hex_ascii_dump_at (char *out, size_t out_size,
                                       const uint8 *data, int n,
                                       uint32 addr_base)
{
	size_t pos = 0;
	for (int row = 0; row < n && pos + 80 < out_size; row += 16)
	{
		pos += snprintf(out + pos, out_size - pos, "  %04X: ",
			(unsigned)(addr_base + row));
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

// Backwards-compat wrapper -- starts addresses at 0.
static size_t xband_hex_ascii_dump (char *out, size_t out_size,
                                    const uint8 *data, int n)
{
	return xband_hex_ascii_dump_at(out, out_size, data, n, 0);
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

// ServerTalk opcode → name lookup. There are TWO distinct opcode
// enums in XBAND:
//
//   - SERVER → BOX  (received by the SNES BIOS): opcodes from
//     xband_post.txt enum -- msReceive*, msNew*, msSet*, msExecute*.
//     These tell the box what to do or hand it data the server
//     wants persisted (game patches, news, mail, etc.).
//
//   - BOX → SERVER  (sent by the SNES BIOS): opcodes from xbsega.go
//     -- msLogin, msSystemVersion, msBoxType, msSendGameResults, etc.
//     These are how the box reports its state and answers server
//     queries during the login handshake.
//
// The two enums overlap in numeric range (server-side $01..$47,
// box-side $0B..$27), so you MUST know the direction to label an
// opcode correctly. The lookup helpers below take an `is_tx` flag.

static const char *xband_servertalk_name_server_to_box (uint8 opcode)
{
	switch (opcode)
	{
	// $01 = kFirstServerMessage is a SENTINEL constant from
	// xband_post.txt (lower bound for valid msg IDs in
	// _ReceiveServerMessageDispatch). It's not a real message --
	// flag it so the scanner doesn't get fooled by stray $01 bytes
	// in payload data.
	case 1:  return "(kFirstServerMessage sentinel)";
	case 2:  return "msEndOfStream";
	case 3:  return "msGamePatch";
	case 4:  return "msSetDateAndTime";
	case 5:  return "msServerMiscControl";
	case 9:  return "msExecuteCode";
	case 10: return "msPatchOSCode";
	case 12: return "msRemoveDBTypeOpCode";
	case 13: return "msRemoveMessageHandler";
	case 14: return "msRegisterPlayer";
	case 15: return "msNewNGPList";
	case 16: return "msSetBoxSerialNumber";
	case 17: return "msGetTypeIDsFromDB";
	case 18: return "msAddItemToDB";
	case 19: return "msDeleteItemFromDB";
	case 20: return "msGetItemFromDB";
	case 21: return "msGetFirstItemIDFromDB";
	case 22: return "msGetNextItemIDFromDB";
	case 23: return "msClearSendQ";
	case 27: return "msLoopBack";
	case 28: return "msWaitForOpponent";
	case 29: return "msOpponentPhoneNumber";
	case 30: return "msReceiveMail";
	case 31: return "msNewsHeader";
	case 32: return "msNewsPage";
	case 34: return "msQDefDialog";
	case 35: return "msAddAddressBookEntry";
	case 36: return "msDeleteAddressBookEntry";
	case 37: return "msReceiveRanking";
	case 38: return "msDeleteRanking";
	case 39: return "msGetNumRankings";
	case 40: return "msGetFirstRankingID";
	case 41: return "msGetNextRankingID";
	case 42: return "msGetRankingData";
	case 43: return "msSetBoxPhoneNumber";
	case 44: return "msSetLocalAccessPhoneNumber";
	case 45: return "msSetConstants";
	case 46: return "msReceiveValidPers";
	case 47: return "msGetInvalidPers";
	case 49: return "msCorrelateAddressBookEntry";
	case 50: return "msReceiveWriteableString";
	case 51: return "msReceiveCredit";
	case 52: return "msReceiveRestrictions";
	case 53: return "msReceiveCreditToken";
	case 54: return "msSetCurrentUserName";
	case 56: return "msSetBoxHometown";
	case 57: return "msGetConstant";
	case 58: return "msReceiveProblemToken";
	case 59: return "msReceiveValidationToken";
	case 60: return "msLiveDebitSmartCard";
	case 61: return "msSendDialScript";
	case 62: return "msSetCurrentUserNumber";
	case 63: return "msBoxWipeMind";
	case 64: return "msGetHiddenSerials";
	case 66: return "msGetLoadedGameInfo";
	case 67: return "msClearNetOpponent";
	case 68: return "msGetBoxMemStats";
	case 69: return "msReceiveRentalSerialNumber";
	case 70: return "msReceiveNewsIndex";
	case 71: return "msReceiveBoxNastyLong";
	default: return "?";
	}
}

// Box → server opcodes from xbsega.go (Genesis xbsega reference). The
// SNES BIOS uses the same enum -- xbsega processes packet dumps from
// both Genesis and SNES boxes via the same byte-search parser.
static const char *xband_servertalk_name_box_to_server (uint8 opcode)
{
	switch (opcode)
	{
	case 0x02: return "msEndOfStream";       // shared with server enum
	case 0x0B: return "msLogin";
	case 0x0C: return "msGAMEIDAndPatchVersion";
	case 0x0E: return "msChallengeRequest";
	case 0x0F: return "msSystemVersion";
	case 0x10: return "msSendNGPVersion";
	case 0x11: return "msDBIDInfo";
	case 0x12: return "msSendItemFromDB";
	case 0x13: return "msSendFirstItemID";
	case 0x14: return "msSendNextItemID";
	case 0x15: return "msSendSendQElements";
	case 0x16: return "msSendAddressesToVerify";
	case 0x17: return "msSendNumRankings";
	case 0x18: return "msSendFirstRankingID";
	case 0x19: return "msSendNextRankingID";
	case 0x1A: return "msSendRankingData";
	case 0x1B: return "msSendInvalidPers";
	case 0x1D: return "msSendOutgoingMail";
	case 0x1E: return "msSendCreditDebitInfo";
	case 0x1F: return "msBoxType";
	case 0x20: return "msSendGameResults";
	case 0x21: return "msSendNoGameResults";
	case 0x22: return "msSendConstant";
	case 0x23: return "msSendGameErrorResults";
	case 0x24: return "msSendNoGameErrorResults";
	case 0x25: return "msSendNetErrors";
	case 0x26: return "msNoNetErrors";
	case 0x27: return "msSendHiddenSerials";
	default:   return "?";
	}
}

// Direction-aware opcode lookup. Use this whenever you have an opcode
// AND know which direction it came from -- the dump tables, the
// counter rows, the decoded message log all need to be direction-aware
// because $20 means very different things in each direction
// (msNewsPage server->box vs msSendGameResults box->server).
static const char *xband_servertalk_name_dir (uint8 opcode, bool is_tx)
{
	return is_tx
		? xband_servertalk_name_box_to_server(opcode)
		: xband_servertalk_name_server_to_box(opcode);
}

// Parse a deframed ADSP body (the same shape stored in
// xband_adsp_first_frame[] / xband_adsp_first_tx_frame[]) into the
// 13-byte ADSP header + ServerTalk opcode + payload split. The body
// is expected to still have the trailing 2 CRC bytes; those are
// excluded from out->payload_len.
//
// Returns false for malformed bodies (too short to even hold the ADSP
// header + CRC). For header-only frames (length == 13 + 2 = 15), out
// is filled with has_opcode=false; the caller can still bump the
// header-only counter and look at the descriptor byte.
static bool xband_servertalk_parse (const uint8 *body, int body_len,
                                    XBandParsedFrame *out)
{
	if (!out || !body) return false;
	memset(out, 0, sizeof(*out));

	// Need at least 13 header + 2 CRC = 15 bytes.
	if (body_len < XBAND_ADSP_HEADER_LEN + 2)
		return false;

	out->source_conn_id =
		((uint16)body[0] << 8) | body[1];
	out->first_byte_seq =
		((uint32)body[2] << 24) | ((uint32)body[3] << 16) |
		((uint32)body[4] << 8)  |  (uint32)body[5];
	out->next_recv_seq =
		((uint32)body[6] << 24) | ((uint32)body[7] << 16) |
		((uint32)body[8] << 8)  |  (uint32)body[9];
	out->recv_window =
		((uint16)body[10] << 8) | body[11];
	out->descriptor = body[12];

	int data_start = XBAND_ADSP_HEADER_LEN;          // 13
	int data_end   = body_len - 2;                   // exclude CRC

	if (data_end > data_start)
	{
		out->has_opcode  = true;
		out->opcode      = body[data_start];
		out->payload     = body + data_start + 1;
		out->payload_len = data_end - data_start - 1;
	}
	else
	{
		out->has_opcode  = false;
		out->opcode      = 0;
		out->payload     = NULL;
		out->payload_len = 0;
	}
	return true;
}

// Append one entry to the decoded-message ring. The ring is meant for
// human inspection in the dump, not for replay — entries get evicted
// once XBAND_SERVERTALK_DECODED_LOG is full.
static void xband_servertalk_log_decoded (uint8 opcode, bool is_tx,
                                          int payload_len,
                                          const char *fmt, ...)
{
	XBandDecodedMessage *e =
		&xband_servertalk_decoded[xband_servertalk_decoded_head];
	xband_servertalk_decoded_head =
		(xband_servertalk_decoded_head + 1) % XBAND_SERVERTALK_DECODED_LOG;
	if (xband_servertalk_decoded_count < XBAND_SERVERTALK_DECODED_LOG)
		xband_servertalk_decoded_count++;

	e->opcode      = opcode;
	e->is_tx       = is_tx;
	e->payload_len = payload_len;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
}

// Read a big-endian uint32 from payload at offset, with bounds check.
// Returns 0 on out-of-range — the formatted decode text shows the raw
// length so the user can spot truncation.
static uint32 xband_be32 (const uint8 *p, int len, int off)
{
	if (off + 4 > len) return 0;
	return ((uint32)p[off] << 24) | ((uint32)p[off+1] << 16) |
	       ((uint32)p[off+2] << 8) |  (uint32)p[off+3];
}
static uint16 xband_be16 (const uint8 *p, int len, int off)
{
	if (off + 2 > len) return 0;
	return ((uint16)p[off] << 8) | p[off+1];
}

// Copy a fixed-length null-padded ASCII string from payload into a
// caller buffer, replacing non-printable bytes with '.'. Used for the
// 34-byte hometown / username fields per sample_packets.txt.
static void xband_copy_padded (char *dst, size_t dst_size,
                               const uint8 *p, int len, int off, int field_len)
{
	size_t out = 0;
	if (dst_size == 0) return;
	for (int i = 0; i < field_len && off + i < len && out + 1 < dst_size; i++)
	{
		uint8 c = p[off + i];
		if (c == 0) break;
		dst[out++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	dst[out] = '\0';
}

// Per-opcode decoder. Format known ServerTalk messages into a
// human-readable summary and append to the decoded log. Unknown
// opcodes get a generic "(opcode $XX, N bytes payload)" entry. Both
// directions go through this — set is_tx for BIOS->server frames.
//
// IMPORTANT CAVEAT: ServerTalk runs as a STREAM over ADSP. ADSP can
// fragment a single ServerTalk message across multiple segments. We
// only see the byte at the start of each segment's data section --
// for the FIRST segment of a message that's the opcode, but for
// CONTINUATION segments it's just the next data byte (which will
// usually look like garbage when fed to a switch on opcode values).
// The xbsega.go reference reassembles all incoming segments into one
// big buffer and scans for opcode bytes to find them. We will need to
// do the same to get reliable per-message decoding -- this per-segment
// decoder is best-effort and only labels the FIRST byte as an opcode.
static void xband_servertalk_decode (const XBandParsedFrame *p, bool is_tx)
{
	if (!p->has_opcode)
	{
		// Header-only ADSP control segment — ack/window/etc. Skip the
		// decoded log to avoid drowning out the data frames.
		return;
	}

	uint8 op = p->opcode;
	const uint8 *d = p->payload;
	int n = p->payload_len;

	// TX-side decoders: box-to-server messages, sourced from xbsega.go
	// and the SSR-side handlers. Most of these are short fixed-format
	// or counter-style messages so we can decode them straight from
	// the segment without needing stream reassembly.
	if (is_tx)
	{
		switch (op)
		{
		case 0x02: // msEndOfStream — terminator, no payload
			xband_servertalk_log_decoded(op, is_tx, n, "msEndOfStream");
			break;

		case 0x0B: // msLogin
			xband_servertalk_log_decoded(op, is_tx, n,
				"msLogin (login state, %d bytes)", n);
			break;

		case 0x0C: // msGAMEIDAndPatchVersion
			// 4-byte gameID + patch version. Cart fingerprint.
			if (n >= 4)
				xband_servertalk_log_decoded(op, is_tx, n,
					"msGAMEIDAndPatchVersion gameID=$%08X (+%d more bytes)",
					xband_be32(d, n, 0), n - 4);
			else
				xband_servertalk_log_decoded(op, is_tx, n,
					"msGAMEIDAndPatchVersion (truncated, %d bytes)", n);
			break;

		case 0x0E: // msChallengeRequest
			xband_servertalk_log_decoded(op, is_tx, n,
				"msChallengeRequest (%d bytes)", n);
			break;

		case 0x0F: // msSystemVersion
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSystemVersion (%d bytes)", n);
			break;

		case 0x10: // msSendNGPVersion — uint16 version
			if (n >= 2)
				xband_servertalk_log_decoded(op, is_tx, n,
					"msSendNGPVersion ver=$%04X", xband_be16(d, n, 0));
			else
				xband_servertalk_log_decoded(op, is_tx, n,
					"msSendNGPVersion (truncated)");
			break;

		case 0x1B: // msSendInvalidPers — password / personalization
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendInvalidPers (%d bytes -- password block)", n);
			break;

		case 0x1F: // msBoxType — 4-byte ASCII box type ("sn07" for SNES)
		{
			char tag[8] = "";
			xband_copy_padded(tag, sizeof(tag), d, n, 0, 4);
			xband_servertalk_log_decoded(op, is_tx, n,
				"msBoxType '%s'", tag);
			break;
		}

		case 0x20: // msSendGameResults
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendGameResults (%d bytes)", n);
			break;

		case 0x21: // msSendNoGameResults
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendNoGameResults");
			break;

		case 0x22: // msSendConstant
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendConstant (%d bytes)", n);
			break;

		case 0x25: // msSendNetErrors
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendNetErrors (%d bytes)", n);
			break;

		case 0x26: // msNoNetErrors — opcode only
			xband_servertalk_log_decoded(op, is_tx, n, "msNoNetErrors");
			break;

		case 0x27: // msSendHiddenSerials
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSendHiddenSerials (%d bytes)", n);
			break;

		default:
			xband_servertalk_log_decoded(op, is_tx, n,
				"%s (op $%02X, %d bytes payload) [TX continuation?]",
				xband_servertalk_name_box_to_server(op),
				(unsigned)op, n);
			break;
		}
		return;
	}

	// RX-side decoders: server-to-box messages, sourced from
	// xband_post.txt and sample_packets.txt.
	switch (op)
	{
	case 2:  // msEndOfStream
		xband_servertalk_log_decoded(op, is_tx, n, "msEndOfStream");
		break;

	case 4:  // msSetDateAndTime
		// Per sample_packets.txt: 4 bytes date + 5? bytes time.
		// (Sample shows 04 000059C3 000031DC02.) Render both raw.
		if (n >= 4)
		{
			uint32 date = xband_be32(d, n, 0);
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetDateAndTime date=$%08X (raw, %d bytes payload)",
				date, n);
		}
		else
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetDateAndTime (truncated, %d bytes)", n);
		break;

	case 14: // msRegisterPlayer
		// Per sample_packets.txt: 4-byte wait time.
		if (n >= 4)
			xband_servertalk_log_decoded(op, is_tx, n,
				"msRegisterPlayer wait=%u", xband_be32(d, n, 0));
		else
			xband_servertalk_log_decoded(op, is_tx, n,
				"msRegisterPlayer (truncated)");
		break;

	case 15: // msNewNGPList — game list
		// 0F 0010 0001 0009 C4CDDF0C 00000000 00000003 0010 4D6F7274616C204B6F6D626174203200
		// length(2) count(2) version(2) gameID(4) gameflags(4) patchver(4) titleLen(2) title(N)
		if (n >= 6)
		{
			uint16 length      = xband_be16(d, n, 0);
			uint16 count       = xband_be16(d, n, 2);
			uint16 list_ver    = xband_be16(d, n, 4);
			char title[40] = "";
			uint32 game_id = 0, gameflags = 0, patchver = 0;
			if (n >= 22)
			{
				game_id   = xband_be32(d, n, 6);
				gameflags = xband_be32(d, n, 10);
				patchver  = xband_be32(d, n, 14);
				uint16 title_len = xband_be16(d, n, 18);
				if (title_len > 0)
					xband_copy_padded(title, sizeof(title),
						d, n, 20, title_len);
			}
			xband_servertalk_log_decoded(op, is_tx, n,
				"msNewNGPList len=%u count=%u ver=%u game=$%08X title='%s'",
				length, count, list_ver, game_id, title);
		}
		else
			xband_servertalk_log_decoded(op, is_tx, n,
				"msNewNGPList (truncated, %d bytes)", n);
		break;

	case 16: // msSetBoxSerialNumber
		if (n >= 8)
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetBoxSerialNumber region=$%08X serial=$%08X",
				xband_be32(d, n, 0), xband_be32(d, n, 4));
		else
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetBoxSerialNumber (truncated, %d bytes)", n);
		break;

	case 23: // msClearSendQ — opcode only
		xband_servertalk_log_decoded(op, is_tx, n, "msClearSendQ");
		break;

	case 27: // msLoopBack
		xband_servertalk_log_decoded(op, is_tx, n,
			"msLoopBack (%d bytes payload)", n);
		break;

	case 43: // msSetBoxPhoneNumber
	{
		// 2B 00 00 35313238363735333039 00...  ←  DBID + padding + 24 bytes phone
		char phone[32] = "";
		if (n >= 4)
			xband_copy_padded(phone, sizeof(phone), d, n, 3, 24);
		xband_servertalk_log_decoded(op, is_tx, n,
			"msSetBoxPhoneNumber phone='%s'", phone);
		break;
	}

	case 54: // msSetCurrentUserName — opcode + 34 bytes name
	{
		char name[40] = "";
		xband_copy_padded(name, sizeof(name), d, n, 0, 34);
		xband_servertalk_log_decoded(op, is_tx, n,
			"msSetCurrentUserName name='%s'", name);
		break;
	}

	case 56: // msSetBoxHometown — opcode + 34 bytes town
	{
		char town[40] = "";
		xband_copy_padded(town, sizeof(town), d, n, 0, 34);
		xband_servertalk_log_decoded(op, is_tx, n,
			"msSetBoxHometown town='%s'", town);
		break;
	}

	case 58: // msReceiveProblemToken
		xband_servertalk_log_decoded(op, is_tx, n,
			"msReceiveProblemToken (%d bytes)", n);
		break;

	case 59: // msReceiveValidationToken
		xband_servertalk_log_decoded(op, is_tx, n,
			"msReceiveValidationToken (%d bytes)", n);
		break;

	case 62: // msSetCurrentUserNumber
		if (n >= 1)
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetCurrentUserNumber user=%u", (unsigned)d[0]);
		else
			xband_servertalk_log_decoded(op, is_tx, n,
				"msSetCurrentUserNumber (truncated)");
		break;

	default:
		xband_servertalk_log_decoded(op, is_tx, n,
			"%s (op $%02X, %d bytes payload) [RX continuation?]",
			xband_servertalk_name_server_to_box(op),
			(unsigned)op, n);
		break;
	}
}

// Reverse byte-stuffing on a buffer in place: every `\x10\x10` pair
// collapses to a single `\x10`. Returns the new length. Used by the
// TX stream path because the TX capture stores wire bytes (the BIOS
// produces already-stuffed output) -- we want the stream view to
// match the RX view, which is logical (de-stuffed) bytes.
static int xband_destuff_inplace (uint8 *buf, int len)
{
	int o = 0;
	for (int i = 0; i < len; i++)
	{
		if (i + 1 < len && buf[i] == 0x10 && buf[i + 1] == 0x10)
		{
			buf[o++] = 0x10;
			i++;  // skip the second \x10
		}
		else
		{
			buf[o++] = buf[i];
		}
	}
	return o;
}

// Append the data section of a parsed ADSP segment to the per-direction
// stream accumulator. Ring-buffer behaviour: once full, older bytes get
// shifted out (the buffer always holds the MOST RECENT
// XBAND_STREAM_BUF_SIZE bytes of the conversation).
static void xband_stream_append (uint8 *stream, uint32 *stream_pos,
                                 uint32 *stream_dropped,
                                 const uint8 *data, int data_len)
{
	if (!data || data_len <= 0) return;

	// Tail pointer = (stream_pos - dropped) within the buffer.
	uint32 used = *stream_pos - *stream_dropped;
	if (used > XBAND_STREAM_BUF_SIZE) used = XBAND_STREAM_BUF_SIZE;

	// If the new chunk would overflow, shift the existing window left
	// to make room. This is O(N) on append but we only run a few
	// thousand bytes per session, so cost is negligible.
	if (used + (uint32)data_len > XBAND_STREAM_BUF_SIZE)
	{
		uint32 keep = XBAND_STREAM_BUF_SIZE - (uint32)data_len;
		if (keep > used) keep = used;
		uint32 shift = used - keep;
		if (shift > 0 && keep > 0)
			memmove(stream, stream + shift, keep);
		*stream_dropped += shift;
		used = keep;
	}

	int copy_len = data_len;
	if (copy_len > XBAND_STREAM_BUF_SIZE)
	{
		// Single chunk bigger than the whole buffer -- only keep the
		// tail end so the overall window stays the most recent.
		*stream_dropped += copy_len - XBAND_STREAM_BUF_SIZE;
		data += copy_len - XBAND_STREAM_BUF_SIZE;
		copy_len = XBAND_STREAM_BUF_SIZE;
		used = 0;
	}

	memcpy(stream + used, data, copy_len);
	*stream_pos += data_len;
}

// Returns true if the parsed frame is an ADSP control packet (its
// descriptor has bit 7 set). Control packets carry connection
// management info -- open-conn req/ack, attention, retransmit advice,
// forward reset -- and DO NOT advance the data byte stream. Counting
// their body bytes toward data_total breaks the send_seq math used by
// the fake-server injector.
static inline bool xband_adsp_is_control (const XBandParsedFrame *p)
{
	return (p->descriptor & 0x80) != 0;
}

// Receive-side dispatcher: called from xband_adsp_validate_frame for
// every good-CRC frame. Bumps per-opcode counter, appends segment data
// to the RX stream (for data packets only), and runs the per-segment
// decoder. Does NOT inject any reply — actual ServerTalk message
// processing is done by the BIOS itself (we just emulate the bus).
// The decoded log and stream view are for our visibility, not for
// protocol participation.
static void xband_servertalk_dispatch_rx (const uint8 *body, int body_len)
{
	XBandParsedFrame p;
	if (!xband_servertalk_parse(body, body_len, &p))
		return;

	bool is_control = xband_adsp_is_control(&p);

	if (p.has_opcode && !is_control)
		xband_servertalk_rx_count[p.opcode]++;
	else
		xband_servertalk_rx_headeronly++;

	// Snoop the SERVER's ADSP connection state for the fake-server
	// injector. ConnID + first_seq + next_recv update on EVERY frame
	// (including control packets) since the connection identity comes
	// from there. data_total only advances for DATA packets -- control
	// packet body bytes don't count toward the ADSP data stream and
	// counting them poisons our send_seq computation.
	xband_sniff_srv_conn_id   = p.source_conn_id;
	xband_sniff_srv_first_seq = p.first_byte_seq;
	xband_sniff_srv_next_recv = p.next_recv_seq;
	xband_sniff_srv_recv_win  = p.recv_window;
	if (!is_control)
	{
		int data_off = XBAND_ADSP_HEADER_LEN;
		int data_end = body_len - 2;
		if (data_end > data_off)
			xband_sniff_srv_data_total += (uint32)(data_end - data_off);
	}
	xband_sniff_srv_seen = true;

	// Append the segment's data section (everything between the ADSP
	// header and the trailing CRC) to the RX stream accumulator -- but
	// only for non-control segments. Control packets contain ADSP
	// management bytes that pollute the stream view of ServerTalk.
	if (!is_control)
	{
		int data_off = XBAND_ADSP_HEADER_LEN;
		int data_end = body_len - 2;  // exclude CRC
		if (data_end > data_off)
		{
			xband_stream_append(xband_rx_stream,
			                    &xband_rx_stream_pos,
			                    &xband_rx_stream_dropped,
			                    body + data_off, data_end - data_off);
		}
	}

	xband_servertalk_decode(&p, false /*is_tx*/);
}

// Send-side dispatcher: called from the TX-frame-capture path so we
// also see what the BIOS is sending. The TX body comes from the BIOS
// in already-byte-stuffed form (we just forward what the BIOS wrote
// to fred reg $90 to the socket), so we need to de-stuff it locally
// before parsing to get the same logical view the RX path produces.
static void xband_servertalk_dispatch_tx (const uint8 *body, int body_len)
{
	// Local de-stuff buffer. 256 is enough for typical ADSP segments
	// (~125 bytes wire) and we cap input length to match the capture
	// arrays.
	uint8 dest[256];
	int n = body_len;
	if (n > (int)sizeof(dest)) n = (int)sizeof(dest);
	memcpy(dest, body, n);
	int destuffed_len = xband_destuff_inplace(dest, n);

	XBandParsedFrame p;
	if (!xband_servertalk_parse(dest, destuffed_len, &p))
		return;

	bool is_control = xband_adsp_is_control(&p);

	if (p.has_opcode && !is_control)
		xband_servertalk_tx_count[p.opcode]++;
	else
		xband_servertalk_tx_headeronly++;

	// Snoop the BOX's ADSP connection state -- needed by the
	// fake-server injector to ack the box's data correctly. data_total
	// only advances for DATA packets, same reasoning as the RX side.
	xband_sniff_box_conn_id   = p.source_conn_id;
	xband_sniff_box_first_seq = p.first_byte_seq;
	xband_sniff_box_next_recv = p.next_recv_seq;
	xband_sniff_box_recv_win  = p.recv_window;
	if (!is_control)
	{
		int data_off2 = XBAND_ADSP_HEADER_LEN;
		int data_end2 = destuffed_len - 2;
		if (data_end2 > data_off2)
			xband_sniff_box_data_total += (uint32)(data_end2 - data_off2);
	}
	xband_sniff_box_seen = true;

	if (!is_control)
	{
		int data_off = XBAND_ADSP_HEADER_LEN;
		int data_end = destuffed_len - 2;
		if (data_end > data_off)
		{
			xband_stream_append(xband_tx_stream,
			                    &xband_tx_stream_pos,
			                    &xband_tx_stream_dropped,
			                    dest + data_off, data_end - data_off);
		}
	}

	xband_servertalk_decode(&p, true /*is_tx*/);
}

// Heuristic: should we treat this byte as a likely opcode candidate?
// We filter out the ASCII printable range $20..$7E because most opcode
// false positives in the byte search come from text fields (usernames,
// email subjects, hometown). msBoxType happens to be $1F (just below
// space) and the box-side opcodes cluster in $0B..$27, so the
// $20..$7E filter only loses msSendGameResults ($20), msSendNoGameResults
// ($21), msSendConstant ($22), msSendGameErrorResults ($23),
// msSendNoGameErrorResults ($24), msSendNetErrors ($25), msNoNetErrors
// ($26), msSendHiddenSerials ($27) -- but those are all "stats reply"
// messages that show up rarely and would be drowned in noise anyway.
// Better to lose those signals than report 100 spurious hits per dump.
//
// We also keep $02 (msEndOfStream) since it's a critical anchor for
// finding message boundaries.
static bool xband_stream_byte_is_candidate_opcode (uint8 b, bool is_tx)
{
	// Always-keep anchors:
	if (b == 0x02) return true;          // msEndOfStream
	if (b == 0x1F) return true;          // msBoxType (TX) / msNewsHeader (RX)

	// Filter ASCII range to avoid text-field false positives.
	if (b >= 0x20 && b < 0x7F) return false;

	// For the rest, only keep bytes that map to a non-"?" name in the
	// relevant enum. This excludes high values like $E8 / $BB / $FF
	// which are continuation noise.
	const char *name = is_tx
		? xband_servertalk_name_box_to_server(b)
		: xband_servertalk_name_server_to_box(b);
	return name[0] != '?';
}

// Walk the reassembled ServerTalk byte stream looking for known opcodes
// using the same byte-search approach as xbsega.go. The result is a
// best-effort opcode location list -- byte search will always have
// false positives when payload data contains values that look like
// opcodes. We filter out ASCII printables and unknown values to keep
// the noise down.
//
// We also use $02 (msEndOfStream) as a message-boundary anchor: each
// occurrence likely separates two ServerTalk messages.
//
// Returns the number of bytes consumed by the formatter so the caller
// can advance the dump output buffer.
static size_t xband_stream_format (char *out, size_t out_size,
                                   const uint8 *stream, uint32 stream_used,
                                   bool is_tx)
{
	if (!out || out_size == 0 || stream_used == 0) return 0;
	size_t pos = 0;

	pos += snprintf(out + pos, out_size - pos,
		"Reassembled %s ServerTalk stream (%u bytes used, %u total ever appended):\n",
		is_tx ? "TX" : "RX",
		(unsigned)stream_used,
		(unsigned)(is_tx ? xband_tx_stream_pos : xband_rx_stream_pos));

	// Show the START of the stream first (login handshake is here).
	// If the stream is large, follow up with the END (most recent
	// activity). Cap at ~512 + 256 bytes total for the hex dump.
	uint32 head_n = stream_used;
	if (head_n > 512) head_n = 512;
	pos += snprintf(out + pos, out_size - pos,
		"  -- start of stream (first %u bytes) --\n", (unsigned)head_n);
	pos += xband_hex_ascii_dump_at(out + pos, out_size - pos,
		stream, (int)head_n, 0);

	if (stream_used > head_n + 16)
	{
		uint32 tail_n = stream_used - head_n;
		if (tail_n > 256) tail_n = 256;
		uint32 tail_start = stream_used - tail_n;
		pos += snprintf(out + pos, out_size - pos,
			"  -- end of stream (last %u bytes, offset %u..%u) --\n",
			(unsigned)tail_n, (unsigned)tail_start,
			(unsigned)(stream_used - 1));
		pos += xband_hex_ascii_dump_at(out + pos, out_size - pos,
			stream + tail_start, (int)tail_n, tail_start);
	}

	// Scan the WHOLE stream (not just the dumped portion) for opcode
	// candidates. Print up to 32 hits with absolute offsets so the
	// user can cross-reference with the hex dump above.
	pos += snprintf(out + pos, out_size - pos,
		"Opcode candidates in stream (filtered, $02 = msEndOfStream is a boundary marker):\n");
	int hits = 0;
	for (uint32 i = 0; i < stream_used && hits < 32 &&
	                   pos + 120 < out_size; i++)
	{
		uint8 b = stream[i];
		if (!xband_stream_byte_is_candidate_opcode(b, is_tx))
			continue;
		const char *name = is_tx
			? xband_servertalk_name_box_to_server(b)
			: xband_servertalk_name_server_to_box(b);
		// Pretty-print the next 4 bytes after the candidate so the
		// user can eyeball whether the field shape matches the opcode
		// (e.g. msBoxType $1F should be followed by 4 ASCII chars).
		char tail[20] = "";
		int tail_off = 0;
		for (int k = 1; k <= 6 && i + k < stream_used &&
		                tail_off < (int)sizeof(tail) - 4; k++)
		{
			tail_off += snprintf(tail + tail_off,
				sizeof(tail) - tail_off, "%02X ",
				(unsigned)stream[i + k]);
		}
		pos += snprintf(out + pos, out_size - pos,
			"  +%04u: $%02X %-30s next=[%s]\n",
			(unsigned)i, (unsigned)b, name, tail);
		hits++;
	}
	if (hits == 0)
		pos += snprintf(out + pos, out_size - pos,
			"  (no recognized opcodes in stream)\n");
	pos += snprintf(out + pos, out_size - pos, "\n");
	return pos;
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

		// Dispatch good frames into the ServerTalk decoder so the dump
		// can show per-opcode counts and human-readable details. Bad-CRC
		// frames are not parsed — the body is suspect.
		if (good)
		{
			xband_servertalk_dispatch_rx(
				xband_adsp_frame_buf, xband_adsp_frame_pos);
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

// =====================================================================
// Phase A: ADSP frame BUILDER + RX injection (the inverse of the
// parser/dispatcher above). Used by the fake-server menu trigger to
// synthesize a server reply and feed it into XBand.rxbuf as if it had
// arrived over the wire.
// =====================================================================

// Build a wire-format ADSP frame from logical fields. Output layout:
//
//   \x00 + escape(header[13] + payload + CRC[2]) + \x10\x03
//
// where escape() doubles every \x10 byte. CRC is CCITT-16-FALSE
// computed over the encapsulation byte (\x00) + header + payload --
// matches the canonical formula we validated against live RX frames
// (`xband_ccitt_crc16` above).
//
// Returns the number of bytes written to `out`. Returns 0 on failure
// (out too small for worst-case-stuffed output).
//
// Worst-case output size: 1 (encap) + 2*(13 + payload_len + 2) + 2 (EOP)
//   = 33 + 2*payload_len  for payload_len >= 0
// We require out_size >= that.
static int xband_adsp_build_frame (
	uint8 *out, int out_size,
	uint16 source_conn_id,
	uint32 first_byte_seq,
	uint32 next_recv_seq,
	uint16 recv_window,
	uint8  descriptor,
	const uint8 *payload, int payload_len)
{
	if (!out || out_size <= 0) return 0;
	if (payload_len < 0) payload_len = 0;
	if (33 + 2 * payload_len > out_size) return 0;

	// 1. Build the unstuffed body: 13-byte ADSP header + payload + CRC.
	//    We use a local scratch buffer for this so we can compute the
	//    CRC over (encap + body) before stuffing.
	uint8 body[1024];
	int   body_len = XBAND_ADSP_HEADER_LEN + payload_len + 2;
	if (body_len + 1 > (int)sizeof(body)) return 0;

	body[0]  = (uint8)(source_conn_id >> 8);
	body[1]  = (uint8)(source_conn_id);
	body[2]  = (uint8)(first_byte_seq >> 24);
	body[3]  = (uint8)(first_byte_seq >> 16);
	body[4]  = (uint8)(first_byte_seq >> 8);
	body[5]  = (uint8)(first_byte_seq);
	body[6]  = (uint8)(next_recv_seq >> 24);
	body[7]  = (uint8)(next_recv_seq >> 16);
	body[8]  = (uint8)(next_recv_seq >> 8);
	body[9]  = (uint8)(next_recv_seq);
	body[10] = (uint8)(recv_window >> 8);
	body[11] = (uint8)(recv_window);
	body[12] = descriptor;
	if (payload && payload_len > 0)
		memcpy(body + 13, payload, payload_len);

	// 2. Compute CRC over encap byte (\x00) + header + payload. The CRC
	//    helper xband_ccitt_crc16 already prepends the encap byte.
	uint16 crc = xband_ccitt_crc16(body, XBAND_ADSP_HEADER_LEN + payload_len);
	body[XBAND_ADSP_HEADER_LEN + payload_len]     = (uint8)(crc >> 8);
	body[XBAND_ADSP_HEADER_LEN + payload_len + 1] = (uint8)(crc);

	// 3. Emit the wire frame: leading \x00, byte-stuffed body, \x10\x03.
	int o = 0;
	out[o++] = 0x00;
	for (int i = 0; i < body_len; i++)
	{
		uint8 b = body[i];
		if (b == 0x10)
		{
			if (o + 2 > out_size) return 0;
			out[o++] = 0x10;
			out[o++] = 0x10;
		}
		else
		{
			if (o + 1 > out_size) return 0;
			out[o++] = b;
		}
	}
	if (o + 2 > out_size) return 0;
	out[o++] = 0x10;
	out[o++] = 0x03;
	return o;
}

// TX rewriter: parse an outgoing wire frame, look for the BIOS's
// broken default GameID in a msGAMEIDAndPatchVersion message, and
// rebuild the frame with the SSF2 Japan GameID instead. Used by the
// TX flush path when xband_tx_gameid_spoof is enabled, so the server
// (and our fake-server injects) think the box has SSF2 Japan loaded.
//
// `wire_in` / `wire_in_len` = original ADSP-framed wire bytes from
// txbuf (starting with $00, ending with $10$03, byte-stuffing applied).
// `wire_out` / `wire_out_size` = buffer to fill with rebuilt frame.
//
// Returns the new wire length on rewrite, 0 if no rewrite was needed
// (frame contained no msGAMEIDAndPatchVersion match), or -1 on error.
static int xband_tx_rewrite_gameid (
	const uint8 *wire_in, int wire_in_len,
	uint8 *wire_out, int wire_out_size)
{
	if (!wire_in || wire_in_len < 19 || !wire_out || wire_out_size <= 0)
		return 0;
	if (wire_in[0] != 0x00) return 0;
	if (wire_in[wire_in_len - 2] != 0x10 ||
	    wire_in[wire_in_len - 1] != 0x03)
		return 0;

	// 1. De-stuff the body (everything between encap byte and EOP).
	//    Maximum unstuffed body size is wire_in_len - 3.
	uint8 body[1024];
	int   body_len = 0;
	for (int i = 1; i < wire_in_len - 2 && body_len < (int)sizeof(body); i++)
	{
		uint8 b = wire_in[i];
		if (b == 0x10 && i + 1 < wire_in_len - 2 &&
		    wire_in[i + 1] == 0x10)
		{
			body[body_len++] = 0x10;
			i++;  // skip the second 0x10
		}
		else
		{
			body[body_len++] = b;
		}
	}

	// 2. Need at least the 13-byte ADSP header + 5 bytes payload (opcode
	//    + 4-byte GameID) + 2-byte CRC to even consider rewriting.
	if (body_len < XBAND_ADSP_HEADER_LEN + 5 + 2) return 0;

	// 3. Search the data section for the byte sequence
	//    `0C F7 2B 5D 1A` (msGAMEIDAndPatchVersion + broken GameID).
	//    Data section spans body[13..body_len-3] (excluding 2-byte CRC).
	int data_start = XBAND_ADSP_HEADER_LEN;
	int data_end   = body_len - 2;
	int found_off  = -1;
	for (int i = data_start; i + 5 <= data_end; i++)
	{
		if (body[i + 0] == 0x0C &&
		    body[i + 1] == 0xF7 && body[i + 2] == 0x2B &&
		    body[i + 3] == 0x5D && body[i + 4] == 0x1A)
		{
			found_off = i;
			break;
		}
	}
	if (found_off < 0) return 0;  // no rewrite needed

	// 4. Replace the 4 GameID bytes with SSF2 Japan's expected value.
	body[found_off + 1] = 0xD8;
	body[found_off + 2] = 0x22;
	body[found_off + 3] = 0x21;
	body[found_off + 4] = 0x03;

	// 5. Recompute the CRC over the modified body (header + data).
	//    Same formula the parser/dispatcher already validates against:
	//    CCITT-FALSE with leading $00 encap byte (xband_ccitt_crc16
	//    handles the encap-byte prefix internally).
	int data_only_len = body_len - 2;  // header + data, no CRC
	uint16 new_crc = xband_ccitt_crc16(body, data_only_len);
	body[data_only_len]     = (uint8)(new_crc >> 8);
	body[data_only_len + 1] = (uint8)(new_crc);

	// 6. Re-stuff and re-frame: $00 + escape(body) + $10$03.
	int o = 0;
	if (o >= wire_out_size) return -1;
	wire_out[o++] = 0x00;
	for (int i = 0; i < body_len; i++)
	{
		uint8 b = body[i];
		if (b == 0x10)
		{
			if (o + 2 > wire_out_size) return -1;
			wire_out[o++] = 0x10;
			wire_out[o++] = 0x10;
		}
		else
		{
			if (o + 1 > wire_out_size) return -1;
			wire_out[o++] = b;
		}
	}
	if (o + 2 > wire_out_size) return -1;
	wire_out[o++] = 0x10;
	wire_out[o++] = 0x03;

	xband_tx_gameid_spoof_count++;
	snprintf(xband_tx_gameid_spoof_last,
	         sizeof(xband_tx_gameid_spoof_last),
	         "spoof #%u: $0C $F7$2B$5D$1A -> $D8$22$21$03 "
	         "(in_len=%d out_len=%d body_len=%d off=%d)",
	         xband_tx_gameid_spoof_count, wire_in_len, o,
	         body_len, found_off);
	return o;
}

// Push raw wire bytes into XBand.rxbuf as if they had arrived from the
// socket. The BIOS will pull them out byte-at-a-time via fred reg $94
// (`xband_rxbuf_pop`) and feed them through its ADSP modem driver --
// the same path live RX bytes take.
//
// Returns the number of bytes injected (may be less than `len` if the
// rxbuf would overflow).
static int xband_inject_rxbuf_bytes (const uint8 *bytes, int len)
{
	if (!bytes || len <= 0) return 0;
	int written = 0;
	for (int i = 0; i < len; i++)
	{
		if (XBand.rxbufpos >= XBAND_RXBUF_SIZE) break;
		XBand.rxbuf[XBand.rxbufpos++] = bytes[i];

		// Mirror the byte into the RX socket-first capture so it shows
		// up in the kctl dump's RX hex view -- otherwise injected bytes
		// would be invisible to all our debug tooling.
		if (xband_sock_rx_first_used < XBAND_SOCK_FIRST_SIZE)
			xband_sock_rx_first[xband_sock_rx_first_used++] = bytes[i];

		// Also feed the ADSP detector so the dispatcher sees our
		// injected frame in the same way it sees live RX. This means
		// the per-opcode counters and decoded log reflect both real
		// and fake traffic, which is what we want for verification.
		xband_adsp_feed_byte(bytes[i]);

		written++;
	}
	xband_sock_rx_bytes += (uint64)written;
	return written;
}

// Public entry point: build an ADSP-framed ServerTalk reply with the
// given opcode + payload, sniff connID/seq from the live connection,
// and inject the frame into rxbuf so the BIOS will read it.
//
// Returns true on success. Failure modes:
//   - no live connection state sniffed yet (haven't seen any frames)
//   - frame builder ran out of buffer space (payload too big)
//   - rxbuf overflow on inject
bool S9xXBandFakeInject (uint8 opcode, const uint8 *payload, int payload_len)
{
	if (!xband_sniff_box_seen)
	{
		snprintf(xband_fake_inject_last,
		         sizeof(xband_fake_inject_last),
		         "FAILED: no box frames sniffed yet");
		return false;
	}

	// Pick the source connID per the active mode (see
	// xband_fake_connid_source above). Default is the sniffed server
	// connID, which matches Apple ADSP semantics; the alternate mode
	// uses the box's connID, which matches the xbsega open-conn-ack
	// behavior where the server echoes the box's frame back unchanged.
	uint16 srv_conn;
	const char *connid_label;
	if (xband_fake_connid_source == XBAND_FAKE_CONNID_BOX &&
	    xband_sniff_box_seen)
	{
		srv_conn     = xband_sniff_box_conn_id;
		connid_label = "BOX";
	}
	else if (xband_sniff_srv_seen)
	{
		srv_conn     = xband_sniff_srv_conn_id;
		connid_label = "SRV";
	}
	else
	{
		srv_conn     = (uint16)0x08C8;  // bsnes-plus default fallback
		connid_label = "FALLBACK";
	}

	// Build the ServerTalk message body: opcode + payload.
	uint8  st_body[256];
	int    st_len = 1 + payload_len;
	if (st_len > (int)sizeof(st_body))
		st_len = (int)sizeof(st_body);
	st_body[0] = opcode;
	if (payload && payload_len > 0)
		memcpy(st_body + 1, payload, st_len - 1);

	// ADSP send seq: where this server segment starts in the server's
	// outbound byte stream. Use a dedicated running counter that we
	// prime from the BOX's next_recv_seq on the first inject -- the
	// box authoritatively tells us "the next byte I expect from the
	// server is at sequence N", so injecting at exactly that position
	// will be in-order. For subsequent injects we just advance the
	// counter by the previous inject's data length so consecutive
	// injects line up tail-to-head with no gaps.
	if (!xband_fake_send_seq_primed)
	{
		xband_fake_send_seq        = xband_sniff_box_next_recv;
		xband_fake_send_seq_primed = true;
	}
	uint32 send_seq = xband_fake_send_seq;

	// Ack: the next byte we expect from the box. Use the box's
	// first_byte_seq + total observed data bytes since. The box's own
	// frames keep advancing this in real time, so as long as we read
	// it fresh each inject we should never under-ack. (Under-acking
	// just keeps the box's send window full -- ADSP receivers don't
	// reject under-acked segments.)
	uint32 ack_seq = xband_sniff_box_first_seq +
	                 xband_sniff_box_data_total;
	if (xband_sniff_srv_next_recv > ack_seq)
		ack_seq = xband_sniff_srv_next_recv;

	// Descriptor: bit 6 ($40) = ack request, bit 5 ($20) = EOM.
	// Together = $60. The ack-request bit asks the BIOS to send us an
	// ack frame after processing -- which gives us a clean signal that
	// the BIOS actually accepted our segment (its next next_recv_seq
	// will jump forward by our data length).
	uint8 descriptor = 0x60;

	// Recv window -- 1024 bytes is generous and matches what the live
	// server sends.
	uint16 recv_win = 0x0400;

	uint8 wire[600];
	int wire_len = xband_adsp_build_frame(
		wire, (int)sizeof(wire),
		srv_conn, send_seq, ack_seq, recv_win, descriptor,
		st_body, st_len);
	if (wire_len <= 0)
	{
		snprintf(xband_fake_inject_last,
		         sizeof(xband_fake_inject_last),
		         "FAILED: build_frame returned 0 (op $%02X len %d)",
		         (unsigned)opcode, payload_len);
		return false;
	}

	int injected = xband_inject_rxbuf_bytes(wire, wire_len);
	if (injected < wire_len)
	{
		snprintf(xband_fake_inject_last,
		         sizeof(xband_fake_inject_last),
		         "PARTIAL: %d/%d wire bytes injected (op $%02X)",
		         injected, wire_len, (unsigned)opcode);
		return false;
	}

	// Advance our running send_seq counter so the next inject lines
	// up tail-to-head with this one. NOTE: do NOT also bump
	// xband_sniff_srv_data_total here -- the dispatcher already
	// auto-bumped it when our injected frame went through
	// xband_inject_rxbuf_bytes -> xband_adsp_feed_byte ->
	// xband_servertalk_dispatch_rx (the same path live RX takes).
	// Double-counting was the source of the previous send_seq=$8
	// off-by-8 bug.
	xband_fake_send_seq += (uint32)st_len;
	xband_fake_inject_count++;
	snprintf(xband_fake_inject_last,
	         sizeof(xband_fake_inject_last),
	         "OK op=$%02X stlen=%d wirelen=%d connID=$%04X(%s) send_seq=$%08X ack=$%08X",
	         (unsigned)opcode, st_len, wire_len,
	         (unsigned)srv_conn, connid_label,
	         (unsigned)send_seq, (unsigned)ack_seq);
	return true;
}

// Inject a large ServerTalk payload by fragmenting it across multiple
// ADSP segments. Used for delivering binary blobs that exceed the
// ~109-byte ADSP segment payload limit -- e.g. game patches from the
// Cinghialotto/xband repo's XBAND_Game_Patches.zip, which are
// thousands of bytes long.
//
// Each segment carries a chunk of the payload as raw stream data:
//   - segment 0..N-2: descriptor = $00 (data, no EOM)
//   - segment N-1   : descriptor = $20 (EOM = end of message)
//
// All segments use sequential send_seq values so the BIOS's ADSP
// reassembler glues them back together into the original payload.
// We don't use ack-request bits on chunks because we send them all
// back-to-back without waiting for acks; ADSP receivers will catch
// up at their own pace.
//
// Returns the number of segments successfully injected. The caller
// can compare against the expected segment count to detect partial
// failure.
static int xband_fake_inject_chunked (
	const uint8 *payload, int payload_len,
	const char *label_for_status)
{
	if (!payload || payload_len <= 0) return 0;

	if (!xband_sniff_box_seen)
	{
		snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
		         "FAILED: no box frames sniffed yet (chunked %s)",
		         label_for_status ? label_for_status : "?");
		return 0;
	}

	// Pick connID per the active source mode.
	uint16 srv_conn;
	const char *connid_label;
	if (xband_fake_connid_source == XBAND_FAKE_CONNID_BOX &&
	    xband_sniff_box_seen)
	{
		srv_conn     = xband_sniff_box_conn_id;
		connid_label = "BOX";
	}
	else if (xband_sniff_srv_seen)
	{
		srv_conn     = xband_sniff_srv_conn_id;
		connid_label = "SRV";
	}
	else
	{
		srv_conn     = (uint16)0x08C8;
		connid_label = "FALLBACK";
	}

	// Prime send_seq from the box's next_recv_seq if needed.
	if (!xband_fake_send_seq_primed)
	{
		xband_fake_send_seq        = xband_sniff_box_next_recv;
		xband_fake_send_seq_primed = true;
	}

	// Choose chunk size. ADSP segment data section is typically
	// ~109 bytes in the live server traffic we observed. Stay at
	// or below that to avoid the BIOS's per-segment size limit.
	const int chunk_size = 100;

	// Compute initial ack_seq once -- box.first_seq + data_total
	// reflects the latest box send position. We don't refresh
	// between chunks because they're sent back-to-back.
	uint32 ack_seq = xband_sniff_box_first_seq +
	                 xband_sniff_box_data_total;
	if (xband_sniff_srv_next_recv > ack_seq)
		ack_seq = xband_sniff_srv_next_recv;

	uint16 recv_win = 0x0400;
	int    segments_sent = 0;
	int    pos = 0;

	while (pos < payload_len)
	{
		int remaining = payload_len - pos;
		int this_chunk = (remaining > chunk_size) ? chunk_size : remaining;
		bool is_last  = (pos + this_chunk == payload_len);

		// Descriptor: data segment, EOM only on the final chunk.
		uint8 descriptor = is_last ? 0x20 : 0x00;

		// Build wire frame for this chunk.
		uint8 wire[600];
		int wire_len = xband_adsp_build_frame(
			wire, (int)sizeof(wire),
			srv_conn, xband_fake_send_seq, ack_seq, recv_win,
			descriptor,
			payload + pos, this_chunk);
		if (wire_len <= 0)
		{
			snprintf(xband_fake_inject_last,
			         sizeof(xband_fake_inject_last),
			         "FAILED: build_frame chunk %d at offset %d (%s)",
			         segments_sent, pos,
			         label_for_status ? label_for_status : "?");
			return segments_sent;
		}

		int injected = xband_inject_rxbuf_bytes(wire, wire_len);
		if (injected < wire_len)
		{
			snprintf(xband_fake_inject_last,
			         sizeof(xband_fake_inject_last),
			         "PARTIAL: chunk %d %d/%d wire bytes (%s)",
			         segments_sent, injected, wire_len,
			         label_for_status ? label_for_status : "?");
			return segments_sent;
		}

		// Advance our running send_seq for the next chunk so they
		// stitch together as a contiguous byte stream.
		xband_fake_send_seq += (uint32)this_chunk;
		segments_sent++;
		pos += this_chunk;
	}

	xband_fake_inject_count++;
	snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
	         "OK chunked %s: %d segments, %d bytes payload, connID=$%04X(%s)",
	         label_for_status ? label_for_status : "?",
	         segments_sent, payload_len,
	         (unsigned)srv_conn, connid_label);
	return segments_sent;
}

// Read a binary file from disk into a caller-provided buffer.
// Returns the number of bytes read, or 0 on failure.
static int xband_read_file (const char *path, uint8 *buf, int buf_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	int n = (int)fread(buf, 1, (size_t)buf_size, f);
	fclose(f);
	return n;
}

// Inject a fake msNewNGPList with the BIOS's broken default cart
// hash ($F7 2B 5D 1A) mapped to "Super Street Fighter II". The hope
// is that the BIOS uses the NGP list as its "what games are
// supported" table -- so telling it the broken hash is a real game
// might make the BIOS skip the "not an XBAND Card" dialog when the
// user clicks Challenge.
//
// Format from sample_packets.txt:
//   0F           opcode (msNewNGPList)
//   00 23        2-byte length up to next length field (0x23 = 35)
//   00 01        2-byte count = 1 game
//   00 09        2-byte version of list = 9 (matches the example)
//   F7 2B 5D 1A  4-byte gameID = the BIOS's broken default hash
//   00 00 00 00  4-byte gameflags
//   00 00 00 03  4-byte patch version = 3
//   00 17        2-byte length of title
//   "Super Street Fighter2\0"  title (23 bytes including terminator)
//   02           msEndOfStream terminator
//
// Returns true on successful injection.
bool S9xXBandFakeInjectFakeNGPList (void)
{
	uint8 body[64];
	int   o = 0;

	// msNewNGPList header
	body[o++] = 0x0F;            // opcode
	body[o++] = 0x00;            // length high
	body[o++] = 0x23;            // length low = 35 (header + 1 game entry)
	body[o++] = 0x00;            // count high
	body[o++] = 0x01;            // count low = 1 game
	body[o++] = 0x00;            // list version high
	body[o++] = 0x09;            // list version low = 9

	// Game entry: gameID (the BIOS's broken hash so it self-recognizes)
	body[o++] = 0xF7;
	body[o++] = 0x2B;
	body[o++] = 0x5D;
	body[o++] = 0x1A;

	// gameflags
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x00;

	// patch version
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x03;

	// title length + title (23 bytes including terminator)
	body[o++] = 0x00; body[o++] = 0x17;
	const char *title = "Super Street Fighter2";
	for (int i = 0; title[i] && o < (int)sizeof(body); i++)
		body[o++] = (uint8)title[i];
	body[o++] = 0x00;            // null terminator
	body[o++] = 0x00;            // padding to 23 bytes

	// msEndOfStream
	body[o++] = 0x02;

	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

// Load SSF2.JSNES from BIOS_DIR and inject it as a chained
// msGamePatch. The patch file is the complete ServerTalk message:
// it already starts with the $03 opcode at byte 0, followed by the
// 4-byte GameID (d8 22 21 03 = SSF2 Japan), the patch version,
// type, length fields, and the in-game controller-injection code.
// Total ~3.3 KB; ends with a $02 msEndOfStream byte.
//
// Drop SSF2.JSNES into win32/BIOS/ alongside the SRAM dumps before
// firing this. Source:
//   https://github.com/Cinghialotto/xband
//   XBAND_Game_Patches.zip -> XBAND Game Patches/SNES/SSF2.JSNES
//
// Returns true if the file was found AND fully injected.
bool S9xXBandFakeInjectSSF2Patch (void)
{
	// Look for the patch in BIOS_DIR (same place as the SRAM dumps).
	std::string path = S9xGetDirectory(BIOS_DIR);
	path += SLASH_STR;
	path += "SSF2.JSNES";

	uint8 patch[8192];
	int patch_len = xband_read_file(path.c_str(), patch,
	                                (int)sizeof(patch));
	if (patch_len <= 0)
	{
		snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
		         "FAILED: SSF2.JSNES not found in BIOS_DIR (%s)",
		         path.c_str());
		return false;
	}

	// Sanity-check the patch shape: should start with $03 (msGamePatch
	// opcode) and the GameID for SSF2 Japan (d8 22 21 03).
	if (patch_len < 16 ||
	    patch[0] != 0x03 ||
	    patch[1] != 0xd8 || patch[2] != 0x22 ||
	    patch[3] != 0x21 || patch[4] != 0x03)
	{
		snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
		         "FAILED: SSF2.JSNES doesn't look right "
		         "(len=%d hdr=%02x %02x %02x %02x %02x...)",
		         patch_len,
		         patch_len > 0 ? patch[0] : 0,
		         patch_len > 1 ? patch[1] : 0,
		         patch_len > 2 ? patch[2] : 0,
		         patch_len > 3 ? patch[3] : 0,
		         patch_len > 4 ? patch[4] : 0);
		return false;
	}

	int segments = xband_fake_inject_chunked(patch, patch_len,
	                                          "SSF2.JSNES");
	return (segments > 0) && (segments * 100 >= patch_len - 100);
}

// Public toggle for the TX GameID spoofer. When ON, every outgoing
// ADSP frame containing $0C $F7 $2B $5D $1A is rewritten to use
// $D8 $22 $21 $03 (SSF2 Japan's expected GameID) and a fresh CRC.
// Returns the new state.
bool S9xXBandToggleGameIDSpoof (void)
{
	xband_tx_gameid_spoof = !xband_tx_gameid_spoof;
	return xband_tx_gameid_spoof;
}

bool S9xXBandGetGameIDSpoof (void)
{
	return xband_tx_gameid_spoof;
}

// Overwrite every $F7 $2B $5D $1A in known memory regions with
// $D8 $22 $21 $03 (SSF2 Japan's expected GameID). Returns the
// number of locations modified. Used to bypass the BIOS's local
// cart-detection check at Challenge click time -- we can't make
// the BIOS COMPUTE the right value without disassembling its
// cart-id routine, but we can stomp the cache it ends up reading.
//
// Writes a human-readable report into `out` so the user can see
// where we patched and verify the BIOS picks up the change.
int S9xXBandForceCartIDOverride (char *out, size_t out_size)
{
	int written = 0;
	size_t pos = 0;
	const uint8 from[4] = { 0xF7, 0x2B, 0x5D, 0x1A };
	const uint8 to[4]   = { 0xD8, 0x22, 0x21, 0x03 };

	if (out && out_size > 0)
		pos += snprintf(out + pos, out_size - pos,
			"Cart-ID override: $F7 $2B $5D $1A -> $D8 $22 $21 $03\n\n");

	// 1. WRAM ($7E:$0000 - $7F:$FFFF)
	if (Memory.RAM)
	{
		for (int i = 0; i + 4 <= 0x20000; i++)
		{
			if (Memory.RAM[i + 0] == from[0] &&
			    Memory.RAM[i + 1] == from[1] &&
			    Memory.RAM[i + 2] == from[2] &&
			    Memory.RAM[i + 3] == from[3])
			{
				Memory.RAM[i + 0] = to[0];
				Memory.RAM[i + 1] = to[1];
				Memory.RAM[i + 2] = to[2];
				Memory.RAM[i + 3] = to[3];
				written++;
				if (out && pos + 80 < out_size)
				{
					uint32 bank = (i >> 16) & 1;
					uint32 addr = i & 0xFFFF;
					pos += snprintf(out + pos, out_size - pos,
						"  WRAM   $7%c:$%04X (linear $%05X) PATCHED\n",
						bank ? 'F' : 'E', addr, i);
				}
			}
		}
	}

	// 2. XBAND SRAM
	for (int i = 0; i + 4 <= XBAND_SRAM_SIZE; i++)
	{
		if (XBand.sram[i + 0] == from[0] &&
		    XBand.sram[i + 1] == from[1] &&
		    XBand.sram[i + 2] == from[2] &&
		    XBand.sram[i + 3] == from[3])
		{
			XBand.sram[i + 0] = to[0];
			XBand.sram[i + 1] = to[1];
			XBand.sram[i + 2] = to[2];
			XBand.sram[i + 3] = to[3];
			written++;
			if (out && pos + 80 < out_size)
				pos += snprintf(out + pos, out_size - pos,
					"  SRAM   $%04X PATCHED\n", i);
		}
	}

	// 3. Fred general regs
	for (int i = 0; i + 4 <= XBAND_FRED_REGS; i++)
	{
		if (XBand.regs[i + 0] == from[0] &&
		    XBand.regs[i + 1] == from[1] &&
		    XBand.regs[i + 2] == from[2] &&
		    XBand.regs[i + 3] == from[3])
		{
			XBand.regs[i + 0] = to[0];
			XBand.regs[i + 1] = to[1];
			XBand.regs[i + 2] = to[2];
			XBand.regs[i + 3] = to[3];
			written++;
			if (out && pos + 80 < out_size)
				pos += snprintf(out + pos, out_size - pos,
					"  Fred   reg[$%02X..$%02X] PATCHED\n",
					i, i + 3);
		}
	}

	// 4. Modem regs
	for (int i = 0; i + 4 <= XBAND_MODEM_REGS; i++)
	{
		if (XBand.modem_regs[i + 0] == from[0] &&
		    XBand.modem_regs[i + 1] == from[1] &&
		    XBand.modem_regs[i + 2] == from[2] &&
		    XBand.modem_regs[i + 3] == from[3])
		{
			XBand.modem_regs[i + 0] = to[0];
			XBand.modem_regs[i + 1] = to[1];
			XBand.modem_regs[i + 2] = to[2];
			XBand.modem_regs[i + 3] = to[3];
			written++;
			if (out && pos + 80 < out_size)
				pos += snprintf(out + pos, out_size - pos,
					"  Modem  reg[$%02X..$%02X] PATCHED\n",
					i, i + 3);
		}
	}

	if (out && pos + 80 < out_size)
		pos += snprintf(out + pos, out_size - pos,
			"\nTotal patched: %d location(s)\n\n"
			"Now click Challenge. If the cart is recognized, the\n"
			"BIOS uses one of these locations and the dialog should\n"
			"either disappear or change content (e.g. show 'Super\n"
			"Street Fighter II' as a recognized game). If you still\n"
			"see 'This game may not be available', the BIOS reads\n"
			"the cart-id from a memory region we don't scan -- the\n"
			"value is computed on-demand or stored byte-swapped, etc.\n",
			written);

	return written;
}

// Search known memory regions for the BIOS's cached cart-id bytes
// (the "broken default" $F7 $2B $5D $1A that gets sent in
// msGAMEIDAndPatchVersion). If we find it, we know where the BIOS
// caches its computed cart-id and can overwrite it to spoof the
// cart locally before Challenge click.
//
// Now also searches for the spoofed value $D8 $22 $21 $03 so we
// can verify the read interceptor is working: if F7 2B 5D 1A is
// gone from memory but D8 22 21 03 is now present at $7F:$0C8B,
// the BIOS read our spoof and cached it. Confirms interception.
//
// Searches:
//   WRAM       $7E:$0000 - $7F:$FFFF (128 KB)
//   XBAND SRAM XBand.sram[] (64 KB)
//   Fred regs  XBand.regs[] (224 bytes)
//   Modem regs XBand.modem_regs[] (32 bytes)
//
// Writes a human-readable report into `out`. Used by the kctl trace
// menu to report findings without needing a separate dialog.

static void xband_search_one_pattern (char *out, size_t *pos_ptr,
                                       size_t out_size,
                                       const char *label,
                                       const uint8 *target)
{
	size_t pos = *pos_ptr;
	pos += snprintf(out + pos, out_size - pos,
		"Searching for %s ($%02X $%02X $%02X $%02X):\n",
		label, target[0], target[1], target[2], target[3]);

	int total = 0;
	if (Memory.RAM)
	{
		int hits_here = 0;
		for (int i = 0; i + 4 <= 0x20000; i++)
		{
			if (Memory.RAM[i + 0] == target[0] &&
			    Memory.RAM[i + 1] == target[1] &&
			    Memory.RAM[i + 2] == target[2] &&
			    Memory.RAM[i + 3] == target[3])
			{
				if (hits_here < 8)
				{
					uint32 bank = (i >> 16) & 1;
					uint32 addr = i & 0xFFFF;
					pos += snprintf(out + pos, out_size - pos,
						"  WRAM   $7%c:$%04X (linear $%05X)\n",
						bank ? 'F' : 'E', addr, i);
				}
				hits_here++;
				total++;
			}
		}
		if (hits_here > 8)
			pos += snprintf(out + pos, out_size - pos,
				"  WRAM   ... and %d more\n", hits_here - 8);
		if (hits_here == 0)
			pos += snprintf(out + pos, out_size - pos,
				"  WRAM   (no match)\n");
	}
	pos += snprintf(out + pos, out_size - pos,
		"  total: %d\n\n", total);
	*pos_ptr = pos;
}

void S9xXBandSearchCartIDInMemory (char *out, size_t out_size)
{
	if (!out || out_size == 0) return;
	size_t pos = 0;

	// Search both the BIOS default and the spoof target so we can
	// see whether the interceptor is working.
	const uint8 broken[4]  = { 0xF7, 0x2B, 0x5D, 0x1A };
	const uint8 spoofed[4] = { 0xD8, 0x22, 0x21, 0x03 };

	xband_search_one_pattern(out, &pos, out_size,
		"BIOS default cart-id (F7 2B 5D 1A)", broken);
	xband_search_one_pattern(out, &pos, out_size,
		"Spoofed SSF2 Japan cart-id (D8 22 21 03)", spoofed);

	pos += snprintf(out + pos, out_size - pos,
		"Note: PC log offsets >=$10 are for the MASTER source at\n"
		"$7F:2D15-2D18 (the MVN at $00:0EEA copies from there to\n"
		"$7F:0C8B). Offsets >=$100 in the read log mark reads from\n"
		"the master source.\n\n");

	// Dump the captured PCs from the WRITE trap first -- those are the
	// most valuable (they identify the cart-id computation function).
	pos += snprintf(out + pos, out_size - pos,
		"\nWrite trap PCs (BIOS code that WROTE to $7F:0C8B):\n");
	if (xband_cartid_write_pc_count == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"  (none -- BIOS hasn't written $7F:0C8B yet)\n");
	}
	else
	{
		int wn = xband_cartid_write_pc_count;
		if (wn > XBAND_CARTID_PC_LOG_SIZE) wn = XBAND_CARTID_PC_LOG_SIZE;
		uint32 last_pc_w = 0xFFFFFFFF;
		for (int i = 0; i < wn && pos + 400 < out_size; i++)
		{
			XBandCartIDReadEntry *e = &xband_cartid_write_pc_log[i];
			pos += snprintf(out + pos, out_size - pos,
				"  PC=$%06X  off=+%d val=$%02X  "
				"A=$%04X X=$%04X Y=$%04X DBR=$%02X D=$%04X\n",
				(unsigned)e->pc, (int)e->byte_off,
				(unsigned)e->byte_val,
				(unsigned)e->reg_a, (unsigned)e->reg_x,
				(unsigned)e->reg_y, (unsigned)e->reg_db,
				(unsigned)e->reg_d);
			// Only print snapshot bytes once per unique PC.
			if (e->pc != last_pc_w)
			{
				last_pc_w = e->pc;
				pos += snprintf(out + pos, out_size - pos,
					"    snapshot at trap (16 bytes before PC, *PC, 15 after):\n      ");
				for (int j = 0; j < 32; j++)
				{
					int marker = (j == 16) ? '*' : ' ';
					pos += snprintf(out + pos, out_size - pos,
						"%c%02X", marker, (unsigned)e->pc_bytes[j]);
				}
				pos += snprintf(out + pos, out_size - pos, "\n");
			}
		}
		pos += snprintf(out + pos, out_size - pos,
			"  (total writes since reset: %u)\n",
			(unsigned)xband_cartid_write_pc_count);
	}

	// Also dump the captured PCs from the read interceptor (if any).
	pos += snprintf(out + pos, out_size - pos,
		"\nRead interceptor PCs (BIOS code that read $7F:0C8B):\n");
	if (xband_cartid_read_pc_count == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"  (none -- BIOS hasn't read $7F:0C8B yet, "
			"or interceptor is disabled)\n");
	}
	else
	{
		int n = xband_cartid_read_pc_count;
		if (n > XBAND_CARTID_PC_LOG_SIZE) n = XBAND_CARTID_PC_LOG_SIZE;
		// Dedupe -- consecutive entries with the same PC are usually
		// loop iterations and we only need the bytes at that PC once.
		uint32 last_pc = 0xFFFFFFFF;
		for (int i = 0; i < n; i++)
		{
			XBandCartIDReadEntry *e = &xband_cartid_read_pc_log[i];
			pos += snprintf(out + pos, out_size - pos,
				"  PC=$%06X  offset=+%d  value-returned=$%02X\n",
				(unsigned)e->pc, (int)e->byte_off,
				(unsigned)e->byte_val);
			if (e->pc != last_pc)
				last_pc = e->pc;
		}
		pos += snprintf(out + pos, out_size - pos,
			"  (total reads since reset: %u)\n\n",
			(unsigned)xband_cartid_read_pc_count);

		// Dump WRAM bytes around each unique PC -- wider window so the
		// instruction start (which is BEFORE the captured PC because
		// snes9x advances PBPC during operand fetch) is included.
		// The PCs live in $00:$0000-$1FFF which mirrors $7E:$0000-$1FFF,
		// so we read directly from Memory.RAM.
		//
		// Also: since the BIOS copied this code from its firmware ROM
		// into WRAM at boot, we search Memory.BIOSROM for the same
		// 32-byte sequence that's around each PC. If we find a match,
		// the firmware offset gives us a stable reference to disassemble
		// against -- much easier than reading WRAM that gets reused.
		pos += snprintf(out + pos, out_size - pos,
			"WRAM bytes around captured PCs (-64..+96 = 160 bytes):\n"
			"  (instr start is N bytes BEFORE the * marker, where N is\n"
			"   the size of whatever instruction read $7F:0C8B)\n");
		uint32 dumped[XBAND_CARTID_PC_LOG_SIZE];
		int dumped_n = 0;
		for (int i = 0; i < n && pos + 512 < out_size; i++)
		{
			uint32 pc = xband_cartid_read_pc_log[i].pc;
			bool seen = false;
			for (int j = 0; j < dumped_n; j++)
				if (dumped[j] == pc) { seen = true; break; }
			if (seen) continue;
			if (dumped_n < (int)XBAND_CARTID_PC_LOG_SIZE)
				dumped[dumped_n++] = pc;

			uint16 lo  = (uint16)(pc & 0xFFFF);
			uint8  bnk = (uint8)((pc >> 16) & 0xFF);
			if (bnk != 0 || lo > 0x1FFF || !Memory.RAM)
			{
				pos += snprintf(out + pos, out_size - pos,
					"  PC=$%06X (not in low WRAM mirror, skipping)\n",
					(unsigned)pc);
				continue;
			}
			pos += snprintf(out + pos, out_size - pos,
				"\n  PC=$%06X (linear $%05X in WRAM):\n",
				(unsigned)pc, (unsigned)lo);
			int start = (int)lo - 64;
			int end   = (int)lo + 96;
			if (start < 0) start = 0;
			if (end > 0x2000) end = 0x2000;
			for (int row = start; row < end; row += 16)
			{
				pos += snprintf(out + pos, out_size - pos,
					"    $7E:$%04X: ", row);
				for (int col = 0; col < 16; col++)
				{
					if (row + col >= end) break;
					int marker = (row + col == (int)lo) ? '*' : ' ';
					pos += snprintf(out + pos, out_size - pos,
						"%c%02X", marker,
						(unsigned)Memory.RAM[row + col]);
				}
				pos += snprintf(out + pos, out_size - pos, "\n");
			}

			// Search BIOS firmware for the same 32-byte chunk around
			// the PC. If the BIOS copied this code from firmware to
			// WRAM, the chunk will appear once in firmware and we can
			// give the user a stable BIOS offset to look at.
			//
			// In multicart mode (cartType == 6), the BIOS lives at
			// Memory.ROM + Multi.cartOffsetA. Otherwise it's in
			// Memory.BIOSROM (standalone load path).
			uint8 *bios_base = NULL;
			int firmware_size = 0x100000;
			if (Multi.cartType == 6 && Memory.ROM)
				bios_base = Memory.ROM + Multi.cartOffsetA;
			else if (Memory.BIOSROM)
				bios_base = Memory.BIOSROM;
			if (bios_base && lo >= 16 && lo + 16 < 0x2000)
			{
				uint8 chunk[32];
				memcpy(chunk, Memory.RAM + lo - 16, 32);
				int firmware_hits = 0;
				int first_hit = -1;
				for (int o = 0; o + 32 <= firmware_size; o++)
				{
					if (memcmp(bios_base + o, chunk, 32) == 0)
					{
						firmware_hits++;
						if (first_hit < 0) first_hit = o;
						if (firmware_hits >= 4) break;
					}
				}
				if (firmware_hits > 0)
					pos += snprintf(out + pos, out_size - pos,
						"  -> matches BIOS firmware offset $%05X (%d hit%s total)\n",
						(unsigned)first_hit, firmware_hits,
						firmware_hits == 1 ? "" : "s");
				else
					pos += snprintf(out + pos, out_size - pos,
						"  -> no match in BIOS firmware (code may be runtime-generated)\n");
			}
		}
	}
	return;

	// Old detailed search kept below for reference -- not reached.
	const uint8 target[4] = { 0xF7, 0x2B, 0x5D, 0x1A };
	int total_hits = 0;

	// 1. WRAM ($7E:$0000 - $7F:$FFFF) -- 128 KB main system RAM
	if (Memory.RAM)
	{
		int hits_here = 0;
		for (int i = 0; i + 4 <= 0x20000; i++)
		{
			if (Memory.RAM[i + 0] == target[0] &&
			    Memory.RAM[i + 1] == target[1] &&
			    Memory.RAM[i + 2] == target[2] &&
			    Memory.RAM[i + 3] == target[3])
			{
				if (hits_here < 8)
				{
					uint32 bank = (i >> 16) & 1;
					uint32 addr = i & 0xFFFF;
					pos += snprintf(out + pos, out_size - pos,
						"  WRAM   $7%c:$%04X (linear $%05X)\n",
						bank ? 'F' : 'E', addr, i);
				}
				hits_here++;
				total_hits++;
			}
		}
		if (hits_here > 8)
			pos += snprintf(out + pos, out_size - pos,
				"  WRAM   ... and %d more\n", hits_here - 8);
		if (hits_here == 0)
			pos += snprintf(out + pos, out_size - pos,
				"  WRAM   (no match in 128 KB)\n");
	}

	// 2. XBAND SRAM (64 KB)
	{
		int hits_here = 0;
		for (int i = 0; i + 4 <= XBAND_SRAM_SIZE; i++)
		{
			if (XBand.sram[i + 0] == target[0] &&
			    XBand.sram[i + 1] == target[1] &&
			    XBand.sram[i + 2] == target[2] &&
			    XBand.sram[i + 3] == target[3])
			{
				if (hits_here < 8)
					pos += snprintf(out + pos, out_size - pos,
						"  SRAM   $%04X\n", i);
				hits_here++;
				total_hits++;
			}
		}
		if (hits_here > 8)
			pos += snprintf(out + pos, out_size - pos,
				"  SRAM   ... and %d more\n", hits_here - 8);
		if (hits_here == 0)
			pos += snprintf(out + pos, out_size - pos,
				"  SRAM   (no match in 64 KB)\n");
	}

	// 3. Fred general register file (224 bytes)
	{
		int hits_here = 0;
		for (int i = 0; i + 4 <= XBAND_FRED_REGS; i++)
		{
			if (XBand.regs[i + 0] == target[0] &&
			    XBand.regs[i + 1] == target[1] &&
			    XBand.regs[i + 2] == target[2] &&
			    XBand.regs[i + 3] == target[3])
			{
				pos += snprintf(out + pos, out_size - pos,
					"  Fred   reg[$%02X..$%02X]\n", i, i + 3);
				hits_here++;
				total_hits++;
			}
		}
		if (hits_here == 0)
			pos += snprintf(out + pos, out_size - pos,
				"  Fred   (no match in 224 regs)\n");
	}

	// 4. Modem register file (32 bytes)
	{
		int hits_here = 0;
		for (int i = 0; i + 4 <= XBAND_MODEM_REGS; i++)
		{
			if (XBand.modem_regs[i + 0] == target[0] &&
			    XBand.modem_regs[i + 1] == target[1] &&
			    XBand.modem_regs[i + 2] == target[2] &&
			    XBand.modem_regs[i + 3] == target[3])
			{
				pos += snprintf(out + pos, out_size - pos,
					"  Modem  reg[$%02X..$%02X]\n", i, i + 3);
				hits_here++;
				total_hits++;
			}
		}
		if (hits_here == 0)
			pos += snprintf(out + pos, out_size - pos,
				"  Modem  (no match in 32 regs)\n");
	}

	pos += snprintf(out + pos, out_size - pos,
		"\nTotal: %d match(es) found.\n", total_hits);

	if (total_hits == 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"\nThe value isn't cached in any of the regions we\n"
			"searched. Possible causes:\n"
			"  - Cached in BIOS-private RAM that mirrors WRAM\n"
			"  - Stored byte-swapped or in another encoding\n"
			"  - Computed on-the-fly each time (no cache)\n"
			"  - Using a different value than $F7 $2B $5D $1A\n"
			"    (the value might depend on what triggered the\n"
			"    boot path -- run this AFTER reaching the main menu)\n");
	}
	else
	{
		pos += snprintf(out + pos, out_size - pos,
			"\nNext step: find which match the BIOS reads when you\n"
			"click Challenge. We can write a debugger trap on each\n"
			"candidate location to identify the right one, then\n"
			"overwrite it with $D8 $22 $21 $03 (SSF2 Japan) before\n"
			"clicking Challenge.\n");
	}
}

// Public toggle for the connID source -- LIVE_SRV (default) vs BOX
// (xbsega echo-back model). Cycles between the two each time it's
// called. Used by the menu A/B test.
void S9xXBandFakeToggleConnIDSource (void)
{
	xband_fake_connid_source =
		(xband_fake_connid_source == XBAND_FAKE_CONNID_LIVE_SRV)
		? XBAND_FAKE_CONNID_BOX
		: XBAND_FAKE_CONNID_LIVE_SRV;
}

// Post-Challenge "advance" reply.
//
// First attempt sent msGamePatch (op $03) with length=0, hoping the
// BIOS would interpret it as "game accepted, no patch needed". The
// BIOS instead **panicked**: ran straight into its fatal-error
// handler at $D0:3AD8 which wipes XBAND SRAM byte 0, disables IRQs,
// and STPs the CPU. Confirmed empty msGamePatch is poison: the BIOS
// validates the patch contents (probably CRC + cart-hash match) and
// fails recovery on bad input. Lesson: do not send a msGamePatch
// without a real patch payload tied to the actual cart hash.
//
// Safer behavior: send just msEndOfStream. We already proved
// (Phase A) that bare msEndOfStream advances the BIOS state machine
// without triggering validation. The BIOS interprets it as "the
// server has finished its current reply stream, proceed".
//
// If you want to play with msGamePatch in the future, the format
// likely needs at minimum: 4-byte length, 4-byte cart-hash the
// patch is for, N bytes of patch data, possibly a CRC. Get any of
// those wrong and the BIOS will hard-panic again.
bool S9xXBandFakeInjectGameSupported (void)
{
	uint8 body[2];
	body[0] = 0x02;  // msEndOfStream
	return S9xXBandFakeInject(body[0], NULL, 0);
}

// Inject a "post-login canned response" that bundles several
// ServerTalk messages into one ADSP segment, in the order a real
// server would send them after the box's login dump:
//
//   msSetDateAndTime  (4 bytes date + 5 bytes time, raw values)
//   msSetCurrentUserNumber 0  (1 byte profile index)
//   msReceiveValidationToken  (4 zero token bytes)
//   msEndOfStream
//
// All four messages are concatenated tail-to-head into a single
// ServerTalk byte stream and sent as one ADSP data segment with EOM
// + ack-request descriptor. This is a closer match to what the live
// server would actually send than the bare-msEndOfStream test, and
// it gives the BIOS structured fields to populate its UI with.
//
// Returns true on successful injection.
bool S9xXBandFakeInjectLoginReply (void)
{
	uint8 body[64];
	int   o = 0;

	// msSetDateAndTime (op $04) + 4 bytes date + 5 bytes time. The
	// values are taken from sample_packets.txt example: 04 000059C3
	// 000031DC02. We don't know if the BIOS validates the format
	// strictly; pass the same values the example shows.
	body[o++] = 0x04;
	body[o++] = 0x00; body[o++] = 0x1A; // year 2026
	body[o++] = 0x35; body[o++] = 0x00; // April 10
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x00; // time: midnight

	// msSetCurrentUserNumber (op $3E = 62) + 1 byte profile index.
	body[o++] = 0x3E;
	body[o++] = 0x00;  // profile 0 = first user

	// msReceiveValidationToken (op $3B = 59) + 4 zero token bytes.
	// Token format unknown -- 4 zeros is a guess. The BIOS may
	// accept it as "valid" or may reject it based on contents.
	body[o++] = 0x3B;
	body[o++] = 0x00; body[o++] = 0x00; body[o++] = 0x00; body[o++] = 0x00;

	// msEndOfStream (op $02) -- terminator
	body[o++] = 0x02;

	// Inject the entire blob as a single ServerTalk "message" (the
	// individual messages will be unwrapped by the BIOS dispatcher).
	// We pass body[0] as the "opcode" to the inject function and
	// body[1..o-1] as the "payload" -- the inject function doesn't
	// actually distinguish, it just builds an ADSP segment with the
	// concatenated bytes.
	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

const char *S9xXBandFakeConnIDSourceLabel (void)
{
	return (xband_fake_connid_source == XBAND_FAKE_CONNID_BOX)
		? "BOX (xbsega echo-back)"
		: "SRV (Apple ADSP standard)";
}

// ================================================================
// Event-driven XBAND Server. Watches the BIOS's TX ServerTalk
// stream, detects batch boundaries (msEndOfStream $02), analyzes
// what was sent, and injects appropriate server responses. Requires
// a real TCP connection for the ADSP handshake (the real server at
// xbserver.retrocomputing.network handles that), then takes over
// all subsequent ServerTalk processing.
//
// Start via "XBAND: Start Server" menu. State visible in the kctl
// trace. Responses injected via existing S9xXBandFakeInject.
// ================================================================
static void xband_force_prime_sniff_state (void); // forward decl

enum XBandServerState {
	XBSVR_OFF = 0,
	XBSVR_HANDSHAKE,          // waiting for ADSP handshake to complete
	XBSVR_WAIT_LOGIN,         // waiting for login batch from BIOS
	XBSVR_INJECT_LOGIN_REPLY, // ready to inject login response
	XBSVR_WAIT_DATA,          // waiting for BIOS data dump batches
	XBSVR_INJECT_DATA_ACK,    // ready to inject data ack
	XBSVR_INJECT_NGP,         // inject NGP game list
	XBSVR_WAIT_NGP,           // wait for BIOS to process NGP
	XBSVR_INJECT_PATCH,       // inject game patch (chunked)
	XBSVR_WAIT_PATCH,         // wait for BIOS to process patch
	XBSVR_INJECT_MATCHMAKING, // inject msWaitForOpponent
	XBSVR_MATCHMAKING,        // done, in matchmaking
};

static int    xbsvr_state = XBSVR_OFF;
static uint32 xbsvr_tx_baseline = 0;
static uint32 xbsvr_poll_count = 0;
static int    xbsvr_data_batch_count = 0;
static int    xbsvr_connection_number = 0; // 1st connect = login, 2nd = challenge
static bool   xbsvr_intercept_rx = false; // block real server RX

// TX batch accumulator: opcodes seen in current BIOS batch.
#define XBSVR_BATCH_MAX 32
static uint8 xbsvr_batch[XBSVR_BATCH_MAX];
static int   xbsvr_batch_count = 0;

// Captured game ID from msGAMEIDAndPatchVersion ($0C).
static uint8 xbsvr_game_id[4] = {0};
static bool  xbsvr_game_id_seen = false;

// Timestamped server log ring.
#define XBSVR_LOG_SIZE 256
struct XBandServerLogEntry {
	uint64 frame;       // CPU cycle counter or frame number
	bool   is_tx;       // true = BIOS→server, false = server→BIOS
	uint8  opcode;
	char   text[128];
};
static XBandServerLogEntry xbsvr_log[XBSVR_LOG_SIZE];
static int xbsvr_log_head = 0;
static int xbsvr_log_count = 0;

static void xbsvr_log_append (bool is_tx, uint8 op, const char *fmt, ...)
{
	XBandServerLogEntry *e = &xbsvr_log[xbsvr_log_head];
	e->frame = CPU.Cycles;
	e->is_tx = is_tx;
	e->opcode = op;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	xbsvr_log_head = (xbsvr_log_head + 1) % XBSVR_LOG_SIZE;
	if (xbsvr_log_count < XBSVR_LOG_SIZE) xbsvr_log_count++;
}

// --- ADSP keepalive: header-only ack frame ---
// Sends a descriptor-$80 frame with no data — just acknowledges
// the BIOS's TX position so the ADSP session stays alive.
static void xbsvr_send_keepalive (void)
{
	uint16 srv_conn = xband_sniff_srv_seen
		? xband_sniff_srv_conn_id : (uint16)0x08C8;
	uint32 ack_seq = xband_sniff_box_first_seq +
	                 xband_sniff_box_data_total;
	uint16 recv_win = 0x0400;

	// Use our current fake send_seq (position in server→box stream).
	if (!xband_fake_send_seq_primed)
	{
		xband_fake_send_seq        = xband_sniff_box_next_recv;
		xband_fake_send_seq_primed = true;
	}

	uint8 wire[64];
	int wire_len = xband_adsp_build_frame(
		wire, (int)sizeof(wire),
		srv_conn, xband_fake_send_seq, ack_seq, recv_win,
		0x80,       // descriptor: header-only ack, no data
		NULL, 0);   // no payload
	if (wire_len > 0)
		xband_inject_rxbuf_bytes(wire, wire_len);
}

// --- Response injection helpers ---

static bool xbsvr_inject_login_reply (void)
{
	uint8 body[64];
	int o = 0;
	body[o++] = 0x04; // msSetDateAndTime
	body[o++] = 0x00; body[o++] = 0x1A; // year 2026
	body[o++] = 0x35; body[o++] = 0x00; // April 10
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x00; // time: midnight
	body[o++] = 0x3E; // msSetCurrentUserNumber
	body[o++] = 0x00;
	// msReceiveValidationToken ($3B) — zeros don't match the SRAM
	// token so the BIOS shows "battery may be dead", but it DOES
	// proceed past login afterward. Without this message, the BIOS
	// hangs waiting. TODO: read the real token from XBand.sram[].
	body[o++] = 0x3B;
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x17; // msClearSendQ
	body[o++] = 0x02; // msEndOfStream
	xbsvr_log_append(false, 0x04,
		"RX login reply: date+user+token+clearQ+EOS");
	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

static bool xbsvr_inject_data_ack (void)
{
	uint8 body[4];
	int o = 0;
	body[o++] = 0x17; // msClearSendQ
	body[o++] = 0x02; // msEndOfStream
	xbsvr_log_append(false, 0x17,
		"RX data ack: clearQ+EOS (batch #%d)", xbsvr_data_batch_count);
	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

static bool xbsvr_inject_ngp_list (void)
{
	uint8 body[128];
	int o = 0;
	body[o++] = 0x0F; // msNewNGPList
	body[o++] = 0x00; body[o++] = 0x10; // length = 16
	body[o++] = 0x00; body[o++] = 0x01; // count = 1
	body[o++] = 0x00; body[o++] = 0x01; // version = 1
	// Use captured game ID, or SSF2 default $D8222103
	static uint8 default_gid[4] = {0xD8, 0x22, 0x21, 0x03};
	uint8 *gid = xbsvr_game_id_seen ? xbsvr_game_id : default_gid;
	body[o++] = gid[0]; body[o++] = gid[1];
	body[o++] = gid[2]; body[o++] = gid[3];
	body[o++] = 0x00; body[o++] = 0x00; // flags
	body[o++] = 0x00; body[o++] = 0x00;
	body[o++] = 0x00; body[o++] = 0x00; // patch version = 1
	body[o++] = 0x00; body[o++] = 0x01;
	body[o++] = 0x00; body[o++] = 0x18; // title len = 24
	const char *title = "Super Street Fighter 2";
	int tlen = (int)strlen(title);
	for (int i = 0; i < 24; i++)
		body[o++] = (i < tlen) ? (uint8)title[i] : 0x00;
	body[o++] = 0x02; // msEndOfStream
	xbsvr_log_append(false, 0x0F,
		"RX NGP list: gameID=%02X%02X%02X%02X SSF2",
		gid[0], gid[1], gid[2], gid[3]);
	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

static bool xbsvr_inject_game_patch (void)
{
	std::string path = S9xGetDirectory(BIOS_DIR);
	path += SLASH_STR;
	path += "SSF2.JSNES";
	uint8 patch[8192];
	int patch_len = xband_read_file(path.c_str(), patch,
	                                (int)sizeof(patch));
	if (patch_len <= 0)
	{
		xbsvr_log_append(false, 0x03,
			"RX game patch: SSF2.JSNES not found, skipped");
		return false;
	}
	xbsvr_log_append(false, 0x03,
		"RX game patch: SSF2.JSNES %d bytes chunked", patch_len);
	return xband_fake_inject_chunked(patch, patch_len, "SSF2.JSNES") > 0;
}

static bool xbsvr_inject_wait_for_opponent (void)
{
	uint8 body[4];
	int o = 0;
	body[o++] = 0x1C; // msWaitForOpponent
	body[o++] = 0x02; // msEndOfStream
	xbsvr_log_append(false, 0x1C, "RX matchmaking: waitForOpponent+EOS");
	return S9xXBandFakeInject(body[0], body + 1, o - 1);
}

// --- TX Stream Watcher ---
// Instead of parsing per-frame opcodes (which are unreliable due to
// ADSP continuation segments), we watch the reassembled TX stream
// growth. The BIOS sends data in bursts; each burst is a batch.
// When the stream grows and then pauses, a batch has completed.
// We use the stream size + poll-count timeouts to detect this.
static uint32 xbsvr_last_tx_pos = 0; // last scanned TX stream pos

// --- Server Tick: called from kreadmstatus2 poll ---
// Uses TX stream growth to detect when the BIOS has finished
// sending a batch. Much more reliable than per-frame opcode
// parsing (which is broken by ADSP continuation segments).

static void xband_server_tick (void)
{
	if (xbsvr_state <= XBSVR_OFF) return;

	xbsvr_poll_count++;

	// Send ADSP keepalive acks periodically to prevent the BIOS's
	// ADSP layer from timing out. Every ~200 ticks ≈ a few times
	// per second. Runs in ALL states including MATCHMAKING.
	if ((xbsvr_poll_count % 200) == 0 && xband_sniff_box_seen)
		xbsvr_send_keepalive();
	uint32 cur_tx_pos = xband_tx_stream_pos;
	uint32 tx_growth = cur_tx_pos - xbsvr_last_tx_pos;

	switch (xbsvr_state)
	{
	case XBSVR_HANDSHAKE:
		if (xband_sniff_box_seen)
		{
			xbsvr_connection_number++;
			xbsvr_log_append(false, 0,
				"ADSP handshake complete (connection #%d)",
				xbsvr_connection_number);
			xbsvr_intercept_rx = false;
			xbsvr_last_tx_pos = cur_tx_pos;
			// Every connection: wait for TX data, then send
			// login reply + msRegisterPlayer. The BIOS uses
			// whichever is relevant for its current mode.
			xbsvr_state = XBSVR_WAIT_DATA;
			xbsvr_poll_count = 0;
		}
		break;

	// Auto-reset: if the server is past login and the BIOS reconnects
	// (e.g. Challenge dial), detect the new handshake and restart.
	case XBSVR_MATCHMAKING:
		// Check if the BIOS dropped and reconnected. If TX stream
		// resets (new connection), go back to HANDSHAKE.
		if (!xband_sniff_box_seen)
		{
			xbsvr_log_append(false, 0, "=== connection dropped, waiting for reconnect ===");
			xbsvr_state = XBSVR_HANDSHAKE;
			xbsvr_poll_count = 0;
		}
		break;

	case XBSVR_INJECT_LOGIN_REPLY:
		xbsvr_inject_login_reply();
		xbsvr_state = XBSVR_WAIT_DATA;
		xbsvr_last_tx_pos = cur_tx_pos;
		xbsvr_poll_count = 0;
		xbsvr_data_batch_count = 0;
		break;

	case XBSVR_WAIT_DATA:
		// Wait for the BIOS data dump. Give it plenty of time to
		// process the login reply and show the "battery dead" dialog
		// before we send anything else. ~5 seconds.
		if (xbsvr_poll_count > 15000)
		{
			xbsvr_data_batch_count++;
			xbsvr_log_append(true, 0, "TX data batch #%d (%u bytes total)",
				xbsvr_data_batch_count, (unsigned)cur_tx_pos);
			// After the first data batch, go straight to game reply.
			xbsvr_state = XBSVR_INJECT_DATA_ACK;
			xbsvr_data_batch_count = -1; // signal: proceed to NGP
			xbsvr_poll_count = 0;
		}
		break;

	case XBSVR_INJECT_DATA_ACK:
	{
		// Detect BIOS action by scanning TX stream for msChallengeRequest ($0E).
		// If present → Challenge mode → send msRegisterPlayer.
		// If absent → Connect/X-Mail mode → send login reply only.
		bool is_challenge = false;
		{
			uint32 tx_used = xband_tx_stream_pos;
			if (tx_used > XBAND_STREAM_BUF_SIZE)
				tx_used = XBAND_STREAM_BUF_SIZE;
			for (uint32 i = 0; i < tx_used; i++)
			{
				if (xband_tx_stream[i] == 0x0E)
				{
					is_challenge = true;
					break;
				}
			}
		}

		if (is_challenge)
		{
			// Challenge mode: send msRegisterPlayer (xbsega.go style)
			xbsvr_log_append(false, 0, "TX contains $0E → CHALLENGE mode");
			uint8 reg_body[8];
			int ro = 0;
			reg_body[ro++] = 0x0E; // msRegisterPlayer
			reg_body[ro++] = 0x01; // wait time 0x01100000
			reg_body[ro++] = 0x10;
			reg_body[ro++] = 0x00;
			reg_body[ro++] = 0x00;
			reg_body[ro++] = 0x02; // msEndOfStream
			S9xXBandFakeInject(reg_body[0], reg_body + 1, ro - 1);
			xbsvr_log_append(false, 0x0E, "RX msRegisterPlayer");
		}
		else
		{
			// Connect/X-Mail mode: send login reply only
			xbsvr_log_append(false, 0, "TX no $0E → LOGIN mode");
			xbsvr_inject_login_reply();
			xbsvr_log_append(false, 0x04, "RX login reply");
		}

		xbsvr_state = XBSVR_MATCHMAKING;
		xbsvr_log_append(false, 0, "=== SERVER: done ===");
	}
		xbsvr_tx_baseline = cur_tx_pos;
		xbsvr_last_tx_pos = cur_tx_pos;
		xbsvr_poll_count = 0;
		break;

	case XBSVR_INJECT_NGP:
		xbsvr_inject_ngp_list();
		xbsvr_state = XBSVR_WAIT_NGP;
		xbsvr_last_tx_pos = cur_tx_pos;
		xbsvr_poll_count = 0;
		break;

	case XBSVR_WAIT_NGP:
	case XBSVR_WAIT_PATCH:
	{
		// Pure timeout — don't reset on TX growth. The BIOS may be
		// sending a second login dump (challenge reconnect) which
		// would keep resetting the counter forever.
		if (xbsvr_poll_count > 5000)
		{
			xbsvr_state = (xbsvr_state == XBSVR_WAIT_NGP)
				? XBSVR_INJECT_PATCH : XBSVR_INJECT_MATCHMAKING;
			xbsvr_last_tx_pos = cur_tx_pos;
			xbsvr_poll_count = 0;
		}
		break;
	}

	case XBSVR_INJECT_PATCH:
		xbsvr_inject_game_patch();
		xbsvr_state = XBSVR_WAIT_PATCH;
		xbsvr_last_tx_pos = cur_tx_pos;
		xbsvr_poll_count = 0;
		break;

	case XBSVR_INJECT_MATCHMAKING:
	{
		// msRegisterPlayer ($0E) + 4-byte wait time — tells the BIOS
		// "you're registered, wait N seconds." Must come BEFORE
		// msWaitForOpponent per the real server flow.
		uint8 reg_body[8];
		int ro = 0;
		reg_body[ro++] = 0x0E; // msRegisterPlayer
		reg_body[ro++] = 0x01; // wait time (4 bytes BE) = 1 second
		reg_body[ro++] = 0x00;
		reg_body[ro++] = 0x00;
		reg_body[ro++] = 0x00;
		reg_body[ro++] = 0x02; // msEndOfStream
		S9xXBandFakeInject(reg_body[0], reg_body + 1, ro - 1);
		xbsvr_log_append(false, 0x0E, "RX registerPlayer (wait=1s)");

		xbsvr_inject_wait_for_opponent();
		xbsvr_state = XBSVR_MATCHMAKING;
		xbsvr_log_append(false, 0, "=== SERVER: matchmaking active ===");
		break;
	}

	default:
		break;
	}
}

// --- Public API ---

void S9xXBandServerStart (void)
{
	xbsvr_state = XBSVR_HANDSHAKE;
	xbsvr_poll_count = 0;
	xbsvr_tx_baseline = 0;
	xbsvr_last_tx_pos = xband_tx_stream_pos;
	xbsvr_data_batch_count = 0;
	xbsvr_connection_number = 0;
	xbsvr_game_id_seen = false;
	xbsvr_intercept_rx = false;
	xbsvr_log_count = 0;
	xbsvr_log_head = 0;
	xbsvr_log_append(false, 0, "=== SERVER STARTED, waiting for handshake ===");
}

void S9xXBandServerStop (void)
{
	xbsvr_state = XBSVR_OFF;
	xbsvr_intercept_rx = false;
	xbsvr_log_append(false, 0, "=== SERVER STOPPED ===");
}

int S9xXBandServerState (void) { return xbsvr_state; }
void S9xXBandServerTick (void)
{
	// Run in ALL active states including MATCHMAKING (for keepalives).
	if (xbsvr_state > XBSVR_OFF)
		xband_server_tick();
}
bool S9xXBandServerInterceptRX (void) { return xbsvr_intercept_rx; }

void S9xXBandServerLogDump (char *out, size_t out_size)
{
	if (!out || out_size == 0) return;
	size_t pos = 0;
	pos += snprintf(out + pos, out_size - pos,
		"XBAND Server Log (%d entries, state=%d)\n"
		"==========================================\n\n",
		xbsvr_log_count, xbsvr_state);
	int start = (xbsvr_log_head - xbsvr_log_count + XBSVR_LOG_SIZE)
	            % XBSVR_LOG_SIZE;
	for (int i = 0; i < xbsvr_log_count && pos + 200 < out_size; i++)
	{
		int idx = (start + i) % XBSVR_LOG_SIZE;
		XBandServerLogEntry *e = &xbsvr_log[idx];
		pos += snprintf(out + pos, out_size - pos,
			"  %s $%02X  %s\n",
			e->is_tx ? "TX" : "RX",
			(unsigned)e->opcode, e->text);
	}
}

// Compat shims for old fake server API (used by menu handler).
void S9xXBandFakeServerStart (void) { S9xXBandServerStart(); }
void S9xXBandFakeServerStop (void) { S9xXBandServerStop(); }
int  S9xXBandFakeServerState (void) { return S9xXBandServerState(); }

// Force-prime the ADSP sniffer state so injects work even without a
// real server connection. Uses sensible defaults (bsnes-plus connID
// $08C8, seq 0, generous window). Call this before any inject when
// the sniffer state hasn't been populated (box: ?? in the kctl trace).
static void xband_force_prime_sniff_state (void)
{
	if (!xband_sniff_box_seen)
	{
		xband_sniff_box_conn_id   = 0x08C8;
		xband_sniff_box_first_seq = 0;
		xband_sniff_box_next_recv = 0;
		xband_sniff_box_recv_win  = 0x7FFF;
		xband_sniff_box_data_total = 0;
		xband_sniff_box_seen      = true;
	}
	if (!xband_sniff_srv_seen)
	{
		xband_sniff_srv_conn_id   = 0x08C8;
		xband_sniff_srv_first_seq = 0;
		xband_sniff_srv_next_recv = 0;
		xband_sniff_srv_recv_win  = 0x7FFF;
		xband_sniff_srv_data_total = 0;
		xband_sniff_srv_seen      = true;
	}
	// The BIOS only polls Fred reg $98 (kreadmstatus2) for RX data
	// when net_step > 0. Without a real TCP connection, net_step
	// stays at IDLE and our injected bytes sit unread in the rxbuf.
	// Force CONNECTED so the BIOS will actually read our inject.
	if (XBand.net_step == XBAND_NET_IDLE)
		XBand.net_step = XBAND_NET_CONNECTED;
}

// Inject a complete "login + matchmaking" server response in one shot.
// This bundles everything the server would send after the BIOS dumps
// its initial data stream: login confirmation, the NGP game list with
// SSF2 Japan ($F72B5D1A), and a wait-for-opponent command.
//
// The BIOS sends msBoxType + msLogin + msGAMEIDAndPatchVersion +
// msChallengeRequest and then waits. This reply tells it: "login OK,
// SSF2 is a supported game, please wait for an opponent." That should
// trigger the "Would you like to practice <game> while XBAND searches
// for an opponent?" screen.
//
// Message sequence (all RX, server→box):
//   1. msSetDateAndTime      ($04) — login ack
//   2. msSetCurrentUserNumber($3E) — select profile 0
//   3. msReceiveValidationToken($3B) — token
//   4. msClearSendQ          ($17) — stop sending data to server
//   5. msEndOfStream         ($02) — end login batch
//   --- second batch ---
//   6. msNewNGPList          ($0F) — SSF2 in the available games list
//   7. msEndOfStream         ($02) — end batch
//   --- third batch ---
//   8. msWaitForOpponent     ($1C) — enter matchmaking
//   9. msEndOfStream         ($02) — end batch
bool S9xXBandFakeInjectMatchmaking (void)
{
	// Force-prime ADSP state so inject doesn't fail with
	// "no box frames sniffed yet".
	xband_force_prime_sniff_state();

	bool ok = true;

	// === BATCH 1: Login reply ===
	{
		uint8 body[64];
		int o = 0;
		// msSetDateAndTime ($04) + 4 bytes date + 5 bytes time
		body[o++] = 0x04;
		body[o++] = 0x00; body[o++] = 0x1A; // year 2026
		body[o++] = 0x35; body[o++] = 0x00; // April 10
		body[o++] = 0x00; body[o++] = 0x00;
		body[o++] = 0x00; body[o++] = 0x00; // time: midnight
		// msSetCurrentUserNumber ($3E) + 1 byte profile
		body[o++] = 0x3E;
		body[o++] = 0x00;
		// msReceiveValidationToken ($3B) + 4 zero bytes
		body[o++] = 0x3B;
		body[o++] = 0x00; body[o++] = 0x00;
		body[o++] = 0x00; body[o++] = 0x00;
		// msClearSendQ ($17)
		body[o++] = 0x17;
		// msEndOfStream ($02)
		body[o++] = 0x02;
		if (!S9xXBandFakeInject(body[0], body + 1, o - 1))
			ok = false;
	}

	// === BATCH 2: NGP game list with SSF2 Japan ===
	{
		uint8 body[128];
		int o = 0;
		// msNewNGPList ($0F)
		// Format from sample_packets.txt:
		//   Opcode + LenToNextLen(short) + Count(short) + Version(short)
		//   + GameID(long) + Gameflags(long) + PatchVersion(long)
		//   + TitleLen(short) + Title(bytes)
		body[o++] = 0x0F;
		// LenToNextLen = 16 (covers Count+Version+GameID+Flags+PatchVer)
		body[o++] = 0x00; body[o++] = 0x10;
		// Count = 1
		body[o++] = 0x00; body[o++] = 0x01;
		// Version = 1
		body[o++] = 0x00; body[o++] = 0x01;
		// GameID = $D8222103 (SSF2 Japan — must match the GameID
		// inside the SSF2.JSNES patch file header, NOT the BIOS's
		// internal cart-id $F72B5D1A which is a different identifier).
		body[o++] = 0xD8; body[o++] = 0x22;
		body[o++] = 0x21; body[o++] = 0x03;
		// Gameflags = 0
		body[o++] = 0x00; body[o++] = 0x00;
		body[o++] = 0x00; body[o++] = 0x00;
		// PatchVersion = 1
		body[o++] = 0x00; body[o++] = 0x00;
		body[o++] = 0x00; body[o++] = 0x01;
		// TitleLen = 24
		body[o++] = 0x00; body[o++] = 0x18;
		// Title = "Super Street Fighter 2\0" (24 bytes padded)
		const char *title = "Super Street Fighter 2";
		int tlen = (int)strlen(title);
		for (int i = 0; i < 24; i++)
			body[o++] = (i < tlen) ? (uint8)title[i] : 0x00;
		// msEndOfStream ($02)
		body[o++] = 0x02;
		if (!S9xXBandFakeInject(body[0], body + 1, o - 1))
			ok = false;
	}

	// === BATCH 3: SSF2 game patch (msGamePatch, ~3.3 KB chunked) ===
	// The patch file SSF2.JSNES is a complete ServerTalk message:
	//   $03 (opcode) + 4-byte GameID + patch version + type + length +
	//   controller-injection code + $02 (msEndOfStream).
	// Load from BIOS_DIR and inject via the chunked path.
	{
		std::string path = S9xGetDirectory(BIOS_DIR);
		path += SLASH_STR;
		path += "SSF2.JSNES";

		uint8 patch[8192];
		int patch_len = xband_read_file(path.c_str(), patch,
		                                (int)sizeof(patch));
		if (patch_len > 0)
		{
			int segments = xband_fake_inject_chunked(patch, patch_len,
			                                          "SSF2.JSNES");
			if (segments <= 0)
				ok = false;
		}
		// If SSF2.JSNES not found, continue anyway — the login + NGP
		// messages might be enough to advance the BIOS.
	}

	// === BATCH 4: Wait for opponent (matchmaking) ===
	{
		uint8 body[8];
		int o = 0;
		// msWaitForOpponent ($1C)
		body[o++] = 0x1C;
		// msEndOfStream ($02)
		body[o++] = 0x02;
		if (!S9xXBandFakeInject(body[0], body + 1, o - 1))
			ok = false;
	}

	return ok;
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
	xband_adsp_first_tx_frame_count = 0;
	memset(xband_adsp_first_tx_frame,     0, sizeof(xband_adsp_first_tx_frame));
	memset(xband_adsp_first_tx_frame_len, 0, sizeof(xband_adsp_first_tx_frame_len));

	// ServerTalk per-opcode counters and decoded log.
	memset(xband_servertalk_rx_count, 0, sizeof(xband_servertalk_rx_count));
	memset(xband_servertalk_tx_count, 0, sizeof(xband_servertalk_tx_count));
	xband_servertalk_rx_headeronly = 0;
	xband_servertalk_tx_headeronly = 0;
	memset(xband_servertalk_decoded, 0, sizeof(xband_servertalk_decoded));
	xband_servertalk_decoded_head  = 0;
	xband_servertalk_decoded_count = 0;

	// Stream reassembly buffers.
	memset(xband_rx_stream, 0, sizeof(xband_rx_stream));
	memset(xband_tx_stream, 0, sizeof(xband_tx_stream));
	xband_rx_stream_pos     = 0;
	xband_rx_stream_dropped = 0;
	xband_tx_stream_pos     = 0;
	xband_tx_stream_dropped = 0;

	// NOTE: we deliberately do NOT clear the sniffed connection state
	// here because the live connection (XBand.net_step / socket_fd)
	// outlives the dump windows. Resetting it would lose the
	// connID/seq numbers we need for the next inject. The sniff state
	// only resets at S9xResetXBand / S9xXBandConnect / Disconnect time.
	xband_fake_inject_count = 0;
	snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
	         "(none)");
	xband_rxbuf_bytes_consumed = 0;
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
		"  RX bytes consumed = %llu (BIOS popped from rxbuf via fred $94)\n"
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
		(unsigned long long)xband_tx_frames_sent,
		(unsigned long long)xband_rxbuf_bytes_consumed);

	// Sniffed ADSP connection state + fake-server inject status. The
	// connID and seq numbers must be populated before any inject can
	// succeed -- if they're zero, the BIOS hasn't talked to the server
	// yet (and our injects will use the wrong ConnID).
	static const char *const fred_modes[] = { "here", "plain", "softHere" };
	pos += snprintf(out + pos, out_size - pos,
		"Fred bus mode:\n"
		"  mode            = %s (kill $%02X, control $%02X)\n"
		"  switches        = %u\n"
		"  last switch     = %s\n"
		"  BIOS resets     = %u (last from PC=$%06X)\n"
		"\n",
		fred_modes[XBand.fred_mode % 3], XBand.kill, XBand.control,
		(unsigned)xband_fred_switches,
		xband_fred_last,
		(unsigned)xband_bios_resets, (unsigned)xband_bios_reset_pc);

	// kDispatcherVector log. Every JSL $E0:$0040 in BIOS code lands
	// in our trap (cpuexec.cpp) and gets logged here. We track UNIQUE
	// (caller, funcID) pairs in chronological order of first
	// occurrence with hit counters; idle loops don't flood the buffer
	// because each entry is stored only once. The early entries in
	// the buffer are the BIOS boot sequence -- find the first entry
	// that looks like the dialog/game-check there.
	if (xband_dispatch_total_calls > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"kDispatcherVector ($E0:$0040) call log:\n"
			"  total raw calls   = %llu\n"
			"  unique call sites = %u (max %u, overflow=%u)\n\n",
			(unsigned long long)xband_dispatch_total_calls,
			(unsigned)xband_dispatch_log_count,
			(unsigned)XBAND_DISPATCH_LOG_SIZE,
			(unsigned)xband_dispatch_overflow);

		// Top-10 funcID histogram (consumes the histogram buckets to
		// find the top entries; we restore zeros after).
		pos += snprintf(out + pos, out_size - pos,
			"  top-called function IDs:\n");
		uint64 saved_counts[10];
		int saved_ids[10];
		int saved_n = 0;
		for (int top = 0; top < 10; top++)
		{
			uint64 best = 0;
			int best_id = -1;
			for (int i = 0; i < XBAND_DISPATCH_FUNCID_BUCKETS; i++)
			{
				if (xband_dispatch_funcid_count[i] > best)
				{
					best = xband_dispatch_funcid_count[i];
					best_id = i;
				}
			}
			if (best_id < 0) break;
			pos += snprintf(out + pos, out_size - pos,
				"    funcID $%04X : %llu calls\n",
				(unsigned)best_id, (unsigned long long)best);
			saved_counts[saved_n] = best;
			saved_ids[saved_n] = best_id;
			saved_n++;
			xband_dispatch_funcid_count[best_id] = 0; // consume
		}
		// Restore so subsequent dumps still see them.
		for (int i = 0; i < saved_n; i++)
			xband_dispatch_funcid_count[saved_ids[i]] = saved_counts[i];

		// First-seen ordering: dump entries in the order they were
		// added to the buffer. Entries near the start are early-boot
		// calls; the show-dialog call should appear in there as a
		// LOW-hit-count entry (since the dialog is shown only once).
		pos += snprintf(out + pos, out_size - pos,
			"\n  unique call sites in first-seen order"
			" (low-hit entries are interesting):\n");
		for (uint32 i = 0; i < xband_dispatch_log_count &&
		                   pos + 200 < out_size; i++)
		{
			XBandDispatchEntry *e = &xband_dispatch_log[i];
			uint8 caller_bank = (uint8)((e->caller >> 16) & 0xFF);
			uint16 caller_addr = (uint16)(e->caller & 0xFFFF);
			pos += snprintf(out + pos, out_size - pos,
				"    [#%-4u hits=%-7u] caller=$%02X:$%04X"
				"  funcID=$%04X  A0=$%04X  Aend=$%04X\n",
				(unsigned)(i + 1),
				(unsigned)e->hits,
				(unsigned)caller_bank,
				(unsigned)caller_addr,
				(unsigned)e->func_id,
				(unsigned)e->a_first,
				(unsigned)e->a_last);
		}
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	pos += snprintf(out + pos, out_size - pos,
		"TX GameID spoofer ($0C $F7$2B$5D$1A -> $D8$22$21$03):\n"
		"  enabled         = %s\n"
		"  spoofs applied  = %u\n"
		"  last spoof      = %s\n"
		"\n",
		xband_tx_gameid_spoof ? "ON" : "OFF",
		(unsigned)xband_tx_gameid_spoof_count,
		xband_tx_gameid_spoof_last);

	pos += snprintf(out + pos, out_size - pos,
		"Sniffed ADSP state (used by fake-server injector):\n"
		"  box  : %s connID=$%04X first_seq=$%08X next_recv=$%08X "
		"win=$%04X data_total=%u\n"
		"  srv  : %s connID=$%04X first_seq=$%08X next_recv=$%08X "
		"win=$%04X data_total=%u\n"
		"  fake injects sent = %u\n"
		"  last inject       = %s\n"
		"  fake server state = %d (%s)\n"
		"\n",
		xband_sniff_box_seen ? "OK" : "??",
		(unsigned)xband_sniff_box_conn_id,
		(unsigned)xband_sniff_box_first_seq,
		(unsigned)xband_sniff_box_next_recv,
		(unsigned)xband_sniff_box_recv_win,
		(unsigned)xband_sniff_box_data_total,
		xband_sniff_srv_seen ? "OK" : "??",
		(unsigned)xband_sniff_srv_conn_id,
		(unsigned)xband_sniff_srv_first_seq,
		(unsigned)xband_sniff_srv_next_recv,
		(unsigned)xband_sniff_srv_recv_win,
		(unsigned)xband_sniff_srv_data_total,
		(unsigned)xband_fake_inject_count,
		xband_fake_inject_last,
		xbsvr_state,
		(xbsvr_state == XBSVR_OFF)              ? "OFF" :
		(xbsvr_state == XBSVR_HANDSHAKE)        ? "HANDSHAKE" :
		(xbsvr_state == XBSVR_WAIT_LOGIN)       ? "WAIT_LOGIN" :
		(xbsvr_state == XBSVR_INJECT_LOGIN_REPLY) ? "INJECT_LOGIN" :
		(xbsvr_state == XBSVR_WAIT_DATA)        ? "WAIT_DATA" :
		(xbsvr_state == XBSVR_INJECT_DATA_ACK)  ? "INJECT_DATA_ACK" :
		(xbsvr_state == XBSVR_INJECT_NGP)       ? "INJECT_NGP" :
		(xbsvr_state == XBSVR_WAIT_NGP)         ? "WAIT_NGP" :
		(xbsvr_state == XBSVR_INJECT_PATCH)     ? "INJECT_PATCH" :
		(xbsvr_state == XBSVR_WAIT_PATCH)       ? "WAIT_PATCH" :
		(xbsvr_state == XBSVR_INJECT_MATCHMAKING) ? "INJECT_MATCHMAKING" :
		(xbsvr_state == XBSVR_MATCHMAKING)      ? "MATCHMAKING" : "?");

	// Per-opcode RX/TX counter table. Shows every opcode that has been
	// seen at least once in either direction. Header-only frames (no
	// ServerTalk opcode) get their own row at the bottom so we can see
	// how many ADSP acks/window-updates the server is sending separate
	// from real data frames.
	{
		bool any_rx = false, any_tx = false;
		for (int op = 0; op < XBAND_SERVERTALK_OPCODES; op++)
		{
			if (xband_servertalk_rx_count[op]) any_rx = true;
			if (xband_servertalk_tx_count[op]) any_tx = true;
		}
		if (any_rx || any_tx ||
		    xband_servertalk_rx_headeronly ||
		    xband_servertalk_tx_headeronly)
		{
			pos += snprintf(out + pos, out_size - pos,
				"ServerTalk opcode counters (separate enums per direction):\n"
				"  RX = server -> BIOS (xband_post.txt)\n"
				"  TX = BIOS -> server (xbsega.go)\n"
				"NOTE: Continuation segments mis-label byte 13 as opcode.\n"
				"Real opcodes only appear at the START of a ServerTalk\n"
				"message; later segments carry stream bytes that look like\n"
				"random opcodes (high values like $6E/$BB/$E8/$FF are usually\n"
				"continuation noise, not real messages).\n\n");
			for (int op = 0; op < XBAND_SERVERTALK_OPCODES &&
			                 pos + 200 < out_size; op++)
			{
				if (!xband_servertalk_rx_count[op] &&
				    !xband_servertalk_tx_count[op])
					continue;
				pos += snprintf(out + pos, out_size - pos,
					"  $%02X RX=%llu (%-25s) TX=%llu (%s)\n",
					(unsigned)op,
					(unsigned long long)xband_servertalk_rx_count[op],
					xband_servertalk_name_server_to_box((uint8)op),
					(unsigned long long)xband_servertalk_tx_count[op],
					xband_servertalk_name_box_to_server((uint8)op));
			}
			if (xband_servertalk_rx_headeronly ||
			    xband_servertalk_tx_headeronly)
			{
				pos += snprintf(out + pos, out_size - pos,
					"  --  (header-only ADSP control)    RX=%llu TX=%llu\n",
					(unsigned long long)xband_servertalk_rx_headeronly,
					(unsigned long long)xband_servertalk_tx_headeronly);
			}
			pos += snprintf(out + pos, out_size - pos, "\n");
		}
	}

	// Reassembled per-direction ServerTalk stream view. This is the
	// best place to read off what the BIOS is actually sending /
	// receiving because it ignores ADSP segment boundaries (which
	// fragment messages) and shows the raw byte stream the way
	// xbsega.go's parser sees it.
	{
		uint32 rx_used = xband_rx_stream_pos - xband_rx_stream_dropped;
		if (rx_used > XBAND_STREAM_BUF_SIZE) rx_used = XBAND_STREAM_BUF_SIZE;
		uint32 tx_used = xband_tx_stream_pos - xband_tx_stream_dropped;
		if (tx_used > XBAND_STREAM_BUF_SIZE) tx_used = XBAND_STREAM_BUF_SIZE;

		if (rx_used > 0)
			pos += xband_stream_format(out + pos, out_size - pos,
				xband_rx_stream, rx_used, false /*is_tx*/);
		if (tx_used > 0)
			pos += xband_stream_format(out + pos, out_size - pos,
				xband_tx_stream, tx_used, true /*is_tx*/);
	}

	// Decoded message log: most recent N parsed messages from either
	// direction, in chronological order. Helps read off what the BIOS
	// is actually doing without paging through hex dumps.
	if (xband_servertalk_decoded_count > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"Most recent %d decoded ServerTalk messages:\n",
			xband_servertalk_decoded_count);
		// Walk the ring oldest-to-newest.
		int start_idx =
			(xband_servertalk_decoded_head -
			 xband_servertalk_decoded_count +
			 XBAND_SERVERTALK_DECODED_LOG) %
			XBAND_SERVERTALK_DECODED_LOG;
		for (int i = 0; i < xband_servertalk_decoded_count &&
		                pos + 200 < out_size; i++)
		{
			int idx = (start_idx + i) % XBAND_SERVERTALK_DECODED_LOG;
			XBandDecodedMessage *e = &xband_servertalk_decoded[idx];
			pos += snprintf(out + pos, out_size - pos,
				"  [%s op=$%02X len=%-3d] %s\n",
				e->is_tx ? "TX" : "RX",
				(unsigned)e->opcode, e->payload_len, e->text);
		}
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	// First N captured RX ADSP frames with parsed header + opcode.
	if (xband_adsp_first_frame_count > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"First %d RX ADSP frame(s) (server -> BIOS, deframed body):\n",
			xband_adsp_first_frame_count);
		for (int i = 0; i < xband_adsp_first_frame_count && pos + 500 < out_size; i++)
		{
			XBandParsedFrame p;
			bool ok = xband_servertalk_parse(
				xband_adsp_first_frame[i],
				xband_adsp_first_frame_len[i], &p);
			if (ok && p.has_opcode)
				pos += snprintf(out + pos, out_size - pos,
					"\nRX Frame #%d (%d bytes) connID=$%04X seq=$%08X "
					"recvSeq=$%08X win=$%04X desc=$%02X byte13=$%02X (%s)\n",
					i, xband_adsp_first_frame_len[i],
					(unsigned)p.source_conn_id,
					(unsigned)p.first_byte_seq,
					(unsigned)p.next_recv_seq,
					(unsigned)p.recv_window,
					(unsigned)p.descriptor,
					(unsigned)p.opcode,
					xband_servertalk_name_server_to_box(p.opcode));
			else if (ok)
				pos += snprintf(out + pos, out_size - pos,
					"\nRX Frame #%d (%d bytes) connID=$%04X seq=$%08X "
					"recvSeq=$%08X win=$%04X desc=$%02X (header-only ADSP)\n",
					i, xband_adsp_first_frame_len[i],
					(unsigned)p.source_conn_id,
					(unsigned)p.first_byte_seq,
					(unsigned)p.next_recv_seq,
					(unsigned)p.recv_window,
					(unsigned)p.descriptor);
			else
				pos += snprintf(out + pos, out_size - pos,
					"\nRX Frame #%d (%d bytes) too short to parse\n",
					i, xband_adsp_first_frame_len[i]);
			pos += xband_hex_ascii_dump(out + pos, out_size - pos,
				xband_adsp_first_frame[i], xband_adsp_first_frame_len[i]);
			if (xband_adsp_first_frame_len[i] >= 4)
			{
				uint16 expected = xband_adsp_first_frame_crc_expected[i];
				pos += snprintf(out + pos, out_size - pos,
					"  CRC expected = $%04X  (variant 5 [FALSE+\\x00] is canonical)\n",
					(unsigned)expected);
			}
		}
		pos += snprintf(out + pos, out_size - pos, "\n");
	}

	// First N captured TX ADSP frames with parsed header + opcode.
	if (xband_adsp_first_tx_frame_count > 0)
	{
		pos += snprintf(out + pos, out_size - pos,
			"First %d TX ADSP frame(s) (BIOS -> server, deframed body):\n",
			xband_adsp_first_tx_frame_count);
		for (int i = 0; i < xband_adsp_first_tx_frame_count && pos + 500 < out_size; i++)
		{
			XBandParsedFrame p;
			bool ok = xband_servertalk_parse(
				xband_adsp_first_tx_frame[i],
				xband_adsp_first_tx_frame_len[i], &p);
			if (ok && p.has_opcode)
				pos += snprintf(out + pos, out_size - pos,
					"\nTX Frame #%d (%d bytes) connID=$%04X seq=$%08X "
					"recvSeq=$%08X win=$%04X desc=$%02X byte13=$%02X (%s)\n",
					i, xband_adsp_first_tx_frame_len[i],
					(unsigned)p.source_conn_id,
					(unsigned)p.first_byte_seq,
					(unsigned)p.next_recv_seq,
					(unsigned)p.recv_window,
					(unsigned)p.descriptor,
					(unsigned)p.opcode,
					xband_servertalk_name_box_to_server(p.opcode));
			else if (ok)
				pos += snprintf(out + pos, out_size - pos,
					"\nTX Frame #%d (%d bytes) connID=$%04X seq=$%08X "
					"recvSeq=$%08X win=$%04X desc=$%02X (header-only ADSP)\n",
					i, xband_adsp_first_tx_frame_len[i],
					(unsigned)p.source_conn_id,
					(unsigned)p.first_byte_seq,
					(unsigned)p.next_recv_seq,
					(unsigned)p.recv_window,
					(unsigned)p.descriptor);
			else
				pos += snprintf(out + pos, out_size - pos,
					"\nTX Frame #%d (%d bytes) too short to parse\n",
					i, xband_adsp_first_tx_frame_len[i]);
			pos += xband_hex_ascii_dump(out + pos, out_size - pos,
				xband_adsp_first_tx_frame[i], xband_adsp_first_tx_frame_len[i]);
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
	xband_rxbuf_bytes_consumed++;
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

// -----------------------------------------------------------------------
// Fred II bus model (Catapult box source: SNESGameID.c, harddefines.h,
// SNESMemoryMap.h; plus the shipped SSF2 game patch)
// -----------------------------------------------------------------------
//   here      box ROM at $D0-$DF and bank 0's top 4KB, SRAM at $E0-$FF
//   plain     the cart everywhere
//   softHere  plain, plus kill/control at $00:4F00/$4F02 and, with the
//             zero-page hit enable, SRAM $FF00-$FFFF over $00:FF00-$FFFF
// A kill write arms the magic address; reading it enters here or plain by
// kill bit 0. A vector-area read ($00:FFE0+) in plain enters softHere.

static uint8 xband_reg_read (uint8 reg);
static void  xband_reg_write (uint8 reg, uint8 byte, uint32 address);

#define FRED_KILL_HERE		0x01
#define FRED_CTL_INTERNAL	0x08	// kill/control/regs in the ROM window's top 16KB
#define FRED_CTL_FIXED		0x10	// $FB:C000 registers stay visible outside here
#define FRED_HIT_ZEROPAGE	0x80	// reg $6D: the $00:FFxx vector page
#define FRED_VECTORS		11

struct FredMap
{
	uint8	mode, armed, control;
	uint32	magic, ram_start, ram_end, int_start;
	uint16	vec_on;
	uint32	vec[FRED_VECTORS];
};

static FredMap	fred_map;
static bool		fred_map_valid = false;
static bool		fred_cart_hirom = true;
static bool		fred_cart_gsu = false;
static uint8	*fred_gsu_rom = NULL;		// fxemu's view of the game: linear + doubled-32K mirrors
static uint8	*fred_under[MEMMAP_NUM_BLOCKS];		// ROM/RAM behind a trapped block
static bool		fred_under_ram[MEMMAP_NUM_BLOCKS];

static inline bool fred_active (void)
{
	return Settings.XBAND && Multi.cartType == 6 && Multi.cartSizeB;
}

// Fred holds SNES addresses as 24-bit word addresses.
static uint32 fred_word_addr (int r)
{
	return ((XBand.regs[r] | (XBand.regs[r + 1] << 8) | (XBand.regs[r + 2] << 16)) << 1) & 0xFFFFFF;
}

static uint32 fred_vtable (void)
{
	return (uint32) (XBand.regs[0x68] | (XBand.regs[0x69] << 8)) << 5;
}

static void fred_compute (FredMap &m)
{
	memset(&m, 0, sizeof(m));
	m.mode    = XBand.fred_mode;
	m.armed   = XBand.fred_armed;
	m.control = XBand.control & (FRED_CTL_INTERNAL | FRED_CTL_FIXED);
	m.magic   = fred_word_addr(0x38);

	// SafeRAM base/bound: 64-byte units of A21-A6; reg $78 bits 0-1 = A23-A22 (inferred).
	uint32 hi    = (uint32) (XBand.regs[0x78] & 3) << 22;
	uint32 base  = XBand.regs[0x60] | (XBand.regs[0x61] << 8);
	uint32 bound = XBand.regs[0x64] | (XBand.regs[0x65] << 8);
	if (bound > base)
	{
		m.ram_start = hi | (base << 6);
		m.ram_end   = hi | (bound << 6);
	}

	// ROM bound: 16KB units of A21-A14, inclusive; reg $78 bits 2-3 = A23-A22 (inferred).
	m.int_start = ((uint32) ((XBand.regs[0x78] >> 2) & 3) << 22) | ((uint32) XBand.regs[0x70] << 14);

	for (int n = 0; n < FRED_VECTORS; n++)
	{
		bool on = n < 8 ? (XBand.regs[0x6C] >> n) & 1 : (XBand.regs[0x6D] >> (n - 8)) & 1;
		if (on)
		{
			m.vec_on |= 1 << n;
			m.vec[n] = fred_word_addr(n * 4);
		}
	}
}

// Fred only decodes the cart bus: never WRAM, the PPU/CPU registers or $0000-$7FFF of the low banks.
static bool fred_on_cart_bus (uint32 addr)
{
	uint8 bank = addr >> 16;
	if (bank == 0x7E || bank == 0x7F)
		return false;
	return (addr & 0xFFFF) >= 0x8000 || (bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0;
}

// Route 4KB blocks through S9xGetXBand, remembering what they showed.
static void fred_trap_range (uint32 start, uint32 end, bool ram)
{
	for (uint32 a = start & ~(uint32) MEMMAP_MASK; a < end; a += MEMMAP_BLOCK_SIZE)
	{
		if (!fred_on_cart_bus(a & 0xFFFFFF))
			continue;
		uint32 b = (a & 0xFFFFFF) >> MEMMAP_SHIFT;
		uint8 *p = Memory.Map[b];
		if (p >= (uint8 *) CMemory::MAP_LAST)
		{
			fred_under[b] = p;
			fred_under_ram[b] = Memory.BlockIsRAM[b];
		}
		Memory.Map[b] = (uint8 *) (pint) CMemory::MAP_XBAND;
		Memory.BlockIsROM[b] = FALSE;
		Memory.BlockIsRAM[b] = ram;
	}
}

static inline uint8 *fred_gsu_ram (void)
{
	return Memory.SRAM + 0x20000;	// XBAND mode leaves Memory.SRAM unused
}

// Super FX board (Map_SuperFXLoROMMap, GSU1 layout): LoROM at $00-$3F:8000,
// linear at $40-$5F, work RAM at $70-$71; the rest of $40-$7D is open.
static void fred_map_gsu (uint32 bank_s, uint32 bank_e, uint32 addr_s, uint32 addr_e)
{
	uint8 *rom = Memory.ROM + Multi.cartOffsetB;
	for (uint32 c = bank_s; c <= bank_e; c++)
		for (uint32 i = addr_s; i <= addr_e; i += 0x1000)
		{
			uint32 p = (c << 4) | (i >> 12), b = c & 0x7f;
			uint8 *ptr = (uint8 *) CMemory::MAP_NONE;
			bool rom_blk = false, ram_blk = false;
			if (b < 0x40)
			{
				if (i < 0x8000)
					continue;
				ptr = rom + Memory.map_mirror(Multi.cartSizeB, b * 0x8000) - 0x8000;
				rom_blk = true;
			}
			else if (b < 0x60)
			{
				ptr = rom + Memory.map_mirror(Multi.cartSizeB, (b - 0x40) << 16);
				rom_blk = true;
			}
			else if (b == 0x70 || b == 0x71)
			{
				ptr = fred_gsu_ram() + ((b & 1) << 16);
				ram_blk = true;
			}
			Memory.Map[p] = ptr;
			Memory.BlockIsROM[p] = rom_blk;
			Memory.BlockIsRAM[p] = ram_blk;
		}
}

static void fred_map_cart (uint32 bank_s, uint32 bank_e, uint32 addr_s, uint32 addr_e)
{
	if (fred_cart_gsu)
	{
		fred_map_gsu(bank_s, bank_e, addr_s, addr_e);
		return;
	}
	if (fred_cart_hirom)
	{
		Memory.map_hirom_offset(bank_s, bank_e, addr_s, addr_e, Multi.cartSizeB, Multi.cartOffsetB);
		return;
	}
	// LoROM by the absolute bank ($C0 is cart bank $40): map_lorom_offset counts from bank_s,
	// which only agrees for carts of 2MB or less (NBA Jam TE is 3MB).
	for (uint32 c = bank_s; c <= bank_e; c++)
		for (uint32 i = addr_s; i <= addr_e; i += 0x1000)
		{
			uint32 p = (c << 4) | (i >> 12);
			Memory.Map[p] = Memory.ROM + Multi.cartOffsetB + Memory.map_mirror(Multi.cartSizeB, (c & 0x7f) * 0x8000) - (i & 0x8000);
			Memory.BlockIsROM[p] = TRUE;
			Memory.BlockIsRAM[p] = FALSE;
		}
}

// The game cart's battery RAM: the cart decodes it on its own bus. Only when its
// header has some - a cart without it must not find RAM there (SSF2's copier check).
static void fred_map_cart_sram (uint32 lorom_bank_s, uint32 lorom_bank_e)
{
	if (!Multi.sramSizeB || fred_cart_gsu)
		return;
	if (!fred_cart_hirom)
		Memory.map_index(lorom_bank_s, lorom_bank_e, 0x0000, 0x7fff, CMemory::MAP_LOROM_SRAM, CMemory::MAP_TYPE_RAM);
	else if (lorom_bank_s < 0x80)
	{
		Memory.map_index(0x20, 0x3f, 0x6000, 0x7fff, CMemory::MAP_HIROM_SRAM, CMemory::MAP_TYPE_RAM);
		Memory.map_index(0xa0, 0xbf, 0x6000, 0x7fff, CMemory::MAP_HIROM_SRAM, CMemory::MAP_TYPE_RAM);
	}
}

static void fred_remap (bool force)
{
	if (!fred_active())
		return;

	FredMap m;
	fred_compute(m);
	if (!force && fred_map_valid && !memcmp(&m, &fred_map, sizeof(m)))
		return;
	fred_map = m;
	fred_map_valid = true;
	memset(fred_under, 0, sizeof(fred_under));
	memset(fred_under_ram, 0, sizeof(fred_under_ram));

	fred_map_cart(0x00, 0x3f, 0x8000, 0xffff);
	fred_map_cart(0x40, 0x7d, 0x0000, 0xffff);
	fred_map_cart(0x80, 0xbf, 0x8000, 0xffff);
	fred_map_cart_sram(0x70, 0x7d);
	if (Settings.DSP == 1)
		Memory.map_DSP();	// the cart's own chip answers on its own bus
	if (fred_cart_gsu)
	{
		Memory.map_space(0x00, 0x3f, 0x6000, 0x7fff, fred_gsu_ram() - 0x6000);
		Memory.map_space(0x80, 0xbf, 0x6000, 0x7fff, fred_gsu_ram() - 0x6000);
	}

	if (m.mode == XBAND_FRED_HERE)
	{
		fred_map_cart(0xc0, 0xdf, 0x0000, 0xffff);
		Memory.map_hirom_offset(0xd0, 0xdf, 0x0000, 0xffff, Multi.cartSizeA, Multi.cartOffsetA);
		Memory.map_hirom_offset(0x50, 0x5f, 0x0000, 0xffff, Multi.cartSizeA, Multi.cartOffsetA);
		// Reset/NMI/IRQ come from the box ROM's vector page.
		Memory.map_hirom_offset(0x00, 0x00, 0xf000, 0xffff, Multi.cartSizeA, Multi.cartOffsetA);
		Memory.map_hirom_offset(0x80, 0x80, 0xf000, 0xffff, Multi.cartSizeA, Multi.cartOffsetA);
		Memory.map_index(0xe0, 0xfa, 0x0000, 0xffff, CMemory::MAP_XBAND, CMemory::MAP_TYPE_RAM);
		Memory.map_index(0xfb, 0xfb, 0x0000, 0xbfff, CMemory::MAP_XBAND, CMemory::MAP_TYPE_RAM);
		Memory.map_index(0xfc, 0xff, 0x0000, 0xffff, CMemory::MAP_XBAND, CMemory::MAP_TYPE_RAM);
		Memory.map_index(0xfb, 0xfb, 0xc000, 0xffff, CMemory::MAP_XBAND, CMemory::MAP_TYPE_I_O);
	}
	else
	{
		fred_map_cart(0xc0, 0xff, 0x0000, 0xffff);
		fred_map_cart_sram(0xf0, 0xff);		// $E0-$FF are the box's in here mode
		if (m.control & FRED_CTL_FIXED)
			Memory.map_index(0xfb, 0xfb, 0xc000, 0xffff, CMemory::MAP_XBAND, CMemory::MAP_TYPE_I_O);
		if (m.control & FRED_CTL_INTERNAL)
			fred_trap_range(m.int_start, m.int_start + 0x4000, false);
		fred_trap_range(0x00F000, 0x010000, false);		// vector page + exception space
	}
	if (m.ram_end > m.ram_start)
		fred_trap_range(m.ram_start, m.ram_end, true);
	if (m.armed && fred_on_cart_bus(m.magic))
		fred_trap_range(m.magic, m.magic + 1, false);
	if (m.mode != XBAND_FRED_HERE)
	{
		for (int n = 0; n < FRED_VECTORS; n++)
			if (m.vec_on & (1 << n))
				fred_trap_range(m.vec[n], m.vec[n] + 1, false);
	}

	Memory.map_WRAM();
	Memory.map_WriteProtectROM();
	S9xSetPCBase(Registers.PBPC);
}

static void fred_set_mode (uint8 mode, const char *why)
{
	static const char *const names[] = { "here", "plain", "softHere" };
	if (XBand.fred_mode != mode)
	{
		xband_fred_switches++;
		snprintf(xband_fred_last, sizeof(xband_fred_last), "%s -> %s at PC=$%06X",
			why, names[mode], (unsigned) (Registers.PBPC & 0xFFFFFF));
		XBand.fred_mode = mode;
	}
	fred_remap(false);
}

static void fred_kill_control_write (bool control, uint8 byte)
{
	if (control)
		XBand.control = byte;
	else
	{
		XBand.kill = byte;
		XBand.fred_armed = 1;
	}
	fred_remap(false);
}

static void fred_sram_write (uint32 offset, uint8 byte)
{
	offset &= XBAND_SRAM_SIZE - 1;
	if (XBand.sram[offset] != byte)
	{
		XBand.sram[offset] = byte;
		XBand.sram_dirty   = TRUE;
		CPU.SRAMModified   = TRUE;
	}
}

// Kill/control and the register file in the ROM window's top 16KB.
static inline bool fred_internal_hit (uint32 addr)
{
	return XBand.fred_mode != XBAND_FRED_HERE && (XBand.control & FRED_CTL_INTERNAL) &&
	       addr - fred_map.int_start < 0x400;
}

static inline bool fred_vector_page (uint32 addr)
{
	return XBand.fred_mode == XBAND_FRED_SOFTHERE && (XBand.regs[0x6D] & FRED_HIT_ZEROPAGE) &&
	       (addr & 0xFFFF00) == 0x00FF00;
}

static bool fred_read (uint32 addr, uint8 *out)
{
	if (XBand.fred_mode != XBAND_FRED_HERE && fred_map.vec_on)
	{
		for (int n = 0; n < FRED_VECTORS; n++)
		{
			if ((fred_map.vec_on >> n) & 1 && (addr & ~1u) == fred_map.vec[n])
			{
				*out = XBand.sram[(fred_vtable() + n * 2 + (addr & 1)) & (XBAND_SRAM_SIZE - 1)];
				return true;
			}
		}
	}

	if (XBand.fred_armed && (addr & ~1u) == fred_map.magic && fred_on_cart_bus(addr))
	{
		XBand.fred_armed = 0;
		fred_set_mode((XBand.kill & FRED_KILL_HERE) ? XBAND_FRED_HERE : XBAND_FRED_PLAIN, "magic");
	}
	else if (XBand.fred_mode == XBAND_FRED_PLAIN && (XBand.regs[0x6D] & FRED_HIT_ZEROPAGE) &&
	         (addr & 0xFFFFE0) == 0x00FFE0)
		fred_set_mode(XBAND_FRED_SOFTHERE, "vector");

	if (fred_vector_page(addr))
	{
		*out = XBand.sram[0xFF00 | (addr & 0xFF)];
		return true;
	}
	if (fred_internal_hit(addr))
	{
		uint32 o = addr - fred_map.int_start;
		*out = o < 0x200 ? ((o & 2) ? XBand.control : XBand.kill) : xband_reg_read((uint8) ((o - 0x200) >> 1));
		return true;
	}
	if (addr >= fred_map.ram_start && addr < fred_map.ram_end)
	{
		*out = XBand.sram[(addr - fred_map.ram_start) & (XBAND_SRAM_SIZE - 1)];
		return true;
	}
	if (uint8 *u = fred_under[addr >> MEMMAP_SHIFT])
	{
		*out = u[addr & 0xFFFF];
		return true;
	}
	return false;
}

static bool fred_write (uint32 addr, uint8 byte)
{
	if (fred_vector_page(addr))
	{
		fred_sram_write(0xFF00 | (addr & 0xFF), byte);
		return true;
	}
	if (fred_internal_hit(addr))
	{
		uint32 o = addr - fred_map.int_start;
		if (o < 0x200)
			fred_kill_control_write((o & 2) != 0, byte);
		else
			xband_reg_write((uint8) ((o - 0x200) >> 1), byte, addr);
		return true;
	}
	if (addr >= fred_map.ram_start && addr < fred_map.ram_end)
	{
		fred_sram_write(addr - fred_map.ram_start, byte);
		return true;
	}
	uint32 b = addr >> MEMMAP_SHIFT;
	if (fred_under[b])
	{
		if (fred_under_ram[b])
			fred_under[b][addr & 0xFFFF] = byte;
		return true;
	}
	return false;
}

// Registers that move windows, vectors or the magic address.
static void fred_reg_written (uint8 reg)
{
	if (reg <= 0x2A || (reg >= 0x38 && reg <= 0x3A) || (reg >= 0x60 && reg <= 0x65) ||
	    reg == 0x6C || reg == 0x6D || reg == 0x70 || reg == 0x74 || reg == 0x78)
		fred_remap(false);
}

// The loader armed chips from slot A's header (the BIOS); the game's DSP-1
// (Super Mario Kart) is classified as InitROM does, from slot B's.
static void fred_arm_cart_dsp (void)
{
	const uint32 base = fred_cart_hirom ? 0xFFB0 : 0x7FB0;
	if (Multi.cartSizeB < base + 0x30)
		return;
	const uint8 *hdr = Memory.ROM + Multi.cartOffsetB + base;
	const uint8 speed = hdr[0x25], type = hdr[0x26];
	const bool dsp1 = (type == 0x03 && speed != 0x30) ||
	                  (type == 0x05 && speed != 0x20 && !(speed == 0x30 && hdr[0x2a] == 0xb2));
	if (!dsp1)
		return;
	Settings.DSP = 1;
	if (fred_cart_hirom)
	{
		DSP0.boundary = 0x7000;
		DSP0.maptype = M_DSP1_HIROM;
	}
	else if (Multi.cartSizeB > 0x100000)
	{
		DSP0.boundary = 0x4000;
		DSP0.maptype = M_DSP1_LOROM_L;
	}
	else
	{
		DSP0.boundary = 0xc000;
		DSP0.maptype = M_DSP1_LOROM_S;
	}
	SetDSP = &DSP1SetByte;
	GetDSP = &DSP1GetByte;
}

// A Super FX game (DOOM) keeps its GSU and work RAM behind the box, as InitROM
// would arm them from slot B's LoROM header.
static void fred_arm_cart_gsu (void)
{
	fred_cart_gsu = false;
	if (Multi.cartSizeB < 0x8000)
		return;
	const uint8 *hdr = Memory.ROM + Multi.cartOffsetB + 0x7FB0;
	switch (hdr[0x25] | (hdr[0x26] << 8))
	{
		case 0x1320: case 0x1420: case 0x1520: case 0x1A20:
		case 0x1330: case 0x1430: case 0x1530: case 0x1A30:
			break;
		default:
			return;
	}

	if (!fred_gsu_rom && !(fred_gsu_rom = (uint8 *) malloc(FX_MEMORY_32K_MIRRORS + 0x400000)))
		return;
	const uint8 *rom = Memory.ROM + Multi.cartOffsetB;
	const uint32 size = Multi.cartSizeB;
	memset(fred_gsu_rom, 0xff, FX_MEMORY_32K_MIRRORS);
	for (uint32 o = 0; o < 0x400000; o += 0x8000)
		memcpy(fred_gsu_rom + o, rom + Memory.map_mirror(size, o), 0x8000);
	for (uint32 c = 0; c < 64; c++)
	{
		const uint8 *src = rom + Memory.map_mirror(size, c * 0x8000);
		memcpy(fred_gsu_rom + FX_MEMORY_32K_MIRRORS + c * 0x10000, src, 0x8000);
		memcpy(fred_gsu_rom + FX_MEMORY_32K_MIRRORS + c * 0x10000 + 0x8000, src, 0x8000);
	}

	fred_cart_gsu = true;
	fred_cart_hirom = false;
	SuperFX.pvRom = fred_gsu_rom;
	SuperFX.nRomBanks = (size > 0x200000 ? 0x200000 : size) >> 15;
	SuperFX.pvRam = fred_gsu_ram();
	SuperFX.nRamBanks = 2;
	SuperFX.isFx3 = FALSE;
	S9xInitSuperFX();
	Settings.SuperFX = TRUE;
	S9xResetSuperFX();
}

void S9xXBandFredRemap (void)
{
	if (!fred_active())
		return;
	fred_cart_hirom = Memory.ScoreHiROM(FALSE, Multi.cartOffsetB) >= Memory.ScoreLoROM(FALSE, Multi.cartOffsetB);
	fred_arm_cart_gsu();
	fred_arm_cart_dsp();
	fred_remap(true);
}

bool8 S9xXBandSoftReg (uint32 address, uint8 *byte, bool8 write)
{
	if (!fred_active() || XBand.fred_mode != XBAND_FRED_SOFTHERE || (address & 0xFFFFFC) != 0x004F00)
		return FALSE;
	bool control = (address & 2) != 0;
	if (write)
		fred_kill_control_write(control, *byte);
	else
		*byte = control ? XBand.control : XBand.kill;
	return TRUE;
}

// -----------------------------------------------------------------------
// Prepaid XBAND Card (Catapult SmartCard.c / SmartCardPriv.h)
// -----------------------------------------------------------------------
// A Gemplus GPM103: 104 bits read MSB-first a clock at a time through Fred
// reg $80 (clk $01, reset $08, vcc $10) and status reg $84 (bit 0 detect,
// bit 1 data). Bits 0-31 carry the "real debit card" ID, 32-63 the serial,
// 72-103 the octal credit counter C3..C0 (credits = sum of ones * 8^stage).
// Rising clock: with reset high the address returns to 0, else it advances;
// after a reset pulse with no clock inside it, the next rising clock writes
// the addressed counter bit to 0, or, if it already is, refills the next
// lower stage (the carry of WriteCarry103).

#define XBAND_CARD_BITS		104
#define XBAND_CARD_COUNTER	72

struct XBandCard
{
	bool	inserted;
	uint8	bits[XBAND_CARD_BITS / 8];
	int		addr;
	uint8	ctl;
	bool	reset_clean;	// reset is high and no clock has risen during it
	bool	write_armed;
};

static XBandCard xband_card;

static std::string xband_card_path (void)
{
	return S9xGetDirectory(SRAM_DIR) + SLASH_STR + "XBAND Prepaid Card.bin";
}

static bool xband_card_bit (int n)
{
	return (xband_card.bits[n >> 3] >> (7 - (n & 7))) & 1;
}

static void xband_card_save (void)
{
	if (FILE *f = fopen(xband_card_path().c_str(), "wb"))
	{
		fwrite(xband_card.bits, 1, sizeof(xband_card.bits), f);
		fclose(f);
	}
}

// A new card: Gemplus/Catapult debit ID $0BDF04, serial 12345,
// 100 credits (C2 one bit = 64, C1 four = 32, C0 four = 4).
static void xband_card_fresh (void)
{
	static const uint8 fresh[XBAND_CARD_BITS / 8] = {
		0x00, 0x0B, 0xDF, 0x04,  0x00, 0x00, 0x30, 0x39,  0xFF,
		0x00, 0x01, 0x0F, 0x0F
	};
	memcpy(xband_card.bits, fresh, sizeof(fresh));
}

// Read once; every write saves, so memory stays the file's copy. No file = a new card.
static void xband_card_load (void)
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;
	FILE *f = fopen(xband_card_path().c_str(), "rb");
	if (f)
	{
		bool ok = fread(xband_card.bits, 1, sizeof(xband_card.bits), f) == sizeof(xband_card.bits);
		fclose(f);
		if (ok)
			return;
	}
	xband_card_fresh();
}

static void xband_card_write (int n)
{
	if (n < XBAND_CARD_COUNTER || n >= XBAND_CARD_BITS)
		return;		// the ID and serial are fused
	if (xband_card_bit(n))
		xband_card.bits[n >> 3] &= ~(0x80 >> (n & 7));
	else if ((n >> 3) + 1 < XBAND_CARD_BITS / 8)
		xband_card.bits[(n >> 3) + 1] = 0xFF;
	xband_card_save();
}

static void xband_card_control (uint8 v)
{
	const uint8 prev = xband_card.ctl;
	xband_card.ctl = v;
	if (!xband_card.inserted || !(v & 0x10))
		return;

	if (!(prev & 0x08) && (v & 0x08))
		xband_card.reset_clean = true;
	if ((prev & 0x08) && !(v & 0x08) && xband_card.reset_clean && !(v & 0x01))
		xband_card.write_armed = true;

	if (!(prev & 0x01) && (v & 0x01))
	{
		if (v & 0x08)
		{
			xband_card.addr = 0;
			xband_card.reset_clean = false;
			xband_card.write_armed = false;
		}
		else if (xband_card.write_armed)
		{
			xband_card_write(xband_card.addr);
			xband_card.write_armed = false;
		}
		else if (xband_card.addr < XBAND_CARD_BITS - 1)
			xband_card.addr++;
	}
}

static uint8 xband_card_status (void)
{
	if (!xband_card.inserted)
		return 0x00;
	return 0x01 | (xband_card_bit(xband_card.addr) ? 0x02 : 0x00);
}

bool8 S9xXBandCardInserted (void)
{
	return xband_card.inserted;
}

void S9xXBandInsertCard (bool8 insert)
{
	if (insert && !xband_card.inserted)
	{
		xband_card_load();
		xband_card.addr = 0;
		xband_card.reset_clean = xband_card.write_armed = false;
	}
	xband_card.inserted = insert;
}

void S9xXBandResetCard (void)
{
	xband_card_load();		// memory, not an older file, holds the card from here on
	xband_card_fresh();
	xband_card_save();
	xband_card.addr = 0;
	xband_card.reset_clean = xband_card.write_armed = false;
}

int S9xXBandCardCredits (void)
{
	xband_card_load();
	int credits = 0, weight = 1;
	for (int stage = 3; stage >= 0; stage--, weight *= 8)
		for (int b = 0; b < 8; b++)
			credits += xband_card_bit(XBAND_CARD_COUNTER + stage * 8 + b) ? weight : 0;
	return credits;
}

// Fred register file read; reg = register byte address / 2 (A0 ignored).
static uint8 xband_reg_read (uint8 reg)
{
	uint8 result = 0x00;

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
		// kSStatus: bit 0 card detect, bit 1 the card's data line.
		result = xband_card_status();
	}
	else if (reg >= 0xC0)
	{
		// Rockwell modem register file at modem_reg = reg - $C0.
		// Some return values are "magic constants" the BIOS expects
		// to see during boot-time cart-detection (per the
		// commented-out mcu_access in bsnes-plus xband_gameplay
		// xband_cart.cpp). Without these, the BIOS shows
		// "This game may not be available" even with a supported
		// cart loaded.
		uint8 modemreg = (uint8)(reg - 0xC0);
		uint8 ret = 0;
		switch (modemreg)
		{
			case 0x04:
				// bsnes "$188 -> 0x00" -- return 0 (default)
				ret = 0x00;
				break;
			case 0x09:
				// bsnes "$192 -> 0xff". Previously we returned the
				// last-written value which was usually 0; the BIOS
				// expects 0xff here for a valid XBAND state.
				ret = 0xFF;
				break;
			case 0x0B:
				// TONEA (bit 7): dial tone until a call is up; in a call it would
				// be the call-waiting bong (PUListenToLine) and pause the game.
				// A dial whose connect is still pending rings out: no dial tone, or the BIOS hangs up in ~2 s.
				ret = (XBand.net_step == XBAND_NET_CONNECTED || xband_connecting) ? 0x00 : 0x80;
				// No answer tone while the opponent's line is still ringing.
				if (XBand.modem_set_ATV25 && xband_far_end_up && !xband_atv25_until)
				{
					xband_atv25_until = xband_frame + XBAND_ATV25_FRAMES;
					XBand.modem_set_ATV25 = 0;
				}
				if (xband_atv25_until && (int32) (xband_atv25_until - xband_frame) > 0)
					ret |= (1 << 4); // ATV25
				else if (xband_atv25_until)
					xband_dial_done = true;		// answer tone over: the modems are training, then connected
				break;
			case 0x0D:
				ret |= (1 << 3); // U1DET
				if (xband_answered)
					ret |= (1 << 5); // S1DET: the answering side hears the caller's S1
				break;
			case 0x0E:
				ret |= 3; // k2400Baud
				break;
			case 0x0F:
				ret |= (1 << 7) | (1 << 5); // RLSD + CTS — "modem alive"
				if (xband_ringing)
					ret |= (1 << 3); // RI
				break;
			case 0x18:
				// bsnes "$1b0 -> 0xff" -- not previously handled.
				ret = 0xFF;
				break;
			case 0x19: // X-RAM Data / "For running XBAND"
				// bsnes "$1b2 -> 0x46". We initialize modem_regs[$19]
				// to $46 in S9xResetXBand so the previous read-as-
				// last-written approach also returned $46 by default.
				// Make it explicit so it survives writes.
				ret = 0x46;
				break;
			case 0x1C:
				ret = XBand.modem_regs[0x1C];
				break;
			case 0x1D:
				ret = XBand.modem_regs[0x1D];
				break;
			case 0x1E:
				// bsnes "$1bc -> 0x08". Bit 3 is TDBE (transmitter
				// data buffer empty). We OR our last-written value
				// with bit 3 so TDBE is always asserted.
				ret = XBand.modem_regs[0x1E] | (1 << 3);
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

	return result;
}

uint8 S9xGetXBand (uint32 address)
{
	uint32 addr   = address & 0xFFFFFF;
	uint8  bank   = (addr >> 16) & 0xFF;
	uint16 offset =  addr        & 0xFFFF;
	uint8  result = 0x00;

	if (fred_active() && fred_read(addr, &result))
		return result;

	// XBAND SRAM mirror window (banks $E0-$FA, $FB:$0000-$BFFF,
	// $FC-$FF, $60-$7D — all aliasing the same 64KB).
	if (xband_in_sram(addr))
		result = XBand.sram[offset & (XBAND_SRAM_SIZE - 1)];
	// Fred + modem registers, mirrored every $200 up to the kill/control block
	else if (bank == XBAND_MMIO_BANK && offset >= 0xC000 && offset < 0xFC00)
		result = xband_reg_read((uint8)((offset - 0xC000) >> 1));
	else if (bank == XBAND_MMIO_BANK && offset >= 0xFC00)
	{
		result = (offset & 2) ? XBand.control : XBand.kill;
		S9xXBandKCtlLog(address, result, false);
	}

	xband_trace_log(address, result, false);
	return result;
}

// Fred register file write; the BIOS writes the even address, game
// patches the odd one or both (16-bit stores).
static void xband_reg_write (uint8 reg, uint8 byte, uint32 address)
{
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
					// Reset ADSP sniffer so the server detects
					// the next connection as a fresh handshake.
					xband_sniff_box_seen = false;
					xband_sniff_srv_seen = false;
					xband_fake_send_seq_primed = false;
					// If our server is running, reset to HANDSHAKE
					// so it handles the BIOS's next dial.
					if (xbsvr_state > XBSVR_OFF)
					{
						xbsvr_log_append(false, 0,
							"=== BIOS hung up, resetting for next dial ===");
						xbsvr_state = XBSVR_HANDSHAKE;
						xbsvr_poll_count = 0;
					}
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
					if (!xband_ring_answer())
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
			case 0x1B:
			case 0x1D:
				// YACC/XACC (bit 7) request a DSP RAM access; the DSP finishes it
				// at once and clears the bit (PModem.c WHILE_TIMEOUT_XACC waits on it).
				XBand.modem_regs[modemreg] = byte & 0x7F;
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
	if (reg == 0x80)
		xband_card_control(byte);
	// LED line 6 is the console's /RESET: enabled and driven low resets it.
	if ((reg == 0xB4 || reg == 0xB5) && (XBand.regs[0xB5] & 0x40) && !(XBand.regs[0xB4] & 0x40))
	{
		xband_reset_pending = true;
		xband_bios_resets++;
		xband_bios_reset_pc = Registers.PBPC & 0xFFFFFF;
	}
	fred_reg_written(reg);
}

void S9xSetXBand (uint8 byte, uint32 address)
{
	uint32 addr   = address & 0xFFFFFF;
	uint8  bank   = (addr >> 16) & 0xFF;
	uint16 offset =  addr        & 0xFFFF;

	xband_trace_log(address, byte, true);

	if (fred_active() && fred_write(addr, byte))
		return;

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

	if (bank == XBAND_MMIO_BANK && offset >= 0xC000 && offset < 0xFC00)
	{
		xband_reg_write((uint8)((offset - 0xC000) >> 1), byte, address);
		return;
	}
	if (bank == XBAND_MMIO_BANK && offset >= 0xFC00)
	{
		S9xXBandKCtlLog(address, byte, true);
		fred_kill_control_write((offset & 2) != 0, byte);
	}
}

uint8 *S9xGetBasePointerXBand (uint32 address)
{
	if (fred_active())
	{
		uint32 addr = address & 0xFFFFFF;
		if (addr >= fred_map.ram_start && addr < fred_map.ram_end)
			return (fred_map.ram_start & 0xFFFF) ? NULL : XBand.sram;
		// Trapped blocks and the modes' windows are fetched a byte at a time.
		if (fred_under[addr >> MEMMAP_SHIFT] || XBand.fred_mode != XBAND_FRED_HERE)
			return NULL;
	}
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
// BIOS loading
// -----------------------------------------------------------------------

// The released BIOS (CRC A8B868A0) says so only in its HiROM header; the
// other marks cover dumps carrying the Catapult name or a LoROM header.
bool8 S9xXBandIsBIOS (const uint8 *data, uint32 size)
{
	if (size < 0x10000)
		return (FALSE);
	auto find = [](const char *needle, const uint8 *hay, size_t hay_len) -> bool {
		const size_t n = strlen(needle);
		for (size_t i = 0; i + n <= hay_len; i++)
			if (memcmp(hay + i, needle, n) == 0)
				return (true);
		return (false);
	};
	if (find("CATAPULT", data, 0x400))
		return (TRUE);
	static const char *const marks[] = { "X-BAND", "X-Band", "XBAND" };
	for (const char *m : marks)
		if (find(m, data + 0x7FB0, 0x40) || find(m, data + 0xFFB0, 0x40))
			return (TRUE);
	return (FALSE);
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
	xband_seed_sram = true;
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
// User-selected SRAM dump filename (set via S9xXBandSetPreferredSRAM
// from the Win32 Netplay menu). When set, xband_load_sram_image tries
// this file first; otherwise it falls through to the default candidate
// list. Empty string means "auto-pick first available".
static char xband_preferred_sram[64] = {0};

void S9xXBandSetPreferredSRAM (const char *name)
{
	if (!name)
	{
		xband_preferred_sram[0] = 0;
		return;
	}
	strncpy(xband_preferred_sram, name, sizeof(xband_preferred_sram) - 1);
	xband_preferred_sram[sizeof(xband_preferred_sram) - 1] = 0;
}

const char *S9xXBandGetPreferredSRAM (void)
{
	return xband_preferred_sram[0] ? xband_preferred_sram : NULL;
}

static bool xband_load_sram_image (void)
{
	const char *default_candidates[] = {
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
	std::string used_path;

	// Try the user-selected file first.
	if (xband_preferred_sram[0])
	{
		std::string p = S9xGetDirectory(BIOS_DIR);
		p += SLASH_STR;
		p += xband_preferred_sram;
		f = fopen(p.c_str(), "rb");
		if (f)
			used_path = p;
	}

	// Fall back to default candidate list if no preferred file or
	// preferred file isn't there.
	for (int i = 0; default_candidates[i] != NULL && !f; i++)
	{
		std::string p = S9xGetDirectory(BIOS_DIR);
		p += SLASH_STR;
		p += default_candidates[i];
		f = fopen(p.c_str(), "rb");
		if (f)
			used_path = p;
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

// Public re-entry point: reload the SRAM image from the preferred (or
// default-fallback) file. Used by the Netplay menu when the user picks
// a different SRAM dump. Caller is responsible for triggering a SNES
// reset afterwards so the BIOS re-reads the new contents.
bool8 S9xXBandReloadSRAM (void)
{
	return xband_load_sram_image() ? TRUE : FALSE;
}

// Windows run from one folder each claim a free box save, "<box>.srm", "<box> [window 2].srm", ...,
// for their lifetime instead of overwriting one shared file; a new slot starts as a copy of the first.
#define XBAND_BOX_SLOTS 8
static int xband_box_slot = 0;	// 0 = not claimed yet

static std::string xband_box_slot_path (const char *srm_path, int slot)
{
	std::string path(srm_path);
	if (slot <= 1)
		return path;
	char suffix[24];
	snprintf(suffix, sizeof(suffix), " [window %d]", slot);
	const size_t dot = path.rfind('.'), sep = path.find_last_of("/\\");
	if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
		path += suffix;
	else
		path.insert(dot, suffix);
	return path;
}

static bool xband_box_slot_lock (const std::string &path)
{
#ifdef _WIN32
	// A named mutex per save file; Windows drops it when the process ends, crashed or not.
	uint32 hash = 2166136261u;
	for (char c : path)
		hash = (hash ^ (uint8) tolower((uint8) c)) * 16777619u;
	char name[64];
	snprintf(name, sizeof(name), "Local\\snes9x-xband-box-%08X", (unsigned) hash);
	HANDLE m = CreateMutexA(NULL, FALSE, name);
	if (!m)
		return true;	// can't tell; don't lock everyone out of the first slot
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(m);
		return false;
	}
	return true;		// held (not closed) for the life of the process
#elif defined(__unix__) || defined(__APPLE__)
	int fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR, 0644);
	if (fd < 0)
		return true;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		close(fd);
		return false;
	}
	return true;		// fd held for the life of the process
#else
	return true;
#endif
}

static std::string xband_box_path (const char *srm_path)
{
	if (!xband_box_slot)
	{
		xband_box_slot = 1;
		for (int slot = 1; slot <= XBAND_BOX_SLOTS; slot++)
			if (xband_box_slot_lock(xband_box_slot_path(srm_path, slot)))
			{
				xband_box_slot = slot;
				break;
			}
		if (xband_box_slot > 1)
		{
			std::string msg = "XBAND: this window's box saves to " + xband_box_slot_path(srm_path, xband_box_slot);
			S9xMessage(S9X_INFO, 0, msg.c_str());
		}
	}
	return xband_box_slot_path(srm_path, xband_box_slot);
}

bool8 S9xXBandLoadSRAM (const char *srm_path)
{
	const std::string box = xband_box_path(srm_path);
	FILE *f = fopen(box.c_str(), "rb");
	if (!f && xband_box_slot > 1)
		f = fopen(srm_path, "rb");	// a new slot starts as a copy of the first box
	if (!f)
		return FALSE;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	bool8 ok = FALSE;
	if (sz == XBAND_SRAM_SIZE || sz == XBAND_SRAM_SIZE + 512)
	{
		fseek(f, sz - XBAND_SRAM_SIZE, SEEK_SET);	// past a copier header
		ok = fread(XBand.sram, 1, XBAND_SRAM_SIZE, f) == XBAND_SRAM_SIZE;
	}
	fclose(f);
	if (ok)
		XBand.sram_dirty = FALSE;
	return ok;
}

bool8 S9xXBandSaveSRAM (const char *srm_path)
{
	// All zero: the BIOS never ran, so don't hide a BIOS-folder dump behind it.
	uint32 i = 0;
	while (i < XBAND_SRAM_SIZE && !XBand.sram[i])
		i++;
	if (i == XBAND_SRAM_SIZE)
		return TRUE;
	FILE *f = fopen(xband_box_path(srm_path).c_str(), "wb");
	if (!f)
		return FALSE;
	bool8 ok = fwrite(XBand.sram, 1, XBAND_SRAM_SIZE, f) == XBAND_SRAM_SIZE;
	fclose(f);
	if (ok)
		XBand.sram_dirty = FALSE;
	return ok;
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
	if (!xband_console_reset)
		memset(XBand.modem_regs, 0, sizeof(XBand.modem_regs));

	XBand.regs[0x7C] = 0;      // kAddrStatus
	XBand.regs[0x7D] = 0x80;   // read-constant, also seeded here
	XBand.regs[0xB4] = 0x7F;   // kLEDData
	XBand.regs[222]  = 8;      // UNKNOWN_REG2

	// From the Catapult _PUResetModem routine.
	XBand.modem_regs[0x19] = 0x46;

	XBand.kill    = 0;
	XBand.control = 0;

	// Fred powers up in here mode: the box boots, the cart stays hidden.
	XBand.fred_mode  = XBAND_FRED_HERE;
	XBand.fred_armed = 0;
	xband_fred_switches = 0;
	snprintf(xband_fred_last, sizeof(xband_fred_last), "(none)");
	fred_map_valid = false;
	fred_remap(true);

	// A match never continues a save: the game cart's battery starts blank at every
	// boot, so both boxes run the same game.
	if (fred_active() && Multi.sramSizeB)
		memset(Memory.SRAM, SNESGameFixes.SRAMInitialValue, (1 << (Multi.sramSizeB + 3)) * 128);

	xband_reset_pending = false;
	XBand.consecutive_reads = 0;

	// The modem and its call sit on the box, not the console: a /RESET the box
	// drives (_ResetCPU mid-game) keeps the line; power-on and user resets hang up.
	if (!xband_console_reset)
	{
		xband_hang_up();
		XBand.modem_line_relay  = 0;
		XBand.modem_set_ATV25   = 0;
		XBand.net_step          = XBAND_NET_IDLE;
		XBand.rxbufpos = XBand.rxbufused = 0;
		XBand.txbufpos = XBand.txbufused = 0;
	}

	// Drop sniffed ADSP connection state -- a fresh power-on / reset
	// implies any prior connID/seq numbers are stale. The dispatcher
	// will repopulate them as soon as the BIOS opens a new connection.
	xband_sniff_box_seen       = false;
	xband_sniff_srv_seen       = false;
	xband_sniff_box_data_total = 0;
	xband_sniff_srv_data_total = 0;
	xband_fake_inject_count    = 0;
	xband_fake_send_seq        = 0;
	xband_fake_send_seq_primed = false;
	xband_cartid_read_pc_count = 0;
	xband_cartid_write_pc_count = 0;
	memset(xband_cartid_read_pc_log, 0, sizeof(xband_cartid_read_pc_log));
	memset(xband_cartid_write_pc_log, 0, sizeof(xband_cartid_write_pc_log));
	snprintf(xband_fake_inject_last, sizeof(xband_fake_inject_last),
	         "(none)");

	// Seed from a real XBAND SRAM dump (e.g. one of the Cinghialotto
	// SNES-XBandSRAMs) once per game load; a saved .srm loaded after this
	// wins. Later resets keep the battery-backed SRAM, as the box does.
	if (xband_seed_sram)
	{
		xband_seed_sram = false;
		if (xband_load_sram_image())
			XBand.sram_dirty = FALSE;
	}
}

bool8 S9xXBandPendingReset (void)
{
	return xband_reset_pending;
}

// The BIOS pulled /RESET through the LED lines (SNESBoot.c reboot) and stopped.
void S9xXBandApplyReset (void)
{
	xband_reset_pending = false;
	xband_console_reset = true;
	S9xSoftReset();
	xband_console_reset = false;
}

void S9xXBandPostLoadState (void)
{
	// Don't touch the socket — a save state load implicitly "hangs up"
	// the modem, same as loading a state during snes9x netplay.
	XBand.socket_fd = XBAND_INVALID_SOCKET;
	XBand.connected = FALSE;
	// The bus layout follows the restored Fred mode and registers.
	fred_remap(true);
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
// xband_support branch). The ID `CToY7Enb` was issued to snes9x by the
// retrocomputing.network server's author (bsnes-plus ships `Waj04qaASNfmaRNw`).
// xband_identity_sends counter is declared above near the kctl trace
// state so the dump function can reference it.
static void xband_send_identity (xband_sock_t fd)
{
	static const char IDENTITY[] = "///////EMU-CToY7Enb\x0a";
	const int len = (int)(sizeof(IDENTITY) - 1);
	int sent = (int)send(fd, IDENTITY, len, XBAND_SEND_FLAGS);
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

// A non-blocking connect that is still under way (not an error).
static bool xband_connect_in_progress (void)
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

// Where a non-blocking connect stands: 1 connected, 0 still pending, -1 failed.
static int xband_connect_state (xband_sock_t fd)
{
	fd_set writable, failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(fd, &writable);
	FD_SET(fd, &failed);
	struct timeval now = { 0, 0 };
	const int n = select((int) fd + 1, NULL, &writable, &failed, &now);
	if (n == 0)
		return 0;
	int err = 0;
	socklen_t len = sizeof(err);
	if (n < 0 || FD_ISSET(fd, &failed) || getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &err, &len) != 0 || err)
		return -1;
	return 1;
}

// A connected, non-blocking, no-Nagle TCP socket, or XBAND_INVALID_SOCKET.
static intptr_t xband_open_socket (const char *host, int port)
{
#ifdef _WIN32
	if (!s_winsock_inited)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return XBAND_INVALID_SOCKET;
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
		return XBAND_INVALID_SOCKET;

	xband_sock_t fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if ((intptr_t)fd == XBAND_INVALID_SOCKET)
	{
		freeaddrinfo(result);
		return XBAND_INVALID_SOCKET;
	}

	// Disable Nagle — low-latency input exchange matters
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	// Connect without blocking: a server that doesn't answer used to stall emulation
	// ~21 s in connect(). The caller finishes it with xband_connect_state.
	xband_set_nonblocking(fd);
	if (connect(fd, result->ai_addr, (socklen_t)result->ai_addrlen) == XBAND_SOCKET_ERROR &&
	    !xband_connect_in_progress())
	{
		XBAND_CLOSESOCKET(fd);
		freeaddrinfo(result);
		return XBAND_INVALID_SOCKET;
	}

	freeaddrinfo(result);
	return (intptr_t)fd;
}

// This window's line on the local switchboard; opponents are rung by it.
static uint32 xband_line_id (void)
{
	static uint32 id = 0;
	while (!id)
		id = (uint32) std::chrono::steady_clock::now().time_since_epoch().count() ^
		     (uint32) (uintptr_t) &id ^ (uint32) time(NULL) * 2654435761u;
	return id;
}

static void xband_send_line (xband_sock_t fd, const char *what)
{
	char msg[32];
	snprintf(msg, sizeof(msg), "%s %08X\n", what, (unsigned) xband_line_id());
	send(fd, msg, (int) strlen(msg), XBAND_SEND_FLAGS);
}

static void xband_ring_close (void)
{
	if (xband_ring_fd != XBAND_INVALID_SOCKET)
		XBAND_CLOSESOCKET(xband_ring_fd);
	xband_ring_fd  = XBAND_INVALID_SOCKET;
	xband_ringing  = false;
	xband_ring_len = 0;
	xband_ring_connecting = false;
}

// Between calls, keep a line open so the server can ring this window.
static void xband_ring_poll (void)
{
	if (!xband_local_switch())
	{
		xband_ring_close();
		return;
	}
	if (!xband_last_host[0] || XBand.socket_fd != XBAND_INVALID_SOCKET)
		return;

	if (xband_ring_fd == XBAND_INVALID_SOCKET)
	{
		if (xband_ring_retry)
		{
			xband_ring_retry--;
			return;
		}
		int port;
		const char *host = xband_server(&port);
		xband_ring_fd = xband_open_socket(host, port);
		if (xband_ring_fd == XBAND_INVALID_SOCKET)
		{
			xband_ring_retry = 300;
			return;
		}
		xband_ring_connecting = true;
		return;
	}

	if (xband_ring_connecting)
	{
		const int state = xband_connect_state((xband_sock_t) xband_ring_fd);
		if (state == 0)
			return;
		if (state < 0)
		{
			xband_ring_close();
			xband_ring_retry = 300;
			return;
		}
		xband_ring_connecting = false;
		xband_send_identity((xband_sock_t) xband_ring_fd);
		xband_send_line((xband_sock_t) xband_ring_fd, "RING");
		xband_ring_len = 0;
		return;
	}

	// Once it rings, the caller's bytes stay queued until the BIOS answers.
	while (!xband_ringing)
	{
		char c;
		int got = (int) recv((xband_sock_t) xband_ring_fd, &c, 1, 0);
		if (got == 1)
		{
			if (c != '\n')
			{
				if (xband_ring_len < (int) sizeof(xband_ring_line) - 1)
					xband_ring_line[xband_ring_len++] = c;
				continue;
			}
			xband_ring_line[xband_ring_len] = 0;
			xband_ring_len = 0;
			xband_ringing = !strcmp(xband_ring_line, "RING");
		}
		else
		{
			if (got == 0)
			{
				xband_ring_close();
				xband_ring_retry = 300;
			}
			break;
		}
	}

	// The caller gave up before the BIOS answered: the ringing stops.
	char c;
	if (xband_ringing && recv((xband_sock_t) xband_ring_fd, &c, 1, MSG_PEEK) == 0)
	{
		xband_ring_close();
		xband_ring_retry = 300;
	}
}

// RTS raised while ringing answers the call on the ring line.
static bool xband_ring_answer (void)
{
	if (!xband_ringing)
	{
		xband_ring_close();		// dialing out instead
		return false;
	}
	send((xband_sock_t) xband_ring_fd, "ANSWER\n", 7, XBAND_SEND_FLAGS);	// the caller hears the pickup
	XBand.socket_fd = xband_ring_fd;
	xband_ring_fd   = XBAND_INVALID_SOCKET;
	xband_ringing   = false;
	xband_answered  = true;
	xband_far_end_up = true;
	xband_call_local = true;
	xband_dial_done  = true;
	XBand.connected = TRUE;
	XBand.net_step  = XBAND_NET_CONNECTED;
	XBand.rxbufpos  = XBand.rxbufused = 0;
	XBand.txbufpos  = XBand.txbufused = 0;
	xband_sniff_box_seen = xband_sniff_srv_seen = false;
	return true;
}

bool8 S9xXBandConnect (const char *host, int port)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
		S9xXBandDisconnect();
	xband_ring_close();

	// Remember host/port so the BIOS retry loop can auto-reconnect.
	strncpy(xband_last_host, host, sizeof(xband_last_host) - 1);
	xband_last_host[sizeof(xband_last_host) - 1] = 0;
	xband_last_port = port;

	intptr_t sock = xband_open_socket(host, port);
	if (sock == XBAND_INVALID_SOCKET)
		return FALSE;

	// Until the connect completes the line rings out: no answer tone, TX held in txbuf.
	// The socket opens at the dial, while a real modem could hear nothing: a HELO\n
	// let through then sits ahead of the box's packet parser and shifts every frame.
	XBand.socket_fd = sock;
	xband_connecting = true;
	xband_dial_done = false;
	xband_far_end_up = false;
	xband_atv25_until = 0;
	return TRUE;
}

// The dial's connect completed: identify and bring the call up.
static void xband_call_established (xband_sock_t fd)
{
	XBand.connected = TRUE;
	xband_call_local = xband_local_switch();
	xband_far_end_up = !xband_call_local;	// the switchboard's first bytes answer

	// Fresh connection -- drop all sniffed ADSP state and the running
	// fake-server send_seq counter so a previous session's numbers
	// don't poison the new connection.
	xband_sniff_box_seen       = false;
	xband_sniff_srv_seen       = false;
	xband_sniff_box_data_total = 0;
	xband_sniff_srv_data_total = 0;
	xband_fake_send_seq        = 0;
	xband_fake_send_seq_primed = false;

	// Send identity proactively immediately after connect, then drop
	// straight to CONNECTED so BIOS TX bytes flush on the very next
	// poll cycle. We CAN'T stay in HANDSHAKE waiting for server data
	// to "prove" the link is up, because the HELO\n filter discards
	// every probe byte before it reaches XBand.rxbuf — that means
	// the on-first-RX transition in S9xXBandPoll would never fire
	// against a pure-HELO server, and BIOS TX bytes would be stuck
	// in xband_txbuf forever.
	xband_send_identity(fd);
	if (xband_call_local)
		xband_send_line(fd, "LINE");
	XBand.net_step = XBAND_NET_CONNECTED;

	// Reset the HELO filter state so a previous attempt's partial
	// match doesn't bleed into this connection.
	xband_helo_match_pos = 0;
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

// The BIOS's dial (RTS) opens the connection, as dreampi does on the modem's
// CONNECT: a socket opened ahead of the dial goes stale and the server
// ignores the BIOS's open requests on it.
static void xband_try_auto_reconnect (void)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET) return;
	int port;
	const char *host = xband_server(&port);
	if (S9xXBandConnect(host, port))
		xband_auto_reconnects++;
}

// Emulation -> XBAND picked another server: the idle ring line follows it.
void S9xXBandServerChanged (void)
{
	xband_ring_close();
	xband_ring_retry = 0;
}

void S9xXBandDisconnect (void)
{
	xband_hang_up();
	xband_ring_close();
}

static void xband_hang_up (void)
{
	xband_atv25_until = 0;
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
	{
		XBAND_CLOSESOCKET(XBand.socket_fd);
		XBand.socket_fd = XBAND_INVALID_SOCKET;
	}
	xband_connecting = false;
	xband_answered  = false;
	XBand.connected = FALSE;
	XBand.net_step  = XBAND_NET_IDLE;
	XBand.rxbufpos  = XBand.rxbufused = 0;
	XBand.txbufpos  = XBand.txbufused = 0;
}

void S9xXBandPoll (void)
{
	xband_frame++;
	xband_ring_poll();
	if (XBand.socket_fd == XBAND_INVALID_SOCKET)
		return;

	xband_sock_t fd = (xband_sock_t)XBand.socket_fd;

	// A dial still connecting: nothing to read or send yet. If it fails the socket
	// closes and the BIOS, hearing no answer, hangs up and redials as on a failed dial.
	if (xband_connecting)
	{
		const int state = xband_connect_state(fd);
		if (state == 0)
			return;
		xband_connecting = false;
		if (state < 0)
		{
			XBAND_CLOSESOCKET(fd);
			XBand.socket_fd = XBAND_INVALID_SOCKET;
			return;
		}
		xband_call_established(fd);
	}

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
			xband_far_end_up = true;
			xband_sock_rx_bytes++;
			if (xband_sock_rx_first_used < XBAND_SOCK_FIRST_SIZE)
				xband_sock_rx_first[xband_sock_rx_first_used++] = b;

			// ADSP frame detector — runs on every raw RX byte BEFORE
			// the HELO filter and BEFORE pushing to BIOS rxbuf. Tells
			// us the moment the server transitions from HELO probes
			// to real ADSP frames.
			xband_adsp_feed_byte(b);

			// HELO\n filter: always while the box is still dialing, and
			// for the whole call when switched on at runtime.
			if ((xband_helo_filter_enabled || !xband_dial_done) &&
			    b == (uint8)xband_helo_signature[xband_helo_match_pos])
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

			// Partial match followed by a non-matching byte (or the filter
			// just switched off) — flush what we provisionally absorbed
			// back into rxbuf, then handle the current byte normally.
			for (int i = 0; i < xband_helo_match_pos &&
			                XBand.rxbufpos < XBAND_RXBUF_SIZE; i++)
				XBand.rxbuf[XBand.rxbufpos++] = (uint8)xband_helo_signature[i];
			xband_helo_match_pos = 0;

			if (XBand.rxbufpos < XBAND_RXBUF_SIZE)
				XBand.rxbuf[XBand.rxbufpos++] = b;
		}
		else if (got == 0)
		{
			// Clean server-side close. If our event-driven server
			// is running, suppress the EOF — the BIOS doesn't need
			// to know the real server dropped. We keep the
			// connection appearing alive via our injected responses.
			if (xbsvr_state > XBSVR_OFF)
				break; // silently ignore EOF
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
				// The local switchboard also carries games' raw bytes: send them now.
				if (unsent < XBAND_TXBUF_SIZE / 2 && !xband_call_local)
					break;
				end = XBand.txbufpos;  // flush all
			}

			uint32 frame_len = end - start;
			bool   was_real_frame = frame_len >= 2 &&
			                        XBand.txbuf[end - 2] == 0x10 &&
			                        XBand.txbuf[end - 1] == 0x03;

			// TX rewriter: if the GameID spoofer is enabled, rebuild
			// the frame with $d8 22 21 03 instead of $f7 2b 5d 1a.
			// `rewrite_buf` holds the new wire bytes; `send_data` /
			// `send_len` point at either the rewritten or the original
			// frame depending on whether rewriting actually triggered.
			uint8 rewrite_buf[600];
			const uint8 *send_data = XBand.txbuf + start;
			int          send_len  = (int)frame_len;
			bool         rewrote   = false;
			if (xband_tx_gameid_spoof && was_real_frame)
			{
				int new_len = xband_tx_rewrite_gameid(
					XBand.txbuf + start, (int)frame_len,
					rewrite_buf, (int)sizeof(rewrite_buf));
				if (new_len > 0)
				{
					send_data = rewrite_buf;
					send_len  = new_len;
					rewrote   = true;
				}
			}

			// Process the frame LOCALLY first (capture + dispatch)
			// regardless of whether the socket send succeeds. This
			// ensures our server sees the BIOS's TX data even when
			// the real server has dropped the TCP connection.

			// Capture into the TX-first ring.
			for (int i = 0;
			     i < (int)frame_len &&
			     xband_sock_tx_first_used < XBAND_SOCK_FIRST_SIZE;
			     i++)
				xband_sock_tx_first[xband_sock_tx_first_used++] =
					XBand.txbuf[start + i];

			// Dispatch to ServerTalk parser + our server's TX watcher.
			if (was_real_frame)
			{
				xband_tx_frames_sent++;

				// Compute the deframed body shape (strip leading \x00 +
				// trailing \x10\x03) so both the per-frame capture and
				// the ServerTalk dispatcher see the same view as the RX
				// path. The dispatcher runs for every TX frame; the
				// capture only fills the first N slots.
				uint32 body_start = start;
				uint32 body_end   = end - 2;  // strip \x10\x03
				if (body_start < body_end &&
				    XBand.txbuf[body_start] == 0x00)
					body_start++;  // strip leading \x00
				int body_len = (int)(body_end - body_start);
				if (body_len < 0) body_len = 0;

				if (xband_adsp_first_tx_frame_count < XBAND_ADSP_FIRST_FRAMES)
				{
					int slot = xband_adsp_first_tx_frame_count++;
					int n = body_len;
					if (n > 256) n = 256;
					memcpy(xband_adsp_first_tx_frame[slot],
						XBand.txbuf + body_start, n);
					xband_adsp_first_tx_frame_len[slot] = n;
				}

				if (body_len > 0)
				{
					xband_servertalk_dispatch_tx(
						XBand.txbuf + body_start, body_len);
				}
			}

			// Send on the socket (best-effort, may fail if server dropped).
			if (XBand.socket_fd != (intptr_t)-2)
			{
				int sent = (int)send(fd, (const char *)send_data, send_len, XBAND_SEND_FLAGS);
				if (sent > 0)
					xband_sock_tx_bytes += sent;
			}

			// Advance txbufused over the ORIGINAL frame's bytes.
			// Must always advance regardless of send result to
			// prevent infinite loop.
			XBand.txbufused += frame_len;
		}
		// If buffer is fully drained, reset positions to start.
		if (XBand.txbufused >= XBand.txbufpos)
			XBand.txbufpos = XBand.txbufused = 0;
	}
}

// -----------------------------------------------------------------------
// XBAND Keyboard (SNES port 2)
// -----------------------------------------------------------------------
// Catapult's SNES keyboard firmware (catakybd.SRC v1.7SNES) behind the BIOS
// driver in SNESControls.c: PP7 ($4201.7) falling starts a transaction and
// every $4017 read clocks out one inverted D1:D0 pair, LSB first.

#define XBKBD_ID		0x78	// 'x'
#define XBKBD_QUEUE		15		// the byte count is 4 bits
#define XBKBD_STATE_VER	1

struct SXBandKbd
{
	uint8	pp7;			// last $4201.7 level
	uint8	active;			// a transaction is running
	uint8	clocks;			// $4017 clocks since PP7 fell
	uint8	find;			// PP7 high after the 2nd clock: ID-only transaction
	uint8	break_all;		// PP7 after the 3rd clock: breaks for every key
	uint8	caps_led;		// Caps Lock LED lit
	uint8	led_sample;		// PP7 after the 5th clock (low = LED on)
	uint8	count;			// bytes sent in this transaction
	uint8	len;
	uint8	queue[XBKBD_QUEUE];
};

static struct SXBandKbd	xbkbd;
static uint32	xbkbd_last_poll;
static bool		xbkbd_polled;
static bool		xbkbd_read_seen;

void S9xXBandKeyboardReset (bool8 power)
{
	if (power)
	{
		memset(&xbkbd, 0, sizeof(xbkbd));
		xbkbd_polled = false;
		xbkbd_read_seen = false;
	}

	// A reset only drives WRIO back to $FF; the keyboard keeps its state.
	xbkbd.pp7 = 1;
}

void S9xXBandKeyboardWRIO (uint8 byte)
{
	const uint8	pp7 = byte >> 7;

	if (xbkbd.pp7 && !pp7 && !xbkbd.active)
	{
		// Newly seen: the BIOS hot-plug path stores stack garbage as its key flags
		// (Alt/Ctrl/Caps held); modifier breaks clear them.
		if (!S9xXBandKeyboardPolled())
		{
			static const uint8	breaks[] = { 0xf0, 0x12, 0xf0, 0x59, 0xf0, 0x14, 0xf0, 0x11, 0xf0, 0x58 };
			if (xbkbd.len + sizeof(breaks) <= XBKBD_QUEUE)
			{
				memcpy(xbkbd.queue + xbkbd.len, breaks, sizeof(breaks));
				xbkbd.len += sizeof(breaks);
			}
		}

		xbkbd.active = 1;
		xbkbd.clocks = 0;
		xbkbd.find = 0;
		xbkbd.count = 0;
		xbkbd_last_poll = IPPU.TotalEmulatedFrames;
		xbkbd_polled = true;
	}

	xbkbd.pp7 = pp7;
}

static void xbkbd_finish (void)
{
	xbkbd.active = 0;
	if (xbkbd.find)
		return;

	xbkbd.len -= xbkbd.count;
	memmove(xbkbd.queue, xbkbd.queue + xbkbd.count, xbkbd.len);

	if (!xbkbd_read_seen)
	{
		xbkbd_read_seen = true;
		S9xSetInfoString("XBAND Keyboard connected");
	}

	const uint8	led = !xbkbd.led_sample;
	if (led != xbkbd.caps_led)
	{
		xbkbd.caps_led = led;
		S9xSetInfoString(led ? "XBAND Keyboard: Caps Lock on" : "XBAND Keyboard: Caps Lock off");
	}
}

uint8 S9xXBandKeyboardClock (void)
{
	if (!xbkbd.active)
		return (0);	// both lines idle high

	const uint8	k = xbkbd.clocks;
	uint8		d;

	if (k < 4)
	{
		if (k == 2)
			xbkbd.find = xbkbd.pp7;
		else
		if (k == 3 && !xbkbd.find)
			xbkbd.break_all = xbkbd.pp7;
		d = XBKBD_ID >> (k * 2);
	}
	else
	if (k < 6)
	{
		if (k == 4)
			xbkbd.count = xbkbd.len;
		else
			xbkbd.led_sample = xbkbd.pp7;
		d = xbkbd.count >> ((k - 4) * 2);
	}
	else
	{
		const int	i = k - 6;
		d = xbkbd.queue[i >> 2] >> ((i & 3) * 2);
	}

	xbkbd.clocks = k + 1;
	if (xbkbd.clocks == (xbkbd.find ? 4 : 6 + 4 * xbkbd.count))
		xbkbd_finish();

	return (~d & 3);
}

void S9xXBandKeyboardKey (uint16 key, bool8 down, bool8 repeat)
{
	const uint8	code = key & 0xff;
	const bool	ext = (key & XBAND_KEY_EXT) != 0;
	bool		always_break, repeats;

	if (ext || (code >= 0x86 && code <= 0x8d))	// arrows, pad buttons
		always_break = true, repeats = false;
	else
	if (code == 0x11 || code == 0x12 || code == 0x14 || code == 0x58 || code == 0x59 || code == 0x80 || code == 0x81)
		always_break = true, repeats = false;	// Alt, Shift, Ctrl, Caps, Open/Closed-X
	else
		always_break = false, repeats = true;

	uint8	seq[3];
	int		n = 0;

	if (down)
	{
		if (repeat && !repeats)
			return;
	}
	else
	if (!always_break && !xbkbd.break_all)
		return;

	if (ext)
		seq[n++] = 0xe0;
	if (!down)
		seq[n++] = 0xf0;
	seq[n++] = code;

	// The firmware drops keys once its buffer is full.
	if (xbkbd.len + n > XBKBD_QUEUE)
		return;

	memcpy(xbkbd.queue + xbkbd.len, seq, n);
	xbkbd.len += n;
}

bool8 S9xXBandKeyboardPolled (void)
{
	return (xbkbd_polled && IPPU.TotalEmulatedFrames - xbkbd_last_poll < 64);
}

size_t S9xXBandKeyboardStateSize (void)
{
	return (1 + sizeof(xbkbd));
}

void S9xXBandKeyboardStateSave (uint8 *buf)
{
	buf[0] = XBKBD_STATE_VER;
	memcpy(buf + 1, &xbkbd, sizeof(xbkbd));
}

void S9xXBandKeyboardStateLoad (const uint8 *buf, size_t size)
{
	if (buf && size == S9xXBandKeyboardStateSize() && buf[0] == XBKBD_STATE_VER)
	{
		memcpy(&xbkbd, buf + 1, sizeof(xbkbd));
		if (xbkbd.len > XBKBD_QUEUE)
			xbkbd.len = 0;
		return;
	}

	// No keyboard in the state: no transaction in flight, WRIO as saved.
	xbkbd.active = 0;
	xbkbd.pp7 = Memory.FillRAM[0x4201] >> 7;
}
