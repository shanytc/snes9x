/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * XBAND modem peripheral emulation.
 *
 * Implementation follows the bsnes-plus xband_support branch
 * (fresh-eggs/bsnes-plus). XBAND is a pass-through cartridge by Catapult
 * Entertainment (1994-1997) containing:
 *   - 1MB firmware ROM (XBAND BIOS)
 *   - 64KB SRAM (profiles, patches, mail, news, icons)
 *   - Rockwell RC2324DP 2400-baud modem
 *   - A custom "Fred" chip exposing a flat 224-byte register file that
 *     holds game-patch vectors, LED state, and various magic registers
 *
 * Memory map (SNES address space, standalone XBAND BIOS mode):
 *   $00-$3F:$8000-$FFFF   XBAND firmware ROM (HiROM)
 *   $80-$BF:$8000-$FFFF   XBAND firmware ROM (mirror)
 *   $C0-$FF:$0000-$FFFF   XBAND firmware ROM (HiROM, with mirroring)
 *   $E0:$0000-$FFFF       XBAND SRAM (64KB), via MAP_XBAND
 *   $FB:$C000-$FDFF       Fred + Rockwell modem MMIO, via MAP_XBAND
 *     $FBC000-$FBC17E     Fred general registers (2-byte stride, reg 0x00-0xBF)
 *     $FBC180-$FBC1BE     Rockwell modem registers (2-byte stride, modem 0x00-0x1F)
 *     $FBFE01             XBAND kill register
 *     $FBFE03             XBAND control register
 *
 * Register address decoding is at a 2-byte stride:
 *   reg = (offset - $C000) / 2
 * Writes to even addresses are ignored ("event/strobe" half).
 *
 * The modem data path is bridged to a TCP socket so the emulator can
 * connect to a replacement XBAND server (e.g. 16bit.retrocomputing.network
 * or xband.retrocomputing.network).
 */

#ifndef _XBAND_H_
#define _XBAND_H_

#include <cstdint>
#include <cstddef>

#define XBAND_ROM_SIZE		0x100000	// 1MB firmware ROM
#define XBAND_SRAM_SIZE		0x010000	// 64KB SRAM
#define XBAND_FRED_REGS		0xE0		// 224 Fred general registers
#define XBAND_MODEM_REGS	0x20		// 32 Rockwell modem registers
#define XBAND_RXBUF_SIZE	0x4000		// 16KB network rx buffer
#define XBAND_TXBUF_SIZE	0x4000		// 16KB network tx buffer

// MMIO region on the XBAND — the full upper half of bank $FB gets
// claimed by MAP_XBAND so the dispatch inside S9xGetXBand / S9xSetXBand
// can decode the Fred / modem / kill / control sub-ranges.
#define XBAND_MMIO_BANK		0xFB
#define XBAND_MMIO_BASE		0x8000
#define XBAND_MMIO_END		0xFFFF

// Network state machine (bsnes-plus net_step values)
#define XBAND_NET_IDLE		0
#define XBAND_NET_HANDSHAKE	1
#define XBAND_NET_CONNECTED	2

struct SXBAND
{
	// Enable / detect flags
	bool8	enabled;
	bool8	bios_loaded;
	bool8	connected;
	bool8	sram_dirty;

	// Fred general register file: holds patch vectors, LED state, etc.
	// Indexed by `reg = (addr - $FBC000) / 2` for reg < $C0.
	uint8	regs[XBAND_FRED_REGS];

	// Rockwell RC2324DP modem registers. Indexed by `reg - $C0`
	// (so $FBC180 is modem reg $00).
	uint8	modem_regs[XBAND_MODEM_REGS];

	// XBAND cartridge kill/control registers at $FBFE01 / $FBFE03.
	uint8	kill;
	uint8	control;

	// Modem state
	uint8	modem_line_relay;	// RTS bit from modem reg 0x07
	uint8	modem_set_ATV25;	// one-shot: next read of 0x0B sets ATV25
	uint8	net_step;			// XBAND_NET_*
	uint32	consecutive_reads;	// cap on kreadmstatus2 tight polls

	// Network RX/TX buffers (separate from modem regs — these are the
	// FIFO between the emulator's TCP socket and the XBAND firmware).
	uint8	rxbuf[XBAND_RXBUF_SIZE];
	uint32	rxbufpos;		// write position (bytes received from socket)
	uint32	rxbufused;		// read position (bytes consumed by firmware)
	uint8	txbuf[XBAND_TXBUF_SIZE];
	uint32	txbufpos;		// write position (bytes from firmware)
	uint32	txbufused;		// read position (bytes flushed to socket)

	// SRAM (player profiles, patches, mail, news, icons)
	uint8	sram[XBAND_SRAM_SIZE];

	// Network socket (platform-agnostic; -1 when disconnected).
	// Stored as intptr_t so it holds a Windows SOCKET without truncation
	// on 64-bit builds. Not serialized in save states.
	intptr_t	socket_fd;
};

extern struct SXBAND XBand;

// Core hardware interface (called from memory dispatch).
uint8	S9xGetXBand (uint32 address);
void	S9xSetXBand (uint8 byte, uint32 address);
uint8  *S9xGetBasePointerXBand (uint32 address);

// Lifecycle.
void	S9xInitXBand (void);
void	S9xResetXBand (void);
void	S9xXBandPostLoadState (void);
bool8	S9xLoadXBandBIOS (void);

// Mirror XBand.sram[] back into Memory.SRAM[] so snes9x's standard
// SaveSRAM picks up the current XBAND SRAM contents on shutdown.
void	S9xXBandSyncSRAMOut (void);

// User-selected SRAM dump filename for the BIOS_DIR loader. Set via
// the Win32 Netplay menu (XBAND: Use SRAM ...). Empty string =
// auto-pick from default candidate list.
void	S9xXBandSetPreferredSRAM (const char *name);
const char *S9xXBandGetPreferredSRAM (void);

// Reload the SRAM image from disk into XBand.sram[]. Caller should
// trigger a SNES reset afterwards so the BIOS re-reads the contents.
bool8	S9xXBandReloadSRAM (void);

// Network bridging.
bool8	S9xXBandConnect (const char *host, int port);
void	S9xXBandDisconnect (void);
void	S9xXBandPoll (void);

// Runtime toggle for the HELO\n RX filter. When `on`, HELO\n probes
// from the server are stripped before reaching the BIOS. When `off`,
// raw bytes pass through. Defaults to ON. Used by the GUI to A/B
// test which behavior the BIOS prefers.
void	S9xXBandSetHeloFilter (bool on);
bool	S9xXBandGetHeloFilter (void);

// Debug helper: write the last few MMIO accesses into `out` as a
// human-readable multi-line string. Used by the deadlock handler.
void	S9xXBandDumpTrace (char *out, size_t out_size);

// Temporarily stop logging XBAND accesses into the trace buffer, so
// diagnostic reads (e.g. from the deadlock dump) don't evict the
// actually-interesting entries.
void	S9xXBandTraceSuppress (bool on);

// Debug-only trace entry, used by the deadlock handler for per-site
// code dumps. Keep in sync with the internal definition in xband.cpp.
struct XBandTraceEntry {
	uint32	address;
	uint32	caller_pc;
	uint8	value;
	bool	is_write;
};

// Fetch the Nth-oldest trace entry (0 = oldest). Returns false when
// `index` is past the live entries.
bool	S9xXBandGetTraceEntry (int index, struct XBandTraceEntry *out);
#define XBAND_TRACE_SIZE 256

// -----------------------------------------------------------------------
// Kill / control register trace
// -----------------------------------------------------------------------
//
// Dedicated, narrowly-scoped trace for accesses to the candidate Fred
// "kill" and "control" register addresses. The XBAND BIOS uses these to
// switch the SNES bus between "BIOS visible" and "game cart visible"
// modes (see Catapult source xband_src/xband/gameid/gameid.c). bsnes-plus
// puts the registers at $FB:FE01/$FB:FE03; the Catapult fredequ.h has
// SNES-specific constants at $61:7000/$61:7001. We hook BOTH so we can
// see which the BIOS actually touches.
//
// The kctl trace is separate from the generic xband_trace ring so it
// doesn't get evicted by the firehose of unrelated MMIO accesses while
// the BIOS is running.
struct XBandKCtlEntry {
	uint32	pc;			// PB:PC of the instruction issuing the access
	uint32	address;	// full 24-bit address
	uint8	value;		// byte read or written
	bool	is_write;
};

#define XBAND_KCTL_TRACE_SIZE 128

void	S9xXBandKCtlLog (uint32 address, uint8 value, bool is_write);
void	S9xXBandKCtlDump (char *out, size_t out_size);
void	S9xXBandKCtlReset (void);

// "Silent write" trap: called from S9xSetByte for any write whose
// WriteMap entry is MAP_NONE while Settings.XBAND is set. These writes
// would otherwise vanish — the BIOS shouldn't be making them in normal
// operation, so any stray store is a strong hint that there's an
// unknown MMIO register sitting at that address. Logged into the same
// kctl dump.
void	S9xXBandSilentWriteLog (uint32 address, uint8 value);

// Per-Fred-register write counter. Bumped from S9xSetXBand for every
// Fred general / modem register write so we can see which registers
// the BIOS actually touches (separately from the noisy generic trace).
void	S9xXBandFredRegWriteBump (uint8 reg, uint8 value);

// "Cross-bank read" trap: called from S9xGetByte for any read whose
// program bank is inside BIOS code space ($D0-$DF or its mirror
// $50-$5F) but whose target bank is on the cart side ($00-$3F:$8000+,
// $40-$7D, $80-$BF:$8000+, $C0-$CF). When the BIOS code does a long
// read into cart-side address space, it expects to see GAME bytes —
// even if our current map gives it BIOS bytes. Logging these reads
// shows us exactly where the BIOS thinks the cart should be.
void	S9xXBandCrossBankReadLog (uint32 address, uint8 value);

#endif
