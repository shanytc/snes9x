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
	}
	else if (bank == XBAND_MMIO_BANK && offset == 0xFE03)
	{
		result = XBand.control;
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

		// Modem TX FIFO write at Fred reg $90 ($FBC120)
		if (reg == 0x90)
		{
			if (XBand.net_step == XBAND_NET_CONNECTED ||
			    XBand.net_step == XBAND_NET_HANDSHAKE)
			{
				xband_txbuf_push(byte);
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
						// to initiate a call. We don't auto-connect —
						// the UI does that explicitly via the Netplay
						// menu. Just mark the state machine as pending
						// so a subsequent connect() can enter handshake.
						XBand.net_step = XBAND_NET_HANDSHAKE;
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
		XBand.kill = byte;
		return;
	}
	if (bank == XBAND_MMIO_BANK && offset == 0xFE03)
	{
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
	bool loaded = xband_load_sram_image();
	if (loaded)
		XBand.sram_dirty = FALSE;

#ifdef _WIN32
	// One-shot popup so we can confirm the SRAM image actually loads.
	// Remove once XBAND boot reliably reaches a visible UI.
	{
		char msg[256];
		_snprintf(msg, sizeof(msg) - 1,
			"S9xResetXBand: SRAM dump load = %s\n\n"
			"First 16 bytes of XBand.sram:\n"
			"%02X %02X %02X %02X %02X %02X %02X %02X "
			"%02X %02X %02X %02X %02X %02X %02X %02X",
			loaded ? "OK" : "FAILED (using zeros)",
			XBand.sram[0],  XBand.sram[1],  XBand.sram[2],  XBand.sram[3],
			XBand.sram[4],  XBand.sram[5],  XBand.sram[6],  XBand.sram[7],
			XBand.sram[8],  XBand.sram[9],  XBand.sram[10], XBand.sram[11],
			XBand.sram[12], XBand.sram[13], XBand.sram[14], XBand.sram[15]);
		msg[sizeof(msg) - 1] = 0;
		MessageBoxA(NULL, msg, "XBAND Reset Diag", MB_OK);
	}
#endif
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
	// Enter the handshake phase; a real XBAND firmware raises RTS and
	// expects the server to reply. The firmware polls reg $98 for RX
	// data once it believes the modem is in V.22bis mode.
	XBand.net_step = XBAND_NET_HANDSHAKE;
	return TRUE;
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

	// Flush any buffered TX bytes to the socket. bsnes-plus sends each
	// byte at modem-write time; we accumulate and flush once per frame
	// so the socket isn't poked on every $FBC120 write.
	while (XBand.txbufused < XBand.txbufpos)
	{
		uint8 b = XBand.txbuf[XBand.txbufused];
		int sent = (int)send(fd, (const char *)&b, 1, 0);
		if (sent != 1)
			break; // socket would block or is broken
		XBand.txbufused++;
	}
	if (XBand.txbufused >= XBand.txbufpos)
		XBand.txbufpos = XBand.txbufused = 0;

	// Drain any bytes the server has sent into the RX buffer.
	while (XBand.rxbufpos < XBAND_RXBUF_SIZE)
	{
		uint8 b;
		int got = (int)recv(fd, (char *)&b, 1, 0);
		if (got == 1)
			XBand.rxbuf[XBand.rxbufpos++] = b;
		else
			break;
	}
}
