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
 *     $FBFC00/$FBFE00     XBAND kill register
 *     $FBFC02/$FBFE02     XBAND control register
 *
 * Register address decoding is at a 2-byte stride, A0 ignored:
 *   reg = (offset - $C000) / 2
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

// Fred II bus modes: here = box ROM + SRAM, cart hidden; plain = cart;
// softHere = cart plus the soft kill/control and the SRAM vector page.
#define XBAND_FRED_HERE		0
#define XBAND_FRED_PLAIN	1
#define XBAND_FRED_SOFTHERE	2

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

	// Fred kill/control registers ($FBFC00/$FBFC02, aliased at $FBFE00).
	uint8	kill;
	uint8	control;
	uint8	fred_mode;		// XBAND_FRED_*
	uint8	fred_armed;		// a kill write arms the next magic-address read

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

// Lays the bus out for the current Fred mode (BIOS + game cart loads only).
void	S9xXBandFredRemap (void);

// The BIOS resets the console through Fred's LED line 6; cpuexec applies it.
bool8	S9xXBandPendingReset (void);
void	S9xXBandApplyReset (void);

// The prepaid XBAND Card in the modem's smart-card slot (a Gemplus GPM103).
bool8	S9xXBandCardInserted (void);
void	S9xXBandInsertCard (bool8 insert);
void	S9xXBandResetCard (void);		// back to a new card's 100 credits
int		S9xXBandCardCredits (void);
// The softHere kill/control at $00:4F00/$4F02; false when not decoded.
bool8	S9xXBandSoftReg (uint32 address, uint8 *byte, bool8 write);
// A 1MB image carrying the XBAND BIOS marks; `size` is how much of it is in hand.
bool8	S9xXBandIsBIOS (const uint8 *data, uint32 size);

// The box's 64KB battery SRAM in the BIOS's .srm (Memory.LoadSRAM/SaveSRAM).
bool8	S9xXBandLoadSRAM (const char *srm_path);
bool8	S9xXBandSaveSRAM (const char *srm_path);

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
// Settings.XBANDLocalServer/Host/Port changed; takes effect on the next dial.
void	S9xXBandServerChanged (void);
void	S9xXBandPoll (void);

// Fake-server injection: synthesize an ADSP-framed ServerTalk reply
// with the given opcode + payload, sniff connID/seq from the live
// connection state, and push the wire bytes into XBand.rxbuf so the
// BIOS reads them as if they came from the socket. Used to drive the
// BIOS past the post-login "waiting for server data" wall when the
// live server isn't sending what the BIOS expects. Returns true on
// successful injection. Bool (not bool8) to match its C++ origin.
bool	S9xXBandFakeInject (uint8 opcode, const uint8 *payload, int payload_len);

// Toggle the connID source used by the fake-server injector. There
// are two interpretations of the SNES XBAND ADSP layer: either
// server frames carry the SERVER's source connID (Apple ADSP
// standard) or they carry the BOX's source connID (xbsega echo-back
// model). Default is server. Used by the menu A/B test.
void	S9xXBandFakeToggleConnIDSource (void);
const char *S9xXBandFakeConnIDSourceLabel (void);

// Inject a "post-login canned response" -- multiple ServerTalk
// messages bundled into one ADSP segment, mimicking what a real
// server would send after the box's login dump. Currently includes
// msSetDateAndTime, msSetCurrentUserNumber 0, msReceiveValidationToken,
// and msEndOfStream. Returns true on successful injection.
bool	S9xXBandFakeInjectLoginReply (void);

// Inject a "cart-supported" canned reply for the post-Challenge flow.
// After clicking Challenge, the BIOS sends msGAMEIDAndPatchVersion
// to the server and waits for confirmation that the game is in the
// supported list. This inject sends an empty msGamePatch (the in-game
// controller-input patch the BIOS would normally install) followed
// by msEndOfStream, telling the BIOS "yes, cart accepted, no patch
// needed". Should advance the BIOS into the matchmaking flow without
// needing a real per-game patch. Returns true on successful injection.
bool	S9xXBandFakeInjectGameSupported (void);

// Inject the real SSF2.JSNES game patch from BIOS_DIR. Reads the
// 3.3 KB patch file (from the Cinghialotto/xband repo) and chains
// it across multiple ADSP segments with sequential send_seq numbers
// so the BIOS reassembles it as a single msGamePatch ServerTalk
// message. Requires SSF2.JSNES to be present in win32/BIOS/.
// Returns true on successful injection.
bool	S9xXBandFakeInjectSSF2Patch (void);

// Inject a fake msNewNGPList that maps the BIOS's broken default
// cart hash ($F7 2B 5D 1A) to "Super Street Fighter II". Used to
// test whether the BIOS uses the NGP list as its supported-games
// table when deciding whether to show "not an XBAND Card".
bool	S9xXBandFakeInjectFakeNGPList (void);

// Search WRAM, XBAND SRAM, Fred regs, and modem regs for the
// BIOS's cached cart-id bytes ($F7 $2B $5D $1A). Writes a
// human-readable report into `out`. Used to find where the BIOS
// stores its computed cart-id so we can override it before the
// local Challenge cart-check fires.
void	S9xXBandSearchCartIDInMemory (char *out, size_t out_size);

// Called from the getset.h S9xGetByte read interceptor whenever the
// CPU reads from $7F:$0C8B-$0C8E with the spoofer enabled. Records
// the PC + byte offset + returned value so we can see which BIOS
// instructions read the cart-id cache and disassemble around them.
void	S9xXBandLogCartIDRead (uint32 pc, int byte_off, uint8 byte_val);

// Called from getset.h S9xSetByte for any write to $7F:$0C8B-$0C8E.
// Captures the writer's PC -- this is the cart-id computation
// function we've been hunting. The writer's PC is much more useful
// than the consumer PCs because it points directly at the function
// that decides what value to store.
void	S9xXBandLogCartIDWrite (uint32 pc, int byte_off, uint8 byte_val);

// Overwrite every match of $F7 $2B $5D $1A in the searched
// memory regions with $D8 $22 $21 $03 (SSF2 Japan's expected
// GameID). Returns the number of locations modified. Used to
// bypass the BIOS's local cart-detection check by stomping the
// cached value the BIOS reads at Challenge click time.
int	S9xXBandForceCartIDOverride (char *out, size_t out_size);

// Toggle the TX GameID spoofer. When ON, every outgoing ADSP frame
// is parsed and any byte sequence matching $0C $F7 $2B $5D $1A
// (msGAMEIDAndPatchVersion + the BIOS's broken default cart hash)
// is rewritten to $0C $D8 $22 $21 $03 (SSF2 Japan's expected
// GameID per xbsega.go) before sending. The frame's CRC is
// recomputed over the modified body. Used to fool the server (and
// our own fake-server injects) into thinking the box has SSF2
// Japan loaded even though the BIOS's cart-detection produces
// garbage. OFF by default. Returns the new state.
bool	S9xXBandToggleGameIDSpoof (void);
bool	S9xXBandGetGameIDSpoof (void);

// Cart-id spoof value table. The interceptor returns whichever value
// is currently selected via S9xXBandSetSpoofValueByIndex(). The list
// is a fixed set of ~19 candidates pulled from xbsega.go and the
// commented-out SNES game hashes in the same file -- intended for
// brute-forcing which value the BIOS's local supported-games table
// accepts. The Win32 menu builds one MENUITEM per candidate.
void		S9xXBandCycleSpoofValue (void);
bool		S9xXBandSetSpoofValueByIndex (int idx);
int			S9xXBandGetSpoofValueCount (void);
const char *S9xXBandGetSpoofValueLabelAt (int idx);
const uint8 *S9xXBandGetSpoofValueBytesAt (int idx);
const char *S9xXBandSpoofValueLabel (void);
const uint8 *S9xXBandSpoofValueBytes (void);

// Inject a complete login + matchmaking server response. Force-primes
// the ADSP sniffer state so it works even without a live server
// connection. Sends: login reply, NGP list (SSF2 Japan), wait for
// opponent, in three ADSP batches.
bool	S9xXBandFakeInjectMatchmaking (void);

// Event-driven XBAND Server. Watches the BIOS's TX ServerTalk
// stream, detects batch boundaries, and injects appropriate server
// responses. Start/Stop via menu. Log shows timestamped message flow.
void	S9xXBandServerStart (void);
void	S9xXBandServerStop (void);
int		S9xXBandServerState (void);
bool	S9xXBandServerInterceptRX (void);
void	S9xXBandServerLogDump (char *out, size_t out_size);
void	S9xXBandServerTick (void);   // called from cpuexec main loop

// Compat shims for old menu handler.
void	S9xXBandFakeServerStart (void);
void	S9xXBandFakeServerStop (void);
int		S9xXBandFakeServerState (void);

// kDispatcherVector ($E0:$0040) call logger. cpuexec.cpp's main loop
// traps PBPC == $00E00040 and calls S9xXBandLogDispatcherCall() with
// the caller PC (read from the stack), function ID (X), and the
// accumulator (A). Dump appears in the kctl trace dialog.
void	S9xXBandLogDispatcherCall (uint32 caller, uint16 func_id, uint16 a);
void	S9xXBandResetDispatcherLog (void);

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
