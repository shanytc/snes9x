/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Game Boy serial port: the shift register itself, plus the BGB 1.4 link
// protocol (https://bgb.bircd.org/bgblink.html) over gb_link.cpp.
//
//   master (SC=$81): send sync1 -> 8 bit periods -> peer's sync2 into SB
//   slave  (SC=$80): arm, wait for peer's sync1, answer sync2, complete
//
// A 3- or 4-player session adds an emulated DMG-07 Four Player Adapter
// on the hosting instance (protocol per Pan Docs "4-Player Adapter" and
// GBE+'s dmg07.cpp). The adapter owns the clock: the host's own Game Boy
// is completed in-process, and each joining instance is driven over the
// ordinary BGB wire with the adapter as permanent master, so peers run
// the unmodified two-player code.
//
// With no peer the port keeps its original stub behaviour, so unlinked
// games are bit-for-bit unaffected.

#include "gb_serial.h"
#include "gb_link.h"
#include "gb_memory.h"
#include "gb_joypad.h"
#include "gb_cpu.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#include <process.h>
#define SGB_LINK_PID _getpid()
#else
#include <unistd.h>
#define SGB_LINK_PID getpid()
#endif

namespace SGB {

namespace {

// ---- Wire trace -------------------------------------------------------------
// SGB_LINK_TRACE=1 in the environment dumps every serial and adapter
// event to sgb_link_trace_<pid>.log in the working directory, one file
// per instance. Spawned instances inherit the variable.
FILE *g_trace         = nullptr;
bool  g_trace_checked = false;
char  g_trace_rom[48] = "";
char  g_trace_file[128] = "";

void TraceFileName(char *out, size_t cap)
{
	if (g_trace_rom[0])
		std::snprintf(out, cap, "sgb_link_trace_%s_%lu.log",
		              g_trace_rom, (unsigned long)SGB_LINK_PID);
	else
		std::snprintf(out, cap, "sgb_link_trace_%lu.log",
		              (unsigned long)SGB_LINK_PID);
}

bool TraceOn()
{
	if (!g_trace_checked)
	{
		g_trace_checked = true;
		const char *v = std::getenv("SGB_LINK_TRACE");
		if (v && *v && *v != '0')
		{
			TraceFileName(g_trace_file, sizeof g_trace_file);
			// Unqualified: the win32 build remaps fopen to its wide-path
			// wrapper via a forced include.
			g_trace = fopen(g_trace_file, "w");
		}
	}
	return g_trace != nullptr;
}

void Trace(const char *fmt, ...)
{
	if (!TraceOn()) return;
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(g_trace, fmt, ap);
	va_end(ap);
	std::fputc('\n', g_trace);
	std::fflush(g_trace);
}

// BGB command bytes.
constexpr uint8_t kCmdVersion        = 1;
constexpr uint8_t kCmdJoypad         = 101;
constexpr uint8_t kCmdSync1          = 104;
constexpr uint8_t kCmdSync2          = 105;
constexpr uint8_t kCmdSync3          = 106;
constexpr uint8_t kCmdStatus         = 108;
constexpr uint8_t kCmdWantDisconnect = 109;
// Ours, outside BGB's range: the hub tells each seat its byte period, so
// queued bursts replay at the wire's true cadence instead of guessing it
// from batched arrival times. Unknown to old peers, who ignore it.
constexpr uint8_t kCmdCadence        = 200;
// Barrier lockstep: the host grants each seat an emulated-time horizon.
constexpr uint8_t kCmdTimeGrant      = 201;

constexpr uint8_t kStatusRunning          = 0x01;
constexpr uint8_t kStatusPaused           = 0x02;
constexpr uint8_t kStatusSupportReconnect = 0x04;

// Bit periods in T-cycles: 8192 Hz normal, 262144 Hz CGB fast. Double
// speed needs no case - SerialStep is billed in CPU-domain cycles.
constexpr int32_t kBitPeriodNormal = 512;
constexpr int32_t kBitPeriodFast   = 16;

// Socket service interval in T-cycles (~0.24 ms): prompt enough for a
// slave to answer sync1, rare enough to cost nothing.
constexpr int32_t kPollInterval = 1024;

// How often to volunteer our timestamp so the peer can pace itself. Once
// a frame is ample; at ~1 ms it buried a paused peer in backlog.
constexpr int32_t kTimestampInterval = 65536;

// Emulated progress a peer may lag before we hold a frame: one span for
// the once-a-frame timestamp interval, one for the transport (2 MiHz).
constexpr int32_t kLockstepWindow = 2 * 35112;

// How long a peer's transfer waits for our game to arm. The two emulators
// free-run on their own timing, so their frame phase drifts against each
// other; a window shorter than a frame gets walked out of, and the games
// then see a dead cable. Held short once the other game has stopped
// answering, so a side that is not playing does not stall the poller.
constexpr int32_t kPendingHoldActive = 70224;   // one GB frame, ~16.7 ms
constexpr int32_t kPendingHoldIdle   = 4096;    // ~1 ms
constexpr uint32_t kUnansweredIdle   = 4;

// Floor between passive completions, ~1.015 ms: the fastest a DMG-07 ever
// clocks bytes (0.887 ms gap + 0.128 ms shift). Games time their mailbox
// reads against it — a queued burst delivered any faster overwrites SB
// before the main loop consumed it, and the game's packet framing slips
// until a reset. That is what garbled F-1 Race's racer-entry screen.
constexpr int32_t kExtByteGap = 4257;

// Arm-to-completion floor, the 0.128 ms the DMG-07 shifts a byte in: a
// same-instruction settle loses the IRQ to the game's own IF clear.
constexpr int32_t kExtShiftTime = 537;

// Ceiling for the observed-cadence floor: the slowest DMG-07 byte period.
constexpr int32_t kExtGapCeil = 10932;

int32_t ExtGapFloor(const Serial &s)
{
	// The hub declares its byte period; without one (direct cable, an
	// older peer) the DMG-07's fastest cadence stays the floor, as it
	// always was.
	int32_t g = s.clk_gap_fixed ? s.clk_gap_fixed : kExtByteGap;
	if (g < kExtByteGap) g = kExtByteGap;
	if (g > kExtGapCeil) g = kExtGapCeil;
	return g;
}

// Ceiling on the master's stall waiting for a reply - bounded so a frozen
// peer can't take this instance down with it.
constexpr int kMasterWaitMs = 500;

// After one timeout the peer is presumed paused: drop to a shorter wait,
// or every byte would cost the full ceiling. Still above the worst honest
// reply - lockstep skew, the peer's once-a-frame arm, and settle pacing
// stack to ~50 ms - or a single unanswered poll poisons the pairing.
constexpr int kStalledWaitMs = 80;
constexpr int kWaitSliceMs   = 5;

// The Serial being stepped and its bus, so the UI can pump the socket
// while the emulation thread is parked. Only ever one GB core.
Serial *g_active     = nullptr;
Memory *g_active_mem = nullptr;

// What the answer to one adapter clock will mean when it lands, frozen at
// send time so later phase changes cannot skew it. The instances free-run
// on their own frame bursts, so replies are collected whenever they arrive
// -- the protocol's own one-packet buffering delay is the tolerance.
struct HubReplyTag
{
	uint32_t seq           = 0;    // echoed back by the peer in its answer
	int16_t  store_at      = -1;   // transmission-buffer index, or none
	int8_t   presence_wire = -1;   // 1/2 when the reply is an $88 ack slot
};

constexpr int kHubFifoDepth = 64;

// Per-channel peer session state. Never serialized: it belongs to the
// connection. Channel 0 is the direct cable; a hub uses all three.
struct LinkSession
{
	int      gen               = -1;      // transport occupant this belongs to
	bool     handshake_sent    = false;   // our version packet is out
	bool     handshaked        = false;   // theirs came back and checked out
	bool     peer_running      = false;
	bool     peer_reconnect_ok = false;
	uint32_t peer_timestamp    = 0;

	// Lockstep baseline, latched at the first timestamp: only progress
	// SINCE the handshake counts, so a fresh boot never stalls a peer
	// that has been running for minutes.
	bool     ts_valid          = false;
	uint32_t ts_base_peer      = 0;
	uint32_t ts_base_ours      = 0;

	// The adapter clocks in flight on this seat, oldest first. Replies
	// echo the sequence number, so a peer that flushed its queues (state
	// load) just leaves holes we skip over instead of shifting every
	// later answer onto the wrong byte.
	uint32_t    seq_next  = 1;
	uint8_t     fifo_head = 0;
	uint8_t     fifo_len  = 0;
	HubReplyTag fifo[kHubFifoDepth];
};

LinkSession g_sessions[kLinkMaxChannels];

// Direct-cable bookkeeping, meaningful only for channel 0 without a hub.
struct DirectState
{
	uint32_t transfers  = 0;
	bool     stalled    = false;   // a master wait timed out

	// Who drove the last transfer, chosen per byte by the games via SC
	// bit 0: 0 = nothing yet, 1 = us, 2 = the peer.
	uint8_t  driving     = 0;
	// When each direction last completed: games like Death Track clock
	// both ways, and neither label alone is true for them. Latched for
	// the session - idle gaps must not flap the OSD role back and forth.
	int64_t  last_drive_us   = -1;
	int64_t  last_drive_peer = -1;
	bool     bidir_seen      = false;
	uint32_t unanswered  = 0;   // polls we clocked that nobody answered
	uint8_t  reported    = 0;   // last value handed to the host UI
	int64_t  next_report = 0;   // real-cycle gate on re-announcing
};

DirectState g_direct;

// Games alternating master/slave per byte would repaint the OSD forever;
// ~3 s of GB time between announcements keeps it readable.
constexpr int64_t kRoleReportInterval = 3 * 4194304;

LinkRole g_role = LinkRole::None;

SerialByteCallback g_serial_cb = nullptr;

// ---- DMG-07 Four Player Adapter --------------------------------------------
// Timing from Pan Docs, in real T-cycles at 4.194304 MHz (1 ms = 4194).
constexpr int32_t kCyclesPerMs = 4194;

// Ping packets: four bytes clustered at the start (128 us of shifting,
// 1.42 ms gap), then a 12.2 ms + (RATE & $0F) ms rest.
constexpr int32_t kPingByteGap = 6486;
constexpr int32_t kPingRest    = 51169;

// Rest after the one $CC packet, socket sessions only: a seat replaying
// a backlog needs room to match it or it never leaves the ping phase.
constexpr int32_t kSyncSettle  = 4 * 70224;

// Transmission packets: byte-to-byte is 0.887 ms + (RATE >> 4) * 0.106 ms
// plus the shift itself, and the packet repeats no faster than
// (17 + (RATE & $0F)) ms.
constexpr int32_t kNetByteBase  = 4257;
constexpr int32_t kNetByteStep  = 445;
constexpr int32_t kNetPacketMin = 71298;
constexpr int32_t kNetPacketPad = 4194;   // tail beyond the last byte

enum : uint8_t
{
	kDmg07Ping    = 0,   // FE + three status bytes, probing for $88 acks
	kDmg07Sync    = 1,   // one packet of $CC announcing transmission mode
	kDmg07Net     = 2,   // data packets, SIZE bytes per player
	kDmg07Restart = 3    // one packet of $FF on the way back to ping
};

struct Dmg07
{
	bool    hosting = false;
	bool    local   = false;  // split screen: every seat is an in-process core
	uint8_t target  = 2;      // players on the session, this instance included

	uint8_t phase      = kDmg07Ping;
	uint8_t wire       = 0;   // byte index inside the current packet
	// Net-phase byte events wait briefly for every console to re-arm: a
	// missed transfer shifts that console's reply cycle permanently and
	// rotates its blocks in the broadcast (Jantaku Boy's confirm landed
	// on the wrong window byte). Real ~1 ms byte gaps never miss.
	uint8_t event_grace = 0;
	// Byte period in force, so the arm grace can be held to it.
	int32_t byte_gap    = kNetByteBase;
	int32_t byte_gap_sent = 0;   // last cadence broadcast to the seats

	// Bulk re-anchoring: a [FD,FF,FF,FF] wake window followed by an
	// LEN-prefixed bulk shifts every game's 4-byte framing by LEN mod 4.
	// The adapter is a dumb pipe — the games' framing is THE framing —
	// so the slot cycle re-anchors when the bulk ends. 0=idle, 1..3 =
	// wake-window match, 4 = next byte is LEN, 6 = counting, 7 = done.
	uint8_t bulk_stage = 0;
	uint8_t bulk_left  = 0;
	uint8_t status     = 0;   // presence bits 4-7, one per port
	uint8_t rate       = 0;   // player 1's RATE reply, latched during ping
	uint8_t size_reply = 0;   // player 1's SIZE reply, latched during ping
	uint8_t size       = 1;   // packet size in force for the transmission run
	bool    begin_sync = false;
	uint8_t ff_run     = 0;   // consecutive $FF replies from player 1
	bool    restart_pending = false;

	// Per-seat consecutive armed $FF replies during transmission. A run of
	// exactly two, broken by a non-$FF, is a game's mid-session restart
	// request ([cmd,FF,FF,00] command blocks): the adapter answers by
	// re-announcing the transmission — the [FD,FF,FF,FF] wake window —
	// which is the only thing that releases Jantaku Boy's lobby blackouts.
	// Runs of 3+ belong to the quit flood and never fire this.
	uint8_t wake_ff[4]  = {0, 0, 0, 0};
	bool    wake_pending = false;
	// The command byte riding ahead of the FF pair is the bulk LENGTH the
	// consoles will read from the first post-wake broadcast byte. When the
	// requesting seat isn't player 1 its command rides the wrong slot, so
	// the announcement presets slot 0 with it and guards that byte from
	// being clobbered by stale pre-wake replies.
	uint8_t wake_prev[4] = {0, 0, 0, 0};
	uint8_t wake_cmd     = 0;
	bool    wake_len_guard = false;
	// Completed request frames per seat, and the command each carried.
	// A repeat of the same command is one standing request, not a new
	// one: the block stays on the wire until the wake answers it.
	uint8_t wake_pairs[4]    = {0, 0, 0, 0};
	uint8_t wake_pair_cmd[4] = {0, 0, 0, 0};
	uint8_t wake_seat        = 0;

	// Bulk relay: the LEN-announced payload is the requesting console's
	// contiguous C300 block ($03C0 stages C600->C300; $0345 harvests the
	// broadcast into C200 byte-for-byte). The per-slot sampling that is
	// right for stage-1 signal windows would interleave three seats of
	// junk into it, so during a bulk the broadcast relays the requester's
	// replies in order instead.
	int8_t  bulk_src  = -1;   // seat whose stream the bulk relays
	bool    relay_arm = false;
	uint8_t rq[8];
	uint8_t rq_head = 0, rq_len = 0;

	uint8_t buffer[32];       // size*8: the half going out + the half filling
	uint8_t buf_pos = 0;

	int32_t timer = 0;        // real T-cycles until the next byte event

	// The previous byte event, kept so player 1's in-process answer can be
	// read at the next event with hardware wire semantics; remote answers
	// instead land whenever they arrive, matched by sequence number.
	bool    prev_valid = false;
	uint8_t prev_phase = kDmg07Ping;
	uint8_t prev_wire  = 0;
	uint8_t prev_buf   = 0;
	uint8_t prev_local = 0xFF;
	// Whether prev_local came from an armed transfer: protocol-steering
	// values must never latch from an idle wire's $FF (GBE+ semantics).
	bool    prev_local_armed = false;
	// Consecutive armed $AA replies: the Pan Docs chart shows player 1
	// answering $AA on every ping byte to start transmission. A single
	// stray $AA (Trump Boy II beacons one per frame from its title
	// screen) must not flip the adapter.
	uint8_t aa_run = 0;

	Dmg07() { std::memset(buffer, 0, sizeof buffer); }
};

Dmg07 g_hub;

int g_hub_reported = -1;   // instances last announced on the OSD

// Split screen: every core on the session lives in this process, in
// player order. Seat 0 is the primary core; the others are peeked and
// completed directly, so nothing here ever waits or buffers.
struct SplitLink
{
	int     count = 0;
	Serial *s[kLinkMaxSeats] = {};
	Memory *m[kLinkMaxSeats] = {};

	// Wire bytes captured from seats 2..4 at the previous adapter event,
	// interpreted one event later — the same hardware semantics as the
	// hub's player 1. (Hub sessions only, so the 4-seat span suffices.)
	uint8_t prev_seat[kLinkMaxSeats - 1]       = {0xFF, 0xFF, 0xFF};
	bool    prev_seat_armed[kLinkMaxSeats - 1] = {};
};

SplitLink g_splitlink;

// ---- Faceball 2000 sixteen-player ring --------------------------------------
// The custom cable Faceball's dormant 16-player mode was written for:
// every unit's serial-out feeds the next unit's serial-in on one shared
// clock, so a transfer rotates the whole ring one hop at once. The game's
// ISR does the store-and-forward, ID assignment ($1x hops, +1 per unit)
// and count broadcast ($2x); the wire only has to shift. A port with SC.7
// clear still shifts — hardware pass-through — just with no interrupt.
struct FaceRing
{
	bool     active   = false;
	bool     shifting = false;   // eight shared bit periods in flight
	int32_t  timer    = 0;
	uint64_t xfers    = 0;
};

FaceRing g_ring;

// ---- Jantaku Boy PC tracer --------------------------------------------------
// Breakpoint-style: logs hits on the game's lobby key addresses along
// with the variable that matters there, so the event/step interleave
// reads as a transcript. Active only while SGB_LINK_TRACE is on.
int g_trace_seat = -1;   // core currently executing in the split loop

struct PcWatch { uint16_t pc; uint8_t bank; const char *name; uint16_t var; };
// bank 0xFF = unbanked ($0000-$3FFF); else must match the shadow $FFAB.
// var: $C0xx/$C6xx -> wram, $FF8x+ -> hram.
constexpr PcWatch kPcWatch[] = {
	// Death Track boots its SC arm from RAM: log what D574 held when the
	// arm ran, whether the ISR's adapter detect fired, and the role pick.
	{ 0x1E69, 0xFF, "dt-arm",      0xD574 },
	{ 0x1BBB, 0xFF, "dt-detect",   0xD572 },
	{ 0x1BD6, 0xFF, "dt-got81",    0xD572 },
	// The lobby-join gate: needs the 4-byte ping capture [FE,s,s,s]
	// complete with presence nibbles, and wipes it on every failure.
	{ 0x76F2, 0xFF, "dt-join-ok",  0xD753 },
	{ 0x76F9, 0xFF, "dt-join-no",  0xD753 },
	{ 0x76E8, 0xFF, "dt-join-hdr", 0xD752 },
	// F-1 Hero's MULTI GAME menu gate: proceeds only on $FFC0 = 1 or 3.
	{ 0x4480, 0xFF, "mg-gate",     0xFFC0 },
	{ 0x08C3, 0xFF, "ev-dispatch", 0xC0B1 },
	{ 0x0998, 0xFF, "ev3-step",    0xC0B2 },
	{ 0x0A08, 0xFF, "step0-stn",   0xC0B3 },
	{ 0x03E6, 0xFF, "blackout+",   0xC0B2 },
	{ 0x03F7, 0xFF, "blackout-",   0xC0B2 },
	{ 0x0AB2, 0xFF, "rollcall",    0xC6A0 },
	{ 0x03D0, 0xFF, "cmdwrite",    0xFFA9 },
	{ 0x1492, 0xFF, "fd-poll",     0xFF96 },
	{ 0x149E, 0xFF, "FD-SEND",     0xFFA1 },
	{ 0x6000, 3,    "stn0",        0xC0B2 },
	{ 0x602A, 3,    "stn1",        0xC0B3 },
	{ 0x6053, 3,    "stn2-Await",  0xFF96 },
	{ 0x6077, 3,    "stn3",        0xFFA1 },
	{ 0x608C, 3,    "stn4",        0xFFA1 },
	{ 0x719F, 6,    "mode3-entry", 0xFF99 },
	{ 0x7164, 6,    "lobby-loop",  0xFF9D },
	// In-game turn machinery (macro-step 5, handler $176B):
	{ 0x2317, 0xFF, "discard-A",   0xC0B9 },   // A-edge commit path from $2038
	{ 0x2285, 0xFF, "discard-B",   0xC0B9 },   // B-edge path from $2038
	{ 0x2150, 0xFF, "turn-modal",  0xC6A5 },   // C6A5 modal branch (riichi menu?)
	{ 0x1830, 0xFF, "turn-owner",  0xC6A2 },   // my-turn entry
	{ 0x1934, 0xFF, "turn-watch",  0xC6A1 },   // spectator watch entry
	{ 0x1A16, 0xFF, "saw-1F",      0xC6A1 },   // spectator saw the discard-done
	{ 0x1788, 0xFF, "dlr-sub",     0xC788 },   // dealer path for absent seat
	{ 0x17B6, 0xFF, "sig-3F",      0xC6A2 },   // [3F] signal write
	{ 0x17D1, 0xFF, "dlr-tick",    0xC788 },   // dealer AI tick (bank7)
	{ 0x1901, 0xFF, "send-1F",     0xC0A6 },   // owner broadcasts 1F + discard
	{ 0x1C99, 0xFF, "zero-barr",   0xC6A2 },   // zero-block + all-zero barrier
	{ 0x197F, 0xFF, "tile-latch",  0xC0A5 },   // value spectator took as the tile
	{ 0x1C80, 0xFF, "tile-follow", 0xC0A7 },   // owner's announce follow-up byte
	{ 0x2106, 0xFF, "announce",    0xC0A7 },   // turn-entry broadcast helper
	// Faceball 2000 ring bring-up ($101A) and round machinery.
	{ 0x101A, 0xFF, "fb-linkup",   0xFF91 },   // bring-up entry, FF91 flags
	{ 0x104F, 0xFF, "fb-elect",    0xFF92 },   // no activity seen -> master
	{ 0x1032, 0xFF, "fb-wait-id",  0xFF92 },   // slave spins for its ring id
	{ 0x1092, 0xFF, "fb-bcast",    0xC907 },   // master broadcasts $2x count
	{ 0x1085, 0xFF, "fb-alone",    0xC907 },   // enumeration timed out
	{ 0x1048, 0xFF, "fb-lnk-ok",   0xC907 },   // bring-up done, count known
	{ 0x11E5, 0xFF, "fb-round-wt", 0xC90B },   // waiting a circulation home
	{ 0x12A2, 0xFF, "fb-kick",     0xC90B },   // master's per-frame ring kick
	{ 0x1246, 0xFF, "fb-teardown", 0xC910 },
	{ 0x3D60, 0xFF, "fb-gamestart",0xDA2A },   // pre-game setup, calls $101A
	{ 0x0F20, 0xFF, "fb-tok-home", 0xC909 },   // ISR matched own $30|id token
	{ 0x0F48, 0xFF, "fb-rnd-home", 0xC90B },   // own bytes all returned
	{ 0x0F4C, 0xFF, "fb-restart",  0xC909 },   // ISR round restart
};
uint32_t g_pc_hits[sizeof kPcWatch / sizeof *kPcWatch][kLinkMaxSeats] = {};

void SerialPcHook(uint16_t pc, uint8_t, const CpuState &st)
{
	// Split tags the executing seat; a single-core instance session
	// watches the primary, so multi-window runs get the PC trace too.
	Memory *m; int seat;
	if (g_trace_seat >= 0) { m = g_splitlink.m[g_trace_seat]; seat = g_trace_seat; }
	else                   { m = g_active_mem;                seat = 0; }
	if (!m) return;
	for (size_t w = 0; w < sizeof kPcWatch / sizeof *kPcWatch; ++w)
	{
		const PcWatch &pw = kPcWatch[w];
		if (pc != pw.pc) continue;
		if (pw.bank != 0xFF && m->hram[0x2B] != pw.bank) continue;
		uint32_t &hits = g_pc_hits[w][seat];
		++hits;
		// Hot spins: after 64 hits, one line per 256.
		if (hits > 64 && (hits & 0xFF) != 0) return;
		const uint8_t v = pw.var >= 0xFF80
		                ? m->hram[pw.var - 0xFF80]
		                : m->wram[pw.var - 0xC000];
		Trace("PC p%d %-11s v=%02X n=%u t=%lld",
		      seat + 1, pw.name, v, hits, (long long)st.t_cycles);
		return;
	}
}

inline int32_t BitPeriod(const Serial &s, uint8_t sc)
{
	return (s.cgb && (sc & 0x02)) ? kBitPeriodFast : kBitPeriodNormal;
}

inline uint32_t Timestamp(const Serial &s)
{
	// 2 MiHz clocks, low 31 bits — the high bit is always 0 per the protocol.
	return static_cast<uint32_t>((s.real_cycles >> 1) & 0x7FFFFFFF);
}

void SendPacket(int ch, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4,
                uint32_t i1, bool droppable = false)
{
	LinkPacket p;
	p.b1 = b1;
	p.b2 = b2;
	p.b3 = b3;
	p.b4 = b4;
	p.i1 = i1;
	LinkSend(ch, p, droppable);
}

void SendVersion(int ch) { SendPacket(ch, kCmdVersion, 1, 4, 0, 0); }

void SendStatus(int ch)
{
	SendPacket(ch, kCmdStatus,
	           static_cast<uint8_t>(kStatusRunning | kStatusSupportReconnect),
	           0, 0, 0);
}

// ---- Held peer clocks (slave side) ------------------------------------------

void PendingPop(Serial &s, uint8_t &data, uint32_t &i1)
{
	data = s.pending_data[s.pending_head];
	i1   = s.pending_i1[s.pending_head];
	s.pending_head = static_cast<uint8_t>((s.pending_head + 1) % Serial::kPendingCap);
	--s.pending_len;
	s.pending_age = 0;
}

// Release the oldest held clock unanswered: the wire idled high for it.
void PendingAckDropOldest(int ch, Serial &s)
{
	uint8_t  data;
	uint32_t i1;
	PendingPop(s, data, i1);
	Trace("[%lld] drop held %02X seq=%u (never armed)",
	      (long long)s.real_cycles, data, i1);
	SendPacket(ch, kCmdSync3, 1, 0, 0, i1);
}

void PendingPush(int ch, Serial &s, uint8_t data, uint32_t i1)
{
	if (s.pending_len == Serial::kPendingCap)
		PendingAckDropOldest(ch, s);

	const int tail = (s.pending_head + s.pending_len) % Serial::kPendingCap;
	s.pending_data[tail] = data;
	s.pending_i1[tail]   = i1;
	s.pending_born[tail] = s.real_cycles;
	if (s.pending_len == 0) s.pending_age = 0;
	++s.pending_len;
}

void PendingClear(Serial &s)
{
	s.pending_head = 0;
	s.pending_len  = 0;
	s.pending_age  = 0;
}

int HandshakedCount()
{
	int n = 0;
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		if (LinkChannelConnected(ch) && g_sessions[ch].handshaked) ++n;
	return n;
}

// ---- Hub reply matching -----------------------------------------------------

HubReplyTag HubFifoPop(LinkSession &ses)
{
	const HubReplyTag t = ses.fifo[ses.fifo_head];
	ses.fifo_head = static_cast<uint8_t>((ses.fifo_head + 1) % kHubFifoDepth);
	--ses.fifo_len;
	return t;
}

void HubFifoPush(LinkSession &ses, const HubReplyTag &t)
{
	// A seat this far behind is stalled; its oldest clock is a dead letter.
	if (ses.fifo_len == kHubFifoDepth) HubFifoPop(ses);
	ses.fifo[(ses.fifo_head + ses.fifo_len) % kHubFifoDepth] = t;
	++ses.fifo_len;
}

inline bool SeqLess(uint32_t a, uint32_t b)
{
	return static_cast<int32_t>(a - b) < 0;
}

// Queue one relay byte from the bulk source's reply stream.
void HubBulkRelayPush(int seat, uint8_t b, bool armed)
{
	if (!g_hub.relay_arm || seat != g_hub.bulk_src) return;
	if (!armed) b = 0x00;
	if (g_hub.rq_len < sizeof g_hub.rq)
		g_hub.rq[(g_hub.rq_head + g_hub.rq_len++) % sizeof g_hub.rq] = b;
}

// Watch one seat's transmission replies for the [cmd,FF,FF,00] restart
// request: an armed $FF pair broken by a non-$FF arms a wake, served as a
// fresh [FD,FF,FF,FF] announcement at the next packet boundary. Counting
// pauses while a bulk is in flight — payload bytes are not protocol.
void HubWakeFF(int seat, uint8_t b, bool armed)
{
	// The request block spans exactly one SIZE=1 packet; at larger sizes
	// (F-1 Race streams at 4) an $FF pair is payload, never protocol.
	if (g_hub.size != 1)
		return;
	if (!armed || g_hub.prev_phase != kDmg07Net || g_hub.bulk_stage != 0)
		return;
	if (b == 0xFF)
	{
		if (g_hub.wake_ff[seat] < 0xFF) ++g_hub.wake_ff[seat];
		return;
	}
	if (g_hub.wake_ff[seat] == 2)
	{
		const uint8_t cmd = g_hub.wake_prev[seat];
		if (cmd != g_hub.wake_pair_cmd[seat])
		{
			g_hub.wake_pair_cmd[seat] = cmd;
			g_hub.wake_pairs[seat]    = 1;
		}
		else if (g_hub.wake_pairs[seat] < 0xFF)
		{
			++g_hub.wake_pairs[seat];
		}
		// Fire on the first completed request frame: the command must not
		// linger on the wire — station matchers re-dispatch a visible
		// command every window they see it in.
		if (g_hub.wake_pairs[seat] == 1)
		{
			g_hub.wake_pending = true;
			g_hub.wake_cmd     = cmd;
			g_hub.wake_seat    = static_cast<uint8_t>(seat);
			Trace("hub wake requested (seat %d $FF pair, cmd=%02X)",
			      seat + 1, g_hub.wake_cmd);
		}
	}
	g_hub.wake_ff[seat]   = 0;
	g_hub.wake_prev[seat] = b;
}

// A seat's answer landed: apply it to the byte it was for. Answers the
// peer never sent (it flushed on a state load) are skipped past; answers
// to bytes already skipped are stale and dropped.
void HubApplyRemoteReply(int ch, uint32_t seq, uint8_t data, bool supplied)
{
	LinkSession &ses = g_sessions[ch];

	int skipped = 0;
	while (ses.fifo_len && SeqLess(ses.fifo[ses.fifo_head].seq, seq))
	{
		HubFifoPop(ses);
		++skipped;
	}
	if (!ses.fifo_len || ses.fifo[ses.fifo_head].seq != seq)
	{
		Trace("rx  ch=%d seq=%u data=%02X sup=%d STALE (skipped=%d fifo=%d)",
		      ch, seq, data, supplied ? 1 : 0, skipped, ses.fifo_len);
		return;
	}
	Trace("rx  ch=%d seq=%u data=%02X sup=%d ok (skipped=%d)",
	      ch, seq, data, supplied ? 1 : 0, skipped);

	const HubReplyTag tag = HubFifoPop(ses);

	if (tag.presence_wire >= 0)
	{
		// Any non-idle reply means a console is plugged in and listening.
		// Death Track's lobby gate needs the presence bits while a seat
		// still only echoes its STAT; the $88 ack merely confirms it.
		const uint8_t bit = static_cast<uint8_t>(1 << (5 + ch));
		if (supplied && data != 0x00 && data != 0xFF)
			g_hub.status = static_cast<uint8_t>(g_hub.status | bit);
		else
			g_hub.status = static_cast<uint8_t>(g_hub.status & ~bit);
	}

	if (tag.store_at >= 0 && tag.store_at < static_cast<int>(sizeof g_hub.buffer))
	{
		// A seat with nothing to say reads as zeroes in the broadcast,
		// which is what the DMG-07 fills empty ports with (Pan Docs).
		// One-shot guard: see the player-1 store site.
		if (g_hub.wake_len_guard && tag.store_at == 4)
			g_hub.wake_len_guard = false;
		else
			g_hub.buffer[tag.store_at] = supplied ? data : 0x00;
	}

	HubWakeFF(ch + 1, data, supplied);
	HubBulkRelayPush(ch + 1, data, supplied);
}

// Finish a transfer we clocked. With no peer byte the wire idles high,
// the same $FF an unplugged cable shifts in.
void CompleteActive(Serial &s, Memory &mem)
{
	mem.serial_data    = s.peer_valid ? s.peer_data : 0xFF;
	mem.serial_control = static_cast<uint8_t>(mem.serial_control & 0x7F);
	mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);

	s.active     = false;
	s.bits_left  = 0;
	// The wire cannot clock again for another byte period, whoever the
	// master was. Without this a peer clock arriving in the same socket
	// drain as our reply is answered from the stale pre-transfer arm -
	// the game's ISR never ran in between. Death Track's pairing died on
	// exactly that race.
	if (s.ext_gap_timer < kExtShiftTime) s.ext_gap_timer = kExtShiftTime;
	if (s.peer_supplied)
	{
		++g_direct.transfers;
		g_direct.driving       = 1;
		g_direct.last_drive_us = s.real_cycles;
		g_direct.unanswered    = 0;
	}
	else if (s.peer_valid)
	{
		// We drove the clock but the other game was not listening.
		++g_direct.unanswered;
	}
	s.peer_valid    = false;
	s.peer_supplied = false;
	s.peer_data     = 0xFF;
}

// Step an armed transfer whose outcome is already known (stub or master
// under the adapter: the wire idles $FF). It still takes its eight bit
// periods — an instant IRQ re-enters the game's ISR before the
// bookkeeping that follows the SC write, and corrupts its state.
void StepResolvedActive(Serial &s, Memory &mem, int32_t tcycles)
{
	if (!s.active) return;
	s.bit_timer -= tcycles;
	while (s.bit_timer <= 0 && s.bits_left > 0)
	{
		--s.bits_left;
		s.bit_timer += s.bit_period;
	}
	if (s.bits_left == 0) CompleteActive(s, mem);
}

// Finish a transfer the peer clocked for us.
void CompletePassive(Serial &s, Memory &mem, uint8_t data)
{
	mem.serial_data    = data;
	mem.serial_control = static_cast<uint8_t>(mem.serial_control & 0x7F);
	mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);

	s.passive       = false;
	// The gap preserves the wire's emission spacing, so a byte that
	// already waited in the queue gets credit - releases must follow the
	// hub's clock, not re-quantize to this game's arm phase. A fixed
	// phase lock let Death Track's join gate fail the same way forever.
	{
		int32_t g = ExtGapFloor(s);
		if (s.pending_len)
		{
			const int64_t waited =
				s.real_cycles - s.pending_born[s.pending_head];
			if (waited > 0) g -= static_cast<int32_t>(
				waited > g ? g : waited);
		}
		if (g < kExtShiftTime) g = kExtShiftTime;
		s.ext_gap_timer = g;
	}
	++g_direct.transfers;
	g_direct.driving         = 2;
	g_direct.last_drive_peer = s.real_cycles;
	g_direct.unanswered      = 0;
	// The peer clocking us proves it is alive: restore the full wait.
	g_direct.stalled         = false;
}

// Deliver the next held clock once one is due: the game has re-armed and
// the wire could really have clocked another byte by now.
void TrySettlePending(Serial &s, Memory &mem)
{
	if (!s.passive || !s.pending_len || s.ext_gap_timer > 0) return;

	uint8_t  data;
	uint32_t i1;
	PendingPop(s, data, i1);
	Trace("[%lld] settle held %02X seq=%u -> reply %02X (left=%u)",
	      (long long)s.real_cycles, data, i1, mem.serial_data, s.pending_len);
	SendPacket(0, kCmdSync2, mem.serial_data, 0x80, 0, i1);
	CompletePassive(s, mem, data);
}

// The countdown of an in-process master ran out: read the other core's
// live state, completing its slave side on the spot. False when the
// peer has nothing armed — the caller waits out the interleave skew
// before ruling the wire idle.
bool SplitTryResolveMaster(Serial &s, Memory &mem)
{
	Serial *ps = (&s == g_splitlink.s[0]) ? g_splitlink.s[1] : g_splitlink.s[0];
	Memory *pm = (&s == g_splitlink.s[0]) ? g_splitlink.m[1] : g_splitlink.m[0];

	if (ps && ps->passive)
	{
		const uint8_t theirs = pm->serial_data;
		CompletePassive(*ps, *pm, mem.serial_data);
		s.peer_data     = theirs;
		s.peer_valid    = true;
		s.peer_supplied = true;
		return true;
	}
	return false;
}

// The peer core was not armed for a clock we drove. Hold the byte for
// it the way the socket transport holds a peer emulator's: hardware
// shifts it into the other console's register whether or not that
// console asked for a transfer, and a game that clocks unilaterally
// (Micro Machines promotes itself to master on a timeout, whatever the
// other console is doing) sends most of its handshake into exactly
// this gap. Games with a settled master and slave never notice, which
// is why nothing else needed it.
void SplitHoldClock(Serial &s, uint8_t data)
{
	Serial *ps = (&s == g_splitlink.s[0]) ? g_splitlink.s[1] : g_splitlink.s[0];
	if (!ps || ps->pending_len >= Serial::kPendingCap) return;

	const int tail = (ps->pending_head + ps->pending_len) % Serial::kPendingCap;
	ps->pending_data[tail] = data;
	ps->pending_i1[tail]   = 0;
	ps->pending_born[tail] = ps->real_cycles;
	if (ps->pending_len == 0) ps->pending_age = 0;
	++ps->pending_len;
}

// Deliver one held clock once this core arms. Nothing is sent back:
// the core that drove it settled long ago, exactly as it would have
// against a wire whose other end was not listening yet.
void SplitSettlePending(Serial &s, Memory &mem)
{
	if (!s.passive || !s.pending_len || s.ext_gap_timer > 0) return;

	uint8_t  data;
	uint32_t i1;
	PendingPop(s, data, i1);
	Trace("[%lld] split settle held %02X (left=%u)",
	      (long long)s.real_cycles, data, s.pending_len);
	CompletePassive(s, mem, data);
}

void HandlePacket(int ch, Serial &s, Memory &mem, const LinkPacket &p)
{
	LinkSession &ses = g_sessions[ch];

	switch (p.b1)
	{
		case kCmdVersion:
			// A mismatched protocol reads as bogus transfers, which can
			// corrupt a trade. Drop instead — on a hub only this seat.
			if (p.b2 != 1 || p.b3 != 4 || p.b4 != 0)
			{
				LinkCloseChannel(ch);
				ses = LinkSession();
				return;
			}
			ses.handshaked = true;
			Trace("ch=%d handshaked", ch);
			SendStatus(ch);
			return;

		case kCmdStatus:
			ses.peer_running      = (p.b2 & kStatusRunning) != 0 &&
			                        (p.b2 & kStatusPaused) == 0;
			ses.peer_reconnect_ok = (p.b2 & kStatusSupportReconnect) != 0;
			// Must not be answered with another status, or the two sides
			// ping-pong forever.
			return;

		case kCmdSync1:
			if (g_hub.hosting)
			{
				// A game clocking internally under the adapter gets garbage
				// on hardware; an empty ack releases its transfer, which is
				// the closest polite translation.
				Trace("in  ch=%d sync1 %02X seq=%u -> ack (seat is clocking)",
				      ch, p.b2, p.i1);
				SendPacket(ch, kCmdSync3, 1, 0, 0, p.i1);
				return;
			}
			if (s.passive && s.pending_len == 0 && s.ext_gap_timer <= 0)
			{
				// Armed and waiting to be clocked — hand back our byte. The
				// echoed i1 tells a DMG-07 host which clock this answers.
				Trace("[%lld] in  sync1 %02X seq=%u -> reply %02X",
				      (long long)s.real_cycles, p.b2, p.i1, mem.serial_data);
				SendPacket(ch, kCmdSync2, mem.serial_data, 0x80, 0, p.i1);
				CompletePassive(s, mem, p.b2);
			}
			else if (s.active)
			{
				if (!s.peer_valid)
				{
					// Crossed masters: the port is full duplex, so two
					// overlapping clocks exchange bytes on hardware.
					// Death Track pairs its players with exactly this.
					Trace("[%lld] in  sync1 %02X seq=%u -> crossed (ours %02X)",
					      (long long)s.real_cycles, p.b2, p.i1,
					      mem.serial_data);
					SendPacket(ch, kCmdSync2, mem.serial_data, 0x80, 0, p.i1);
					s.peer_data     = p.b2;
					s.peer_valid    = true;
					s.peer_supplied = true;
				}
				else
				{
					// Our transfer is already answered; their clock is for
					// whatever we arm next - hold it like any early clock.
					Trace("[%lld] in  sync1 %02X seq=%u -> held past active",
					      (long long)s.real_cycles, p.b2, p.i1);
					PendingPush(ch, s, p.b2, p.i1);
				}
			}
			else
			{
				// Hold it, in order, until our game arms and the pacing gap
				// has passed. Bursts of clocks land here whenever the other
				// emulator's frame ran ahead of ours.
				Trace("[%lld] in  sync1 %02X seq=%u -> held (n=%u pas=%d gap=%d)",
				      (long long)s.real_cycles, p.b2, p.i1,
				      s.pending_len + 1, s.passive ? 1 : 0, s.ext_gap_timer);
				PendingPush(ch, s, p.b2, p.i1);
			}
			return;

		case kCmdSync2:
			if (g_hub.hosting)
			{
				HubApplyRemoteReply(ch, p.i1, p.b2, true);
				return;
			}
			// Only while we are actually clocking: a reply that arrives after
			// we gave up would otherwise be shifted into the next transfer.
			if (!s.active)
			{
				Trace("in  sync2 %02X seq=%u LATE (not clocking)", p.b2, p.i1);
				return;
			}
			Trace("in  sync2 %02X seq=%u", p.b2, p.i1);
			s.peer_data     = p.b2;
			s.peer_valid    = true;
			s.peer_supplied = true;
			return;

		case kCmdSync3:
			if (p.b2 == 1)
			{
				// Peer had nothing armed, so the wire stayed high.
				if (g_hub.hosting)
				{
					HubApplyRemoteReply(ch, p.i1, 0xFF, false);
					return;
				}
				// Releases our transfer but is not an exchange with anyone.
				if (!s.active) return;
				s.peer_data     = 0xFF;
				s.peer_valid    = true;
				s.peer_supplied = false;
			}
			else
			{
				// A timestamp jumping backward is the peer's console
				// resetting (the cable survives a reset); re-baseline.
				const int32_t step = static_cast<int32_t>(
					(p.i1 - ses.peer_timestamp) << 1) >> 1;
				if (!ses.ts_valid || step < -4 * 35112)
				{
					ses.ts_valid     = true;
					ses.ts_base_peer = p.i1;
					ses.ts_base_ours = g_active ? Timestamp(*g_active) : 0;
				}
				ses.peer_timestamp = p.i1;
			}
			return;

		case kCmdCadence:
			s.clk_gap_fixed = static_cast<int32_t>(p.i1);
			Trace("in  cadence %u", p.i1);
			return;

		case kCmdTimeGrant:
			s.grant_horizon = p.i1;
			s.grant_valid   = true;
			return;

		case kCmdJoypad:
		case kCmdWantDisconnect:
			// Remote control is out of scope, and we never auto-reconnect.
			return;

		default:
			return;
	}
}

// Drain everything the transport has buffered on every channel, and drive
// the connection state machine (accepts, connects, version handshakes).
void ServiceLink(Serial &s, Memory &mem)
{
	LinkPoll();

	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
	{
		LinkSession &ses = g_sessions[ch];

		if (!LinkChannelConnected(ch))
		{
			if (ses.handshake_sent || ses.handshaked)
			{
				Trace("ch=%d dropped (was %s)", ch,
				      ses.handshaked ? "handshaked" : "connecting");
				ses = LinkSession();
				if (g_hub.hosting)
				{
					// An emptied port reads as zeroes in the broadcast from
					// here on, like a cable pulled from the real adapter.
					for (int half = 0; half < 2; ++half)
						for (int k = 0; k < g_hub.size; ++k)
							g_hub.buffer[(half * 4 + ch + 1) * g_hub.size + k] = 0;
				}
				else if (ch == 0)
				{
					// Release any waiting transfer, or the game hangs on SC
					// bit 7.
					PendingClear(s);
					if (s.active) { s.peer_valid = false; CompleteActive(s, mem); }
					s.passive = false;
				}
			}
			continue;
		}

		// A seat dropped and refilled inside one poll keeps its channel
		// number; the generation says it is a different occupant.
		const int gen = LinkChannelGeneration(ch);
		if (ses.gen != gen)
		{
			Trace("ch=%d new occupant (gen %d -> %d)", ch, ses.gen, gen);
			ses     = LinkSession();
			ses.gen = gen;
		}

		if (!ses.handshake_sent)
		{
			// Both sides open with a version packet and verify the reply.
			SendVersion(ch);
			ses.handshake_sent = true;
			if (!g_hub.hosting && ch == 0)
			{
				g_direct.transfers = 0;
				g_direct.stalled   = false;
			}
		}

		LinkPacket p;
		while (LinkRecv(ch, p)) HandlePacket(ch, s, mem, p);
	}
}

// Master side of the direct cable: the eight bit periods elapsed but the
// peer's byte has not landed yet. Give it a bounded window, servicing the
// socket throughout.
void WaitForPeer(Serial &s, Memory &mem)
{
	const int budget = g_direct.stalled ? kStalledWaitMs : kMasterWaitMs;

	int waited = 0;
	while (!s.peer_valid && waited < budget)
	{
		if (!LinkIsConnected()) return;
		LinkWaitReadable(0, kWaitSliceMs);
		waited += kWaitSliceMs;
		ServiceLink(s, mem);
	}
	g_direct.stalled = !s.peer_valid;
}

// ---- DMG-07 byte engine -----------------------------------------------------

int HubPacketLen(uint8_t phase)
{
	return (phase == kDmg07Net || phase == kDmg07Restart) ? g_hub.size * 4 : 4;
}

// Read player 1's previous wire byte the way the adapter would have heard
// it: the byte a Game Boy sends during transfer N is its answer to byte
// N-1. Player 1 is in-process, so everything that steers the adapter —
// RATE, SIZE, the $AA switch, the $FF restart — is known synchronously;
// the remote seats only feed presence bits and the data buffer, which
// arrive through HubApplyRemoteReply whenever their answers land.
void HubInterpretLocal()
{
	const uint8_t b     = g_hub.prev_local;
	const bool    armed = g_hub.prev_local_armed;

	if (g_hub.prev_phase == kDmg07Ping)
	{
		// Player 1 announcing the switch to transmission mode: the Pan
		// Docs chart shows $AA on every ping byte, so a real request is a
		// train, never a lone reply.
		if (armed && b == 0xAA)
		{
			if (g_hub.aa_run < 0xFF) ++g_hub.aa_run;
			if (g_hub.aa_run >= 3 && !g_hub.begin_sync)
			{
				Trace("p1  begin-sync ($AA train)");
				g_hub.begin_sync = true;
			}
		}
		else
		{
			g_hub.aa_run = 0;
		}

		switch (g_hub.prev_wire)
		{
			case 0:
				// Reply riding the $FE transfer: player 1's SIZE (GBE+
				// mapping, confirmed twice over — F-1 Race declares $04
				// here, and Jantaku Boy's $01 matches its confirm matcher
				// reading player N at interleave byte N-1).
				if (armed && !g_hub.begin_sync) g_hub.size_reply = b;
				break;

			case 1:
			case 2:
				// Any non-idle reply counts as plugged in (the $88 ack
				// only confirms it); $AA keeps player 1 counted while
				// the train is still short.
				if ((armed && b != 0x00 && b != 0xFF) || g_hub.begin_sync)
					g_hub.status = static_cast<uint8_t>(g_hub.status | 0x10);
				else
					g_hub.status = static_cast<uint8_t>(g_hub.status & ~0x10);
				break;

			case 3:
				// Reply riding the last STAT: player 1's RATE. Armed
				// replies only, or a menu-idled game latches garbage.
				if (armed && !g_hub.begin_sync) g_hub.rate = b;
				break;
		}
		return;
	}

	if (g_hub.prev_phase == kDmg07Net)
	{
		const int size    = g_hub.size;
		const int pkt_pos = g_hub.prev_buf % (size * 4);

		// The first SIZE transfers of a packet carry each player's fresh
		// data; player 1's lands in the half not being broadcast, one
		// packet behind (GBE+ layout).
		if (pkt_pos > 0 && pkt_pos <= size)
		{
			const int at = (g_hub.prev_buf - 1 + size * 4) % (size * 8);
			// The reply riding the wake announcement is a pre-wake block;
			// the preset length byte must survive it. One skip only — the
			// same index recurs as a normal data slot during the bulk.
			if (g_hub.wake_len_guard && at == 4)
				g_hub.wake_len_guard = false;
			else
				g_hub.buffer[at] = b;
		}
		// Only the first SIZE transfers are sampled; anything else the
		// console shifts out is discarded, which is easy to miss by eye.
		else if (armed && b != 0xFD && b != 0x00 && b != 0xFF)
			Trace("[%lld] p1 %02X DROPPED (slot %d, window 1..%d)",
			      (long long)(g_active ? g_active->real_cycles : 0), b, pkt_pos, size);

		// Player 1 pumping $FF asks for the ping phase back: three in a
		// row, armed replies only — an idle wire is a slow game, not a
		// quit. Jantaku Boy's stage-1 command blocks are [cmd,FF,FF,00],
		// so anything looser reads its FILLER as a quit and the restart
		// packet is that game's stage-1 abort signal (DI + reset).
		if (armed && b == 0xFF)
		{
			if (g_hub.ff_run < 0xFF) ++g_hub.ff_run;
			if (g_hub.ff_run == 3) Trace("p1  restart requested ($FF x3)");
			if (g_hub.ff_run >= 3) g_hub.restart_pending = true;
		}
		else if (armed)
		{
			g_hub.ff_run = 0;
		}

		HubWakeFF(0, b, armed);
		HubBulkRelayPush(0, b, armed);
	}
}

// Split-screen seats answer with real wire semantics too: the byte
// captured at event N answers byte N-1. Same rules as the socket path's
// HubApplyRemoteReply, minus the matching — nothing here can arrive late.
void HubInterpretLocalSeats()
{
	for (int k = 1; k < g_splitlink.count; ++k)
	{
		const uint8_t b     = g_splitlink.prev_seat[k - 1];
		const bool    armed = g_splitlink.prev_seat_armed[k - 1];

		if (g_hub.prev_phase == kDmg07Ping &&
		    (g_hub.prev_wire == 1 || g_hub.prev_wire == 2))
		{
			const uint8_t bit = static_cast<uint8_t>(1 << (4 + k));
			if (armed && b != 0x00 && b != 0xFF)
				g_hub.status = static_cast<uint8_t>(g_hub.status | bit);
			else
				g_hub.status = static_cast<uint8_t>(g_hub.status & ~bit);
		}

		if (g_hub.prev_phase == kDmg07Net)
		{
			const int size    = g_hub.size;
			const int pkt_pos = g_hub.prev_buf % (size * 4);
			if (pkt_pos > 0 && pkt_pos <= size)
			{
				const int base = (g_hub.prev_buf - 1 + size * 4) % (size * 8);
				g_hub.buffer[base + k * size] = armed ? b : 0x00;
			}
			else if (armed && b != 0xFD && b != 0x00 && b != 0xFF)
				Trace("[%lld] seat%d %02X DROPPED (slot %d, window 1..%d)",
				      (long long)(g_active ? g_active->real_cycles : 0), k + 1, b,
				      pkt_pos, size);
		}

		HubWakeFF(k, b, armed);
		HubBulkRelayPush(k, b, armed);
	}
}

// Phase changes land on packet edges, once every reply of the packet just
// finished has been interpreted.
void HubPacketBoundary()
{
	if (!g_hub.prev_valid) return;   // first byte of the session

	switch (g_hub.prev_phase)
	{
		case kDmg07Ping:
			if (g_hub.begin_sync)
			{
				g_hub.phase      = kDmg07Sync;
				g_hub.begin_sync = false;
				g_hub.aa_run     = 0;
			}
			break;

		case kDmg07Sync:
		{
			// The RATE and SIZE latched during ping take effect here. Pan
			// Docs: sizes 1..4 are what works without issue.
			uint8_t sz = g_hub.size_reply;
			if (sz < 1) sz = 1;
			if (sz > 4) sz = 4;
			g_hub.size = sz;

			// The real buffer starts as ping-phase garbage; games are told
			// to ignore the first packet, so zeroes are as good (GBE+ does
			// the same). Nothing is announced here: the adapter announces
			// a transmission only when a console asks for one, and a
			// console that wants the first bulk asks like any other.
			std::memset(g_hub.buffer, 0, sizeof g_hub.buffer);
			g_hub.buf_pos         = 0;
			g_hub.bulk_stage      = 0;
			g_hub.bulk_left       = 0;
			g_hub.ff_run          = 0;
			g_hub.restart_pending = false;
			g_hub.wake_pending    = false;
			g_hub.wake_len_guard  = false;
			std::memset(g_hub.wake_ff, 0, sizeof g_hub.wake_ff);
			std::memset(g_hub.wake_pairs, 0, sizeof g_hub.wake_pairs);
			// The session-start bulk is staged by the lobby parent —
			// player 1 by construction (it hosts the adapter).
			g_hub.bulk_src  = 0;
			g_hub.relay_arm = false;
			g_hub.rq_len    = 0;
			g_hub.phase           = kDmg07Net;
			break;
		}

		case kDmg07Net:
			if (g_hub.restart_pending)
			{
				g_hub.phase = kDmg07Restart;
				break;
			}
			// A seat asked for the mid-session restart: re-announce the
			// transmission. Same shape as the session-start packet — the
			// wake window plus a phase-neutral length that the consoles'
			// own post-wake replies overwrite with the real bulk length.
			if (g_hub.wake_pending && g_hub.bulk_stage == 0 && g_hub.size == 1)
			{
				std::memset(g_hub.buffer, 0, sizeof g_hub.buffer);
				std::memset(g_hub.buffer, 0xFF, 4);
				g_hub.buffer[0]    = 0xFD;
				// The requester's command byte doubles as the bulk length
				// ($0395 stages FFA9 bytes; $035C reads C200[0]). It must
				// be the first post-wake broadcast byte no matter which
				// slot the requester sits in.
				g_hub.buffer[4]        = g_hub.wake_cmd ? g_hub.wake_cmd : 0x04;
				g_hub.wake_len_guard   = true;
				g_hub.buf_pos      = 0;
				g_hub.wake_pending = false;
				g_hub.bulk_src     = static_cast<int8_t>(g_hub.wake_seat);
				g_hub.relay_arm    = false;
				g_hub.rq_len       = 0;
				std::memset(g_hub.wake_ff, 0, sizeof g_hub.wake_ff);
				std::memset(g_hub.wake_pairs, 0, sizeof g_hub.wake_pairs);
				Trace("hub wake: FD announce len=%02X src=%d",
				      g_hub.buffer[4], g_hub.bulk_src + 1);
			}
			break;

		case kDmg07Restart:
			g_hub.phase           = kDmg07Ping;
			g_hub.status          = 0;
			g_hub.ff_run          = 0;
			g_hub.aa_run          = 0;
			g_hub.restart_pending = false;
			break;
	}
}

// Put this event's byte on every port: complete the local Game Boy on the
// spot, clock each seated peer over the wire as master.
void HubSendByte(Serial &s, Memory &mem)
{
	// A completed bulk re-anchored every game's framing; restart the
	// slot cycle with them.
	if (g_hub.phase == kDmg07Net && g_hub.size == 1 && g_hub.bulk_stage == 7)
	{
		std::memset(g_hub.buffer, 0, sizeof g_hub.buffer);
		g_hub.buf_pos    = 0;
		g_hub.wire       = 0;
		g_hub.bulk_stage = 0;
		// Replies that crossed during the wake and bulk were pre-wake
		// blocks, not fresh requests; drop anything they armed.
		g_hub.wake_pending   = false;
		g_hub.wake_len_guard = false;
		g_hub.bulk_src       = -1;
		g_hub.relay_arm      = false;
		g_hub.rq_len         = 0;
		std::memset(g_hub.wake_ff, 0, sizeof g_hub.wake_ff);
		std::memset(g_hub.wake_pairs, 0, sizeof g_hub.wake_pairs);
		Trace("hub bulk re-anchor");
	}

	// Relay bytes replace the slot buffer while a sourced bulk is in
	// flight; popped before the tracker so the countdown sees them.
	int relay_byte = -1;
	if (g_hub.phase == kDmg07Net && g_hub.bulk_stage == 6 &&
	    g_hub.bulk_src >= 0 && g_hub.rq_len)
	{
		relay_byte = g_hub.rq[g_hub.rq_head];
		g_hub.rq_head = static_cast<uint8_t>((g_hub.rq_head + 1) % sizeof g_hub.rq);
		--g_hub.rq_len;
	}

	uint8_t out[4];
	switch (g_hub.phase)
	{
		case kDmg07Ping:
			if (g_hub.wire == 0)
			{
				out[0] = out[1] = out[2] = out[3] = 0xFE;
			}
			else
			{
				// STAT bytes: presence up top, the port's own player number
				// below, which is how each Game Boy learns who it is.
				for (int p = 0; p < 4; ++p)
					out[p] = static_cast<uint8_t>((g_hub.status & 0xF0) | (p + 1));
			}
			break;

		case kDmg07Sync:
			out[0] = out[1] = out[2] = out[3] = 0xCC;
			break;

		case kDmg07Net:
			out[0] = out[1] = out[2] = out[3] =
			    relay_byte >= 0 ? static_cast<uint8_t>(relay_byte)
			                    : g_hub.buffer[g_hub.buf_pos];
			break;

		default:
			out[0] = out[1] = out[2] = out[3] = 0xFF;
			break;
	}

	// Track the wake-window + LEN-bulk sequence on the broadcast stream
	// so the slot cycle can re-anchor when the games' framing shifts.
	// Signalling games only: a streaming game's payload will contain
	// this byte sequence eventually, and it means nothing there.
	if (g_hub.phase == kDmg07Net && g_hub.size == 1)
	{
		const uint8_t ob = out[0];
		if (g_hub.bulk_stage < 4)
		{
			if (ob == 0xFD)                            g_hub.bulk_stage = 1;
			else if (g_hub.bulk_stage && ob == 0xFF)   ++g_hub.bulk_stage;
			else                                       g_hub.bulk_stage = 0;
		}
		else if (g_hub.bulk_stage == 4)
		{
			// This byte is the bulk length and also its first transfer.
			Trace("hub bulk len=%02X", ob);
			if (ob <= 1) g_hub.bulk_stage = ob ? 7 : 0;
			else { g_hub.bulk_left = static_cast<uint8_t>(ob - 1); g_hub.bulk_stage = 6; }
			// The source's next reply is its first staged payload byte
			// ($0345 loads C300[0] in the ISR that receives this length).
			g_hub.relay_arm = true;
			g_hub.rq_len    = 0;
			g_hub.rq_head   = 0;
		}
		else if (g_hub.bulk_stage == 6)
		{
			if (--g_hub.bulk_left == 0) g_hub.bulk_stage = 7;
		}
	}

	// Player 1 is in-process: complete its armed transfer immediately. An
	// unarmed Game Boy leaves the wire idle, which reads back as $FF.
	g_hub.prev_local = 0xFF;
	const bool p1_armed = s.passive;
	if (s.passive)
	{
		g_hub.prev_local   = mem.serial_data;
		mem.serial_data    = out[0];
		mem.serial_control = static_cast<uint8_t>(mem.serial_control & 0x7F);
		mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);
		s.passive          = false;
	}
	Trace("[%lld] ev  ph=%u wire=%u buf=%u out=%02X st=%02X p1=%02X%s",
	      (long long)s.real_cycles, g_hub.phase, g_hub.wire, g_hub.buf_pos,
	      out[0], g_hub.status, g_hub.prev_local, p1_armed ? "" : " (idle)");

	if (g_hub.local)
	{
		// Split screen: every seat is in-process. Complete each armed
		// slave on the spot and keep its outgoing byte for the next
		// event's interpretation — the same wire semantics as player 1.
		for (int k = 1; k < g_splitlink.count; ++k)
		{
			Serial *ss = g_splitlink.s[k];
			Memory *sm = g_splitlink.m[k];

			g_splitlink.prev_seat[k - 1]       = 0xFF;
			g_splitlink.prev_seat_armed[k - 1] = false;
			if (ss && ss->passive)
			{
				g_splitlink.prev_seat[k - 1]       = sm->serial_data;
				g_splitlink.prev_seat_armed[k - 1] = true;
				sm->serial_data    = out[k];
				sm->serial_control = static_cast<uint8_t>(sm->serial_control & 0x7F);
				sm->if_            = static_cast<uint8_t>(sm->if_ | IRQ_SERIAL);
				ss->passive        = false;
			}
			Trace("    seat%d in=%02X reply=%02X%s", k + 1, out[k],
			      g_splitlink.prev_seat[k - 1],
			      g_splitlink.prev_seat_armed[k - 1] ? "" : " (idle)");
		}
	}
	else
	{
		// What the answer to this byte will mean, frozen before anything
		// can change phase under it.
		int8_t presence_wire = -1;
		if (g_hub.phase == kDmg07Ping && (g_hub.wire == 1 || g_hub.wire == 2))
			presence_wire = static_cast<int8_t>(g_hub.wire);

		int store_base = -1;
		if (g_hub.phase == kDmg07Net)
		{
			const int size    = g_hub.size;
			const int pkt_pos = g_hub.buf_pos % (size * 4);
			if (pkt_pos > 0 && pkt_pos <= size)
				store_base = (g_hub.buf_pos - 1 + size * 4) % (size * 8);
		}

		for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		{
			LinkSession &ses = g_sessions[ch];
			if (!LinkChannelConnected(ch) || !ses.handshaked) continue;

			HubReplyTag tag;
			tag.seq           = ses.seq_next++;
			tag.presence_wire = presence_wire;
			tag.store_at      = store_base >= 0
			                  ? static_cast<int16_t>(store_base + (ch + 1) * g_hub.size)
			                  : static_cast<int16_t>(-1);
			HubFifoPush(ses, tag);

			SendPacket(ch, kCmdSync1, out[ch + 1], 0x81, 0, tag.seq);
			Trace("    tx  ch=%d seq=%u out=%02X store=%d pw=%d fifo=%u",
			      ch, tag.seq, out[ch + 1], tag.store_at, tag.presence_wire,
			      ses.fifo_len);
		}
	}

	// Jantaku Boy investigation: each split seat's live lobby state —
	// mode $C0C0, ISR stage $FF99, reply $FFA1, gate $FF96, sub-state
	// counters $C0B1/$C0B2/$C0BF, command source $FFA9, gate table $C6AE.
	if (g_hub.local && TraceOn())
	{
		char st[560];
		int  n = 0;
		for (int k = 0; k < g_splitlink.count && n < (int)sizeof(st) - 136; ++k)
		{
			Memory *m = g_splitlink.m[k];
			if (!m) continue;
			// j = live held GB buttons: high nibble A/B/Sel/Start, low
			// nibble R/L/U/D, 1 = pressed (inverted from the wire).
			const uint8_t held = m->joypad
				? static_cast<uint8_t>(((~m->joypad->btns & 0x0F) << 4) |
				                       (~m->joypad->dpad & 0x0F))
				: 0;
			n += std::snprintf(st + n, sizeof(st) - (size_t)n,
			                   " p%d[m=%02X s=%02X r=%02X%02X%02X%02X a=%02X g=%02X j=%02X e=%02X.%02X b=%02X.%02X.%02X.%02X t=%02X.%02X.%02X w=%02X:%02X%02X%02X%02X]",
			                   k + 1, m->wram[0x00C0], m->hram[0x19],
			                   m->hram[0x21], m->hram[0x22], m->hram[0x23],
			                   m->hram[0x24], m->hram[0x26],
			                   m->hram[0x16], held,
			                   m->ie, m->if_,
			                   m->wram[0x00B1], m->wram[0x00B2], m->wram[0x00B3],
			                   m->wram[0x00BF], m->wram[0x06A0], m->wram[0x06A1],
			                   m->wram[0x06A2],
			                   m->hram[0x1A], m->hram[0x1D], m->hram[0x1E],
			                   m->hram[0x1F], m->hram[0x20]);
		}
		Trace("    state%s", st);
	}

	g_hub.prev_phase       = g_hub.phase;
	g_hub.prev_wire        = g_hub.wire;
	g_hub.prev_buf         = g_hub.buf_pos;
	g_hub.prev_local_armed = p1_armed;
	g_hub.prev_valid       = true;
}

// Step the wire position and schedule the next byte event.
void HubAdvance()
{
	const int len = HubPacketLen(g_hub.phase);
	const int hi  = g_hub.rate >> 4;
	const int lo  = g_hub.rate & 0x0F;

	if (g_hub.phase == kDmg07Net)
		g_hub.buf_pos = static_cast<uint8_t>((g_hub.buf_pos + 1) % (g_hub.size * 8));

	g_hub.wire = static_cast<uint8_t>((g_hub.wire + 1) % len);

	int32_t delay;
	if (g_hub.phase == kDmg07Ping || g_hub.phase == kDmg07Sync)
	{
		if (g_hub.wire != 0)
			delay = kPingByteGap;
		else if (g_hub.phase == kDmg07Sync && !g_hub.local)
			delay = kSyncSettle;   // seats over sockets need the room
		else
			delay = kPingRest + lo * kCyclesPerMs;
	}
	else
	{
		const int32_t per_byte = kNetByteBase + hi * kNetByteStep;
		if (per_byte != g_hub.byte_gap_sent && !g_hub.local)
		{
			for (int ch = 0; ch < kLinkMaxChannels; ++ch)
				if (g_sessions[ch].handshaked)
					SendPacket(ch, kCmdCadence, 0, 0, 0,
					           static_cast<uint32_t>(per_byte));
			g_hub.byte_gap_sent = per_byte;
		}
		g_hub.byte_gap = per_byte;
		if (g_hub.wire != 0)
		{
			delay = per_byte;
		}
		else
		{
			// The packet repeats at whichever is slower: its own bytes or
			// the RATE floor.
			const int32_t floor_len = kNetPacketMin + lo * kCyclesPerMs;
			const int32_t body      = per_byte * (len - 1) + kNetPacketPad;
			const int32_t total     = body > floor_len ? body : floor_len;
			delay = total - per_byte * (len - 1);
			// Never below the byte period: a shorter tail lets the next
			// packet's first byte nest inside a preempted serial ISR.
			if (delay < per_byte)
				delay = per_byte;
		}
	}
	g_hub.timer += delay;
}

void HubRunByte(Serial &s, Memory &mem)
{
	// In-process net phase: hold the byte event while a console is
	// between re-arms — the interleave can put a seat a few hundred
	// cycles "behind" an instant that real wiring spreads over a
	// millisecond. Clocking past a late console trails its slot counter
	// for good, so hold for a byte period; signalling games wait longer.
	if (g_hub.local && g_hub.phase == kDmg07Net)
	{
		bool all_armed = s.passive;
		for (int k = 1; k < g_splitlink.count && all_armed; ++k)
			if (g_splitlink.s[k] && !g_splitlink.s[k]->passive)
				all_armed = false;
		int cap = g_hub.byte_gap / 456;
		if (cap < 8)             cap = 8;
		if (g_hub.size == 1)     cap = 64;
		if (!all_armed && g_hub.event_grace < cap)
		{
			++g_hub.event_grace;
			g_hub.timer += 456;
			return;
		}
		g_hub.event_grace = 0;
	}

	// Never blocks: remote answers land through HubApplyRemoteReply as
	// they arrive; one extra drain here just shortens their latency. A
	// split-screen hub has no sockets to service at all.
	if (!g_hub.local) ServiceLink(s, mem);

	// A sourced bulk relays one staged byte per transfer, and the seat
	// staging it produces each byte only when clocked for the one
	// before. In process that answer is instant; over a socket the seat
	// is a whole emulator running in frame bursts, so the byte can be
	// milliseconds out. Hold the event for it rather than broadcasting
	// a gap — the consoles count bulk bytes, and one wrong byte rotates
	// their framing for the rest of the session. The cap is the same
	// idea as the in-process arm grace: long enough for a seat that is
	// merely late, short enough to give up on one that has stopped.
	if (!g_hub.local && g_hub.phase == kDmg07Net && g_hub.bulk_stage == 6 &&
	    g_hub.bulk_src > 0 && g_hub.rq_len == 0)
	{
		if (g_hub.event_grace < 64)
		{
			++g_hub.event_grace;
			g_hub.timer += 456;
			return;
		}
		Trace("hub bulk relay starved (seat %d)", g_hub.bulk_src + 1);
	}
	g_hub.event_grace = 0;

	if (g_hub.prev_valid)
	{
		HubInterpretLocal();
		if (g_hub.local) HubInterpretLocalSeats();
	}
	const uint8_t ph_before = g_hub.phase;
	if (g_hub.wire == 0) HubPacketBoundary();
	if (g_hub.phase != ph_before)
		Trace("[%lld] hub phase %u -> %u (size=%u rate=%02X status=%02X)",
		      (long long)s.real_cycles, ph_before, g_hub.phase,
		      g_hub.size, g_hub.rate, g_hub.status);

	HubSendByte(s, mem);
	HubAdvance();
}

void HubStep(Serial &s, Memory &mem, int32_t real_cycles)
{
	g_hub.timer -= real_cycles;
	while (g_hub.timer <= 0)
		HubRunByte(s, mem);   // HubAdvance always pushes the timer forward
}

} // anonymous

void SetSerialCallback(SerialByteCallback cb) { g_serial_cb = cb; }

void SerialReset(Serial &s, bool cgb)
{
	// A reset does not unplug the cable, so only the transfer machinery is
	// rebuilt; the per-channel handshakes belong to the connections and
	// survive untouched in g_sessions.
	const Serial fresh;
	s     = fresh;
	s.cgb = cgb;

	// Our clock just restarted; the lockstep baselines re-latch on the
	// next timestamps or every peer would read us as eternally behind.
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		g_sessions[ch].ts_valid = false;

	g_active         = &s;
	g_direct.stalled = false;

	if (g_hub.hosting)
	{
		// The console reset does not power the adapter down, but this
		// session of it starts over from the ping phase. In-flight clocks
		// belong to the old run; their late answers fall away.
		Dmg07 fresh_hub;
		fresh_hub.hosting = true;
		fresh_hub.local   = g_hub.local;
		fresh_hub.target  = g_hub.target;
		g_hub = fresh_hub;

		for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		{
			g_sessions[ch].fifo_head = 0;
			g_sessions[ch].fifo_len  = 0;
		}
		for (int k = 0; k < 3; ++k)
		{
			g_splitlink.prev_seat[k]       = 0xFF;
			g_splitlink.prev_seat_armed[k] = false;
		}
	}
}

void SerialAfterStateLoad(Serial &s, Memory &mem)
{
	g_active     = &s;
	g_active_mem = &mem;

	// Anything buffered belongs to the timeline just replaced; applying it
	// to the restored state would shift a byte into the wrong transfer.
	// The sequence counters stay: late answers to flushed clocks miss the
	// emptied queues and fall away instead of landing on the wrong byte.
	LinkFlush();
	g_direct.driving    = 0;
	g_direct.unanswered = 0;
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
	{
		g_sessions[ch].fifo_head = 0;
		g_sessions[ch].fifo_len  = 0;
	}

	s.active        = false;
	s.passive       = false;
	s.bits_left     = 0;
	s.bit_timer     = 0;
	s.peer_valid    = false;
	s.peer_supplied = false;
	s.peer_data     = 0xFF;
	s.ext_gap_timer = 0;
	s.clk_gap_fixed = 0;
	s.grant_valid   = false;
	PendingClear(s);

	if (g_hub.hosting)
	{
		// The adapter's phase belongs to the connection, not the state;
		// restart from ping and let the games renegotiate.
		Dmg07 fresh_hub;
		fresh_hub.hosting = true;
		fresh_hub.local   = g_hub.local;
		fresh_hub.target  = g_hub.target;
		g_hub = fresh_hub;

		if ((mem.serial_control & 0x80) != 0 && (mem.serial_control & 0x01) == 0)
			s.passive = true;
		return;
	}

	if ((mem.serial_control & 0x80) == 0) return;

	if (mem.serial_control & 0x01)
	{
		// Internal clock was mid-transfer. Re-arm locally without putting
		// another sync1 on the wire — the peer never saw this rewind, and
		// replaying the byte would duplicate it.
		s.active     = true;
		s.bits_left  = 8;
		s.bit_period = BitPeriod(s, mem.serial_control);
		s.bit_timer  = s.bit_period;
	}
	else
	{
		s.passive = true;
	}
}

uint8_t SerialReadSC(const Serial &s, const Memory &mem)
{
	// Unused bits read back as 1. Bit 1 is only a real bit on CGB, so on
	// DMG it joins the open-bit mask.
	const uint8_t used = s.cgb ? 0x83 : 0x81;
	return static_cast<uint8_t>((mem.serial_control & used) |
	                            static_cast<uint8_t>(~used));
}

// One shared clock tick of eight bits: every seat's SB moves one hop down
// the ring simultaneously. Armed ports (SC.7, either clock source) get the
// completion interrupt; unarmed ones shift silently, which is also what
// carries bytes across a unit still sitting in its menus.
static void RingExchange()
{
	const int n = g_splitlink.count;
	uint8_t out[kLinkMaxSeats];
	for (int k = 0; k < n; ++k)
		out[k] = g_splitlink.m[k] ? g_splitlink.m[k]->serial_data : 0xFF;

	for (int k = 0; k < n; ++k)
	{
		Serial *ss = g_splitlink.s[k];
		Memory *sm = g_splitlink.m[k];
		if (!ss || !sm) continue;
		sm->serial_data = out[(k + n - 1) % n];
		if (sm->serial_control & 0x80)
		{
			sm->serial_control = static_cast<uint8_t>(sm->serial_control & 0x7F);
			sm->if_            = static_cast<uint8_t>(sm->if_ | IRQ_SERIAL);
			ss->active = ss->passive = false;
		}
	}
	g_ring.shifting = false;
	++g_ring.xfers;

	if (TraceOn())
	{
		char line[16 + kLinkMaxSeats * 3];
		int  p = 0;
		for (int k = 0; k < n && p < (int)sizeof(line) - 4; ++k)
			p += snprintf(line + p, sizeof(line) - (size_t)p, " %02X", out[k]);
		Trace("ring %llu:%s", (unsigned long long)g_ring.xfers, line);
	}
}

void SerialWriteSC(Serial &s, Memory &mem, uint8_t value)
{
	mem.serial_control = value;

	if (value & 0x80)
	{
		int seat = 0;
		if (SerialSplitActive())
			for (int i = 0; i < g_splitlink.count; ++i)
				if (&s == g_splitlink.s[i]) { seat = i + 1; break; }
		if (seat)
			Trace("[%lld] SC<=%02X SB=%02X seat%d", (long long)s.real_cycles,
			      value, mem.serial_data, seat);
		else
			Trace("[%lld] SC<=%02X SB=%02X %s", (long long)s.real_cycles, value,
			      mem.serial_data,
			      g_hub.hosting ? "hub-slave" :
			      LinkIsConnected() ? "link" : "stub");
	}

	if (g_hub.hosting && (g_hub.local || LinkGetState() != LinkState::Off))
	{
		// Under the adapter every Game Boy is an external-clock slave, our
		// own included — the adapter pings it even with no peers seated
		// yet. Driving the internal clock yields garbage on hardware; the
		// unlinked stub models that closely enough. Split-screen seats all
		// take this same path: the adapter completes them directly.
		if (!(value & 0x80))
		{
			s.active = s.passive = false;
			return;
		}
		if (value & 0x01)
		{
			// Driving the internal clock under the adapter reads the idle
			// wire, but only after the real eight bit periods.
			if (g_serial_cb) g_serial_cb(mem.serial_data);
			Trace("[%lld] MASTER WRITE under hub -> timed $FF",
			      (long long)s.real_cycles);
			s.passive       = false;
			s.active        = true;
			s.bits_left     = 8;
			s.bit_period    = BitPeriod(s, value);
			s.bit_timer     = s.bit_period;
			s.peer_valid    = true;
			s.peer_data     = 0xFF;
			s.peer_supplied = false;
			return;
		}
		s.active  = false;
		s.passive = true;
		return;
	}

	if (g_ring.active)
	{
		// Faceball's ring cable: the master's start bit clocks everyone,
		// so arming is just bookkeeping — RingExchange completes every
		// armed port at once when the eight bit periods elapse. A second
		// $81 while a byte is in flight ORs into the same clock.
		if (!(value & 0x80))
		{
			s.active = s.passive = false;
			return;
		}
		if (value & 0x01)
		{
			if (g_serial_cb) g_serial_cb(mem.serial_data);
			s.active = s.passive = false;
			if (!g_ring.shifting)
			{
				g_ring.shifting = true;
				g_ring.timer    = 8 * BitPeriod(s, value);
			}
			return;
		}
		s.active  = false;
		s.passive = true;
		return;
	}

	if (SerialSplitActive())
	{
		// In-process direct cable: the same arming as the socket cable,
		// minus the packets — the exchange resolves when the countdown
		// completes, against the other core's live state.
		if (!(value & 0x80))
		{
			s.active = s.passive = false;
			return;
		}
		if (value & 0x01)
		{
			if (g_serial_cb) g_serial_cb(mem.serial_data);
			s.passive       = false;
			s.active        = true;
			s.bits_left     = 8;
			s.bit_period    = BitPeriod(s, value);
			s.bit_timer     = s.bit_period;
			s.peer_valid    = false;
			s.peer_supplied = false;
			s.peer_data     = 0xFF;
			s.split_grace   = 912;   // two interleave slices of skew
			return;
		}
		s.active  = false;
		s.passive = true;
		if (s.ext_gap_timer < kExtShiftTime) s.ext_gap_timer = kExtShiftTime;
		return;
	}

	if (!LinkIsConnected())
	{
		// Unlinked stub path: internal clock shifts the idle wire's $FF in
		// over the real eight bit periods; external leaves bit 7 set,
		// which is how games spot a missing cable.
		s.active = s.passive = false;
		if ((value & 0x81) == 0x81)
		{
			if (g_serial_cb) g_serial_cb(mem.serial_data);
			s.active        = true;
			s.bits_left     = 8;
			s.bit_period    = BitPeriod(s, value);
			s.bit_timer     = s.bit_period;
			s.peer_valid    = true;
			s.peer_data     = 0xFF;
			s.peer_supplied = false;
		}
		return;
	}

	if (!(value & 0x80))
	{
		// Start bit cleared — abandon whatever was armed.
		s.active = s.passive = false;
		return;
	}

	if (value & 0x01)
	{
		// Internal clock: we drive. Byte goes out now; the bit countdown
		// decides when the answer is due.
		if (g_serial_cb) g_serial_cb(mem.serial_data);

		s.passive       = false;
		s.active        = true;
		s.bits_left     = 8;
		s.bit_period    = BitPeriod(s, value);
		s.bit_timer     = s.bit_period;
		s.peer_valid    = false;
		s.peer_supplied = false;
		s.peer_data     = 0xFF;

		// The peer is clocking too; ack its bytes rather than swallow them.
		while (s.pending_len)
			PendingAckDropOldest(0, s);

		uint8_t control = 0x81;
		if (value & 0x02)     control = static_cast<uint8_t>(control | 0x02);
		if (mem.double_speed) control = static_cast<uint8_t>(control | 0x04);
		SendPacket(0, kCmdSync1, mem.serial_data, control, 0, Timestamp(s));
		return;
	}

	// External clock: arm and wait to be clocked by the peer. A held byte
	// settles from SerialStep once the shift time has passed, so a backlog
	// drains one re-arm at a time at hardware pace and never lands inside
	// the instruction that armed the port.
	s.active  = false;
	s.passive = true;
	if (s.ext_gap_timer < kExtShiftTime) s.ext_gap_timer = kExtShiftTime;
}

void SerialStep(Serial &s, Memory &mem, int32_t tcycles)
{
	if (tcycles <= 0) return;
	g_active     = &s;
	g_active_mem = &mem;

	// Timestamps are real time, so undo the double-speed doubling that
	// the CPU-domain cycle count carries.
	int32_t real = tcycles;
	if (mem.double_speed)
	{
		s.ds_remainder += tcycles;
		real            = s.ds_remainder >> 1;
		s.real_cycles  += real;
		s.ds_remainder &= 1;
	}
	else
	{
		s.real_cycles += tcycles;
	}

	if (!SerialLinkIsEnabled() && !SerialSplitActive())
	{
		// No session: only the stub's timed $FF countdown has work here.
		StepResolvedActive(s, mem, tcycles);
		return;
	}

	if (!SerialSplitActive())
	{
		s.poll_timer -= tcycles;
		if (s.poll_timer <= 0)
		{
			s.poll_timer = kPollInterval;
			ServiceLink(s, mem);
		}
	}

	if (g_hub.hosting)
	{
		// A master write under the adapter completes as a timed $FF.
		StepResolvedActive(s, mem, tcycles);

		// The adapter free-runs from power-on, peers or none: a lone
		// player 1 still sees itself connected, exactly like hardware.
		// In split screen only seat 0's core clocks it; the other cores
		// are pure slaves with nothing of their own to step.
		if (!g_hub.local || &s == g_splitlink.s[0])
			HubStep(s, mem, real);
		return;
	}

	if (g_ring.active)
	{
		// The shared clock advances on seat 0's core alone, like the hub;
		// the ROM's master re-arms only from its ISR, milliseconds apart,
		// so the one-slice skew between seats never outruns it.
		if (g_ring.shifting && &s == g_splitlink.s[0])
		{
			g_ring.timer -= real;
			if (g_ring.timer <= 0) RingExchange();
		}
		return;
	}

	if (!LinkIsConnected() && !SerialSplitActive())
	{
		// Session up but nobody attached yet: same stub semantics.
		StepResolvedActive(s, mem, tcycles);
		return;
	}

	if (s.active)
	{
		s.bit_timer -= tcycles;
		while (s.bit_timer <= 0 && s.bits_left > 0)
		{
			--s.bits_left;
			s.bit_timer += s.bit_period;
		}
		if (s.bits_left == 0)
		{
			bool settle = true;
			if (!s.peer_valid)
			{
				if (SerialSplitActive())
				{
					if (!SplitTryResolveMaster(s, mem))
					{
						// The other core lags by up to an interleave slice;
						// an arm resolved away this instant may already have
						// happened in its near future. Wait out the skew
						// before calling the wire idle.
						if (s.split_grace > 0)
						{
							s.split_grace -= tcycles;
							settle = false;
						}
						else
						{
							Trace("[%lld] split master unanswered -> FF (held)",
							      (long long)s.real_cycles);
							SplitHoldClock(s, mem.serial_data);
							s.peer_data     = 0xFF;
							s.peer_valid    = true;
							s.peer_supplied = false;
						}
					}
				}
				else
				{
					ServiceLink(s, mem);
					if (!s.peer_valid) WaitForPeer(s, mem);
				}
			}
			if (settle) CompleteActive(s, mem);
		}
	}

	// Timestamps are a socket concept; held clocks are not. Deliver one
	// when this core arms, paced like the socket path, and let a byte
	// nobody ever collected age out after a frame.
	if (SerialSplitActive())
	{
		if (s.ext_gap_timer > 0) s.ext_gap_timer -= tcycles;
		SplitSettlePending(s, mem);
		if (s.pending_len && !s.passive)
		{
			s.pending_age += tcycles;
			if (s.pending_age >= kPendingHoldActive)
			{
				uint8_t  stale;
				uint32_t unused;
				PendingPop(s, stale, unused);
			}
		}
		return;
	}

	if (s.ext_gap_timer > 0) s.ext_gap_timer -= tcycles;
	TrySettlePending(s, mem);

	// Aging only while the game is not armed: an armed game is merely
	// waiting out the pacing gap and will take the byte in under a
	// millisecond.
	if (s.pending_len && !s.passive)
	{
		s.pending_age += tcycles;
		const int32_t hold = (g_direct.unanswered >= kUnansweredIdle)
		                         ? kPendingHoldIdle : kPendingHoldActive;
		if (s.pending_age >= hold)
		{
			// Our game never armed its side; release the oldest clock so
			// the peer stops waiting on it.
			PendingAckDropOldest(0, s);
		}
	}

	s.ts_send_timer -= tcycles;
	if (s.ts_send_timer <= 0)
	{
		s.ts_send_timer = kTimestampInterval;
		const uint32_t ours = Timestamp(s);
		const bool host = g_hub.hosting || g_role == LinkRole::Server;
		for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		{
			if (ch != 0 && !host) break;
			if (!g_sessions[ch].handshaked) continue;
			SendPacket(ch, kCmdSync3, 0, 0, 0, ours, true);
		}

		// The host is the clock owner: grant every seat a horizon just
		// past the slowest seat's progress, in that seat's own clock.
		// Everything then slows together instead of drifting apart.
		if (host)
		{
			bool     any = false;
			int32_t  slowest = 0;
			for (int ch = 0; ch < kLinkMaxChannels; ++ch)
			{
				const LinkSession &ses = g_sessions[ch];
				if (!ses.handshaked || !ses.peer_running || !ses.ts_valid) continue;
				if (!LinkChannelConnected(ch)) continue;
				const int32_t prog = static_cast<int32_t>(
					(ses.peer_timestamp - ses.ts_base_peer) << 1) >> 1;
				if (!any || prog < slowest) { slowest = prog; any = true; }
			}
			if (any)
				for (int ch = 0; ch < kLinkMaxChannels; ++ch)
				{
					const LinkSession &ses = g_sessions[ch];
					if (!ses.handshaked || !ses.ts_valid) continue;
					if (!LinkChannelConnected(ch)) continue;
					const uint32_t horizon = (ses.ts_base_peer +
						static_cast<uint32_t>(slowest) +
						static_cast<uint32_t>(kLockstepWindow)) & 0x7FFFFFFF;
					SendPacket(ch, kCmdTimeGrant, 0, 0, 0, horizon, true);
				}
		}

		// A wall-clock beacon once a second, so logs from separate
		// processes merge into one real-time timeline.
		static int64_t last_wall = 0;
		const int64_t wall = static_cast<int64_t>(time(nullptr));
		if (wall != last_wall)
		{
			last_wall = wall;
			Trace("[%lld] wall %lld", (long long)s.real_cycles,
			      (long long)wall);
		}
	}
}

// ---- Link session control --------------------------------------------------

LinkRole SerialLinkAutoStart(uint16_t port, int players, bool force_hub,
                             char *err, size_t err_cap, bool client_only)
{
	SerialLinkDisconnect();

	// A spawned seat never hosts: the master owns the port. If nobody is
	// listening, the master is gone and this window stays unplugged.
	if (client_only)
	{
		if (LinkStartClient("127.0.0.1", port, err, err_cap))
		{
			g_role = LinkRole::Client;
			Trace("session: client (spawned seat), port=%u", port);
		}
		else
			g_role = LinkRole::None;
		return g_role;
	}

	// The PC tracer rides the CPU trace hook whenever the wire trace is
	// on, socket sessions included — instance runs need it most.
	if (TraceOn())
	{
		std::memset(g_pc_hits, 0, sizeof g_pc_hits);
		Cpu::SetTraceHook(SerialPcHook);
	}

	if (players > 2 || force_hub)
	{
		// The hub must own the port: the adapter lives here, and the
		// joining instances are launched to dial in. Nothing to take over
		// from — a survivor of an old session frees the port on unlink.
		const int clamped = players > 4 ? 4 : (players < 2 ? 2 : players);
		const int seats   = clamped - 1;
		if (LinkStartServer(port, seats, err, err_cap))
		{
			g_hub          = Dmg07();
			g_hub.hosting  = true;
			g_hub.target   = static_cast<uint8_t>(seats + 1);
			g_hub_reported = -1;
			g_role         = LinkRole::Server;
			Trace("session: hub host, target=%d players, port=%u", seats + 1, port);
			return g_role;
		}
		if (players > 2)
		{
			g_role = LinkRole::None;
			return g_role;
		}
		// A 2-player adapter session losing the bind can mean the partner
		// window already hosts it — fall through and dial in.
	}

	// Listener first: winning the bind means we are the first instance up,
	// losing it means the other one is already waiting.
	if (LinkStartServer(port, 1, err, err_cap))
	{
		g_role = LinkRole::Server;
		Trace("session: direct cable server, port=%u", port);
		return g_role;
	}

	if (LinkStartClient("127.0.0.1", port, err, err_cap))
	{
		g_role = LinkRole::Client;
		Trace("session: client, port=%u", port);
		return g_role;
	}

	// Port was taken but nothing answered: the other instance was on its
	// way out, so take it over rather than failing.
	if (LinkStartServer(port, 1, err, err_cap))
	{
		g_role = LinkRole::Server;
		return g_role;
	}

	g_role = LinkRole::None;
	return g_role;
}

LinkRole SerialLinkGetRole()
{
	return SerialLinkIsEnabled() ? g_role : LinkRole::None;
}

void SerialLinkDisconnect()
{
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		if (LinkChannelConnected(ch) && g_sessions[ch].handshaked &&
		    g_sessions[ch].peer_reconnect_ok)
			SendPacket(ch, kCmdWantDisconnect, 0, 0, 0, 0);

	LinkStop();
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		g_sessions[ch] = LinkSession();
	g_direct       = DirectState();
	// A split-screen hub has no sockets; unplugging those must not kill it.
	if (!g_hub.local) g_hub = Dmg07();
	g_hub_reported = -1;
	g_role         = LinkRole::None;

	if (g_active)
	{
		PendingClear(*g_active);
		g_active->peer_valid = false;
		g_active->active     = false;
		g_active->passive    = false;
	}
}

bool SerialLinkIsEnabled() { return LinkGetState() != LinkState::Off; }

bool SerialLinkIsConnected()
{
	if (!LinkIsConnected()) return false;
	if (g_hub.hosting) return HandshakedCount() > 0;
	return g_sessions[0].handshaked;
}

void SerialLinkPump()
{
	if (g_active && g_active_mem) ServiceLink(*g_active, *g_active_mem);
	else                          LinkPoll();
}

// Timestamp lockstep: true while our emulated clock has outrun a linked
// peer's by more than hardware skew ever could. The caller idles the
// frame and keeps the socket pumped; the behind side never holds, so
// the pair converges instead of deadlocking.
bool SerialLinkShouldHoldFrame()
{
	if (!g_active || LinkGetState() != LinkState::Connected) return false;

	// Never hold while peer clocks sit in the queue: only a running game
	// can arm the port and answer them, and the peer is blocked on that
	// answer - holding here deadlocks both sides into a timeout.
	if (g_active->pending_len) return false;

	// Barrier lockstep: a granted horizon is absolute - the session
	// slows down together instead of letting one instance drift.
	if (g_active->grant_valid)
	{
		const int32_t past = static_cast<int32_t>(
			(Timestamp(*g_active) - g_active->grant_horizon) << 1) >> 1;
		if (past > 0) return true;
	}

	const uint32_t ours = Timestamp(*g_active);
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
	{
		const LinkSession &ses = g_sessions[ch];
		if (!ses.handshaked || !ses.peer_running || !ses.ts_valid) continue;
		if (!LinkChannelConnected(ch)) continue;

		const int32_t we_ran = static_cast<int32_t>(
			(ours - ses.ts_base_ours) << 1) >> 1;
		const int32_t they_ran = static_cast<int32_t>(
			(ses.peer_timestamp - ses.ts_base_peer) << 1) >> 1;
		if (we_ran - they_ran > kLockstepWindow) return true;
	}
	return false;
}

// What the OSD is currently saying: 0 nothing to report, 1 we drive,
// 2 the peer drives, 3 we drive but the other game never answers,
// 4 linked with nothing exchanged yet.
uint8_t LinkDisplayState()
{
	if (!SerialLinkIsConnected()) return 0;
	if (g_direct.driving)
	{
		// Both directions completing recently means the games clock each
		// other by turns - neither Master nor Passive alone is true.
		constexpr int64_t kBothWindow = 30 * 70224;
		const int64_t now = g_active ? g_active->real_cycles : 0;
		if (g_direct.last_drive_us >= 0 && g_direct.last_drive_peer >= 0 &&
		    now - g_direct.last_drive_us   < kBothWindow &&
		    now - g_direct.last_drive_peer < kBothWindow)
			g_direct.bidir_seen = true;
		if (g_direct.bidir_seen) return 5;
		return g_direct.driving;
	}
	return g_direct.unanswered ? 3 : 4;
}

int SerialLinkPlayerCount()
{
	if (!SerialLinkIsEnabled()) return 0;
	if (g_hub.hosting) return 1 + HandshakedCount();
	return SerialLinkIsConnected() ? 2 : 1;
}

void SerialSplitAttach(Serial *const serials[], Memory *const mems[], int count,
                       bool force_hub, bool ring)
{
	// Split screen and the socket link are mutually exclusive sessions.
	SerialLinkDisconnect();
	SerialSplitDetach();

	if (count < 2 || !serials || !mems) return;
	if (count > (ring ? kLinkMaxSeats : 4)) count = ring ? kLinkMaxSeats : 4;

	g_splitlink.count = count;
	for (int i = 0; i < count; ++i)
	{
		g_splitlink.s[i] = serials[i];
		g_splitlink.m[i] = mems[i];
		// A byte held from an earlier session belongs to nobody here.
		PendingClear(*serials[i]);
	}

	if (ring)
	{
		g_ring = FaceRing();
		g_ring.active = true;
	}
	else if (count > 2 || force_hub)
	{
		// Three or four seats — or a forced 2-seat adapter session — ride
		// the DMG-07, clocked by seat 0's core.
		Dmg07 fresh;
		fresh.hosting = true;
		fresh.local   = true;
		fresh.target  = static_cast<uint8_t>(count);
		g_hub = fresh;
	}
	Trace("session: split x%d%s", count,
	      g_ring.active ? " (Faceball ring)" :
	      g_hub.local   ? " (local DMG-07)" : " (direct cable)");

	// The PC tracer rides the CPU trace hook while the wire trace is on.
	if (TraceOn())
	{
		std::memset(g_pc_hits, 0, sizeof g_pc_hits);
		Cpu::SetTraceHook(SerialPcHook);
	}
}

void SerialSplitDetach()
{
	Cpu::SetTraceHook(nullptr);
	g_trace_seat = -1;
	if (g_hub.local) g_hub = Dmg07();
	g_ring      = FaceRing();
	g_splitlink = SplitLink();
	// The seat cores are freed right after this; the last one to step
	// may still be the active port, and everything null-checks it.
	g_active     = nullptr;
	g_active_mem = nullptr;
}

bool SerialSplitActive() { return g_splitlink.count > 0; }

void SerialTraceMsg(const char *msg)
{
	Trace("%s", msg);
}

bool SerialTraceEnabled() { return TraceOn(); }

// Stamp the trace file with the cartridge so a folder of logs reads by
// game. Renames an already-open file; spaces become underscores.
void SerialTraceSetRom(const char *label, bool from_path)
{
	// A file basename beats a header title: the seats' null-path loads
	// must not override the primary's real name.
	static bool have_path = false;
	if (have_path && !from_path) return;
	if (from_path) have_path = true;

	char clean[sizeof g_trace_rom];
	size_t n = 0;
	for (const char *c = label; c && *c && n + 1 < sizeof clean; ++c)
	{
		const char ch = *c;
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')
			clean[n++] = ch;
		else if (ch == ' ' && n && clean[n - 1] != '_')
			clean[n++] = '_';
	}
	clean[n] = 0;
	if (!n || !std::strcmp(clean, g_trace_rom)) return;
	std::strcpy(g_trace_rom, clean);

	if (!g_trace) return;
	fclose(g_trace);
	char fresh[sizeof g_trace_file];
	TraceFileName(fresh, sizeof fresh);
	if (std::rename(g_trace_file, fresh) == 0)
		std::strcpy(g_trace_file, fresh);
	g_trace = fopen(g_trace_file, "a");
	Trace("rom: %s", label);
}

void SerialSetTraceSeat(int seat)
{
	g_trace_seat = seat;
}

int SerialGetTraceSeat()
{
	return g_trace_seat;
}

bool SerialLinkTakeStatusChange(char *buf, size_t cap)
{
	if (!buf || cap == 0) return false;

	// A split session has no sockets; without this it reads as a cable
	// that came out. The split runner announces its own state.
	if (SerialSplitActive()) return false;

	if (g_hub.hosting)
	{
		// Hub news is arrivals and departures, not who drives: the
		// adapter always does.
		const int now = SerialLinkPlayerCount();
		if (now == g_hub_reported) return false;
		g_hub_reported = now;
		SerialLinkStatusText(buf, cap);
		return true;
	}

	if (!SerialLinkIsConnected())
	{
		// Say so once on the side that did not ask for it: the peer left, and
		// nothing else would tell this instance.
		if (g_direct.reported == 0) return false;
		g_direct.reported = 0;
		SerialLinkStatusText(buf, cap);
		return true;
	}
	const uint8_t st = LinkDisplayState();
	if (st == 0)
	{
		// The handshake itself is news; the driver suffix comes once a
		// byte has actually moved.
		if (g_direct.reported != 0) return false;
		g_direct.reported = 0xFF;
		SerialLinkStatusText(buf, cap);
		return true;
	}
	if (st == g_direct.reported) return false;

	// Only master/passive flapping is rate-limited. Reaching the link, and
	// the first real transfer, are news and go out at once.
	const int64_t now  = g_active ? g_active->real_cycles : 0;
	const bool    flap = (g_direct.reported == 1 || g_direct.reported == 2 ||
	                      g_direct.reported == 5) &&
	                     (st == 1 || st == 2 || st == 5);
	if (flap && now < g_direct.next_report) return false;

	g_direct.reported    = st;
	g_direct.next_report = now + kRoleReportInterval;
	SerialLinkStatusText(buf, cap);
	return true;
}

void SerialLinkStatusText(char *buf, size_t cap)
{
	if (!buf || cap == 0) return;

	if (SerialSplitActive())
	{
		std::snprintf(buf, cap, "Link cable: split screen - %d seats",
		              g_splitlink.count);
		return;
	}

	if (g_hub.hosting && LinkGetState() != LinkState::Off)
	{
		const int linked = SerialLinkPlayerCount();
		if (linked < g_hub.target)
			std::snprintf(buf, cap,
			              "Link cable: %u-player adapter - %d of %u instances linked",
			              static_cast<unsigned>(g_hub.target), linked,
			              static_cast<unsigned>(g_hub.target));
		else
			std::snprintf(buf, cap,
			              "Link cable: %u-player adapter - all instances linked",
			              static_cast<unsigned>(g_hub.target));
		return;
	}

	switch (LinkGetState())
	{
		case LinkState::Off:
			// LinkLastError stays available for diagnostics, but the reason a
			// cable came out is not what the player needs on screen.
			std::snprintf(buf, cap, "Link cable: disconnected");
			return;
		case LinkState::Listening:
			std::snprintf(buf, cap, "Link cable: waiting for the other instance");
			return;
		case LinkState::Connecting:
			std::snprintf(buf, cap, "Link cable: connecting...");
			return;
		case LinkState::Connected:
			// Socket up but no version packet: distinguish from a real link,
			// or a silent game looks like a broken connection.
			if (!g_sessions[0].handshaked)
			{
				std::snprintf(buf, cap, "Link cable: connected, waiting for handshake");
				return;
			}
			// No role claim until a byte has actually moved: the games choose
			// who drives, and clocking an empty wire proves nothing.
			{
				const uint8_t st = LinkDisplayState();
				const char *role = (st == 1) ? " (Master)"
				                 : (st == 2) ? " (Passive)"
				                 : (st == 5) ? " (Bidirectional)"
				                 : (st == 3) ? " - waiting for the other game" : "";
				std::snprintf(buf, cap, "Link cable: connected%s%s", role,
				              g_direct.stalled ? " - peer not responding" : "");
			}
			return;
	}
}

} // namespace SGB
