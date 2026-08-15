/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_SERIAL_H_
#define _SGB_GB_SERIAL_H_

#include <cstdint>
#include <cstddef>

namespace SGB {

struct Memory;

// Transfer machinery for the serial port at 0xFF01/0xFF02. SB and SC
// themselves stay in Memory, which the save-state layout depends on.
struct Serial
{
	bool     cgb = false;          // CGB carts can select the fast shift clock

	bool     active     = false;   // internal clock: we drive, 8 bits counting down
	int32_t  bit_timer  = 0;       // T-cycles until the next bit shifts
	int32_t  bit_period = 512;     // T-cycles per bit for the transfer in flight
	int32_t  bits_left  = 0;

	bool     passive    = false;   // external clock: armed, waiting to be clocked

	bool     peer_valid = false;   // peer answered our active transfer
	uint8_t  peer_data  = 0xFF;
	// True only when that answer carried a byte. An ack meaning "nothing
	// armed here" also releases the transfer, but nobody was listening.
	bool     peer_supplied = false;

	// Peer transfers that landed before our game armed, held in arrival
	// order: the emulators run in frame bursts, so several clocks can turn
	// up while our game had no cycles to re-arm, and the game then drains
	// them as fast as it answers. Each clock's sync1 i1 rides along and is
	// echoed in whatever answers it, so a DMG-07 host can match replies to
	// clocks by sequence number. Overflow acks the oldest away — a slow
	// game misses those bytes on hardware just the same.
	static constexpr int kPendingCap = 32;
	uint8_t  pending_data[kPendingCap] = {0};
	uint32_t pending_i1[kPendingCap]   = {0};
	uint8_t  pending_head = 0;
	uint8_t  pending_len  = 0;
	int32_t  pending_age  = 0;   // age of the oldest held clock

	// Held clocks complete no faster than hardware ever spaces them, or a
	// burst overwrites SB before the game's main loop has read it.
	int32_t  ext_gap_timer = 0;

	// BGB timestamps are 2 MiHz; this counts real-time GB T-cycles (4 MiHz),
	// so double-speed cycles are halved back into real time before use.
	int64_t  real_cycles   = 0;
	int32_t  ds_remainder  = 0;
	int32_t  ts_send_timer = 0;
	int32_t  poll_timer    = 0;
};

// Fires when the CPU starts an internal-clock transfer; the GB test
// harness reads Blargg's pass/fail text through it. nullptr disables.
using SerialByteCallback = void (*)(uint8_t byte);
void SetSerialCallback(SerialByteCallback cb);

void SerialReset(Serial &s, bool cgb);

// Rebuilt, not restored: a state captures one side of a cable whose peer
// never rewound. Re-arms SC so the game can't hang on bit 7.
void SerialAfterStateLoad(Serial &s, Memory &mem);

// Bill T-cycles in the CPU clock domain, next to TimerStep, so double
// speed doubles the shift clock the way hardware does.
void SerialStep(Serial &s, Memory &mem, int32_t tcycles);

// SC writes are what start a transfer; SB writes are plain stores.
void    SerialWriteSC(Serial &s, Memory &mem, uint8_t value);
uint8_t SerialReadSC(const Serial &s, const Memory &mem);

// ---- Link session control (host UI facade) ---------------------------------
enum class LinkRole : uint8_t
{
	None   = 0,
	Server = 1,   // first one up: holds the port, waits for the peer
	Client = 2    // second one up: found the port taken and dialled in
};

// Bring the cable up on loopback. At `players` == 2 the role is sensed:
// binding the port is the probe, and the kernel arbitrates it so there is
// only ever one host. At 3 or 4 this instance hosts an emulated DMG-07
// Four Player Adapter: it must win the bind, every Game Boy on the
// session (this one included) runs as an external-clock slave of the
// adapter, and the joining instances are ordinary clients that never
// know a hub is on the other end.
LinkRole SerialLinkAutoStart(uint16_t port, int players, char *err, size_t err_cap);

void SerialLinkDisconnect();

// Role of the live session, or None when the cable is unplugged.
LinkRole SerialLinkGetRole();

bool SerialLinkIsEnabled();     // a session is listening, connecting or connected
bool SerialLinkIsConnected();   // a peer is attached and past the version handshake

// Service the socket from outside the emulation loop, for when that loop
// is parked and an accept/connect would otherwise never complete.
void SerialLinkPump();

// Game Boys on the session including this one (0..4). On a direct cable
// one side leaving ends it for both; on a hub only that seat empties.
int SerialLinkPlayerCount();

// ---- Split screen (in-process multi-core link) ------------------------------
// Registers every core's serial port and bus, in player order, so the
// cable (2 seats) or the DMG-07 (3-4 seats, adapter clocked by seat 0's
// core) is resolved by direct exchange between the cores — no transport,
// no packets, no processes. Attach drops any socket session first.
void SerialSplitAttach(Serial *const serials[], Memory *const mems[], int count);
void SerialSplitDetach();
bool SerialSplitActive();

// One-line state for the OSD, e.g. "Link cable: connected (Master)". The
// suffix appears only once a byte has crossed.
void SerialLinkStatusText(char *buf, size_t cap);

// True once when the driving end changed and is worth re-announcing;
// fills `buf`. Rate-limited so alternating games can't flood the OSD.
bool SerialLinkTakeStatusChange(char *buf, size_t cap);

} // namespace SGB

#endif
