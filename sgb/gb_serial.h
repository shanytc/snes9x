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

	// A peer's transfer that landed before our game armed, held briefly so
	// skew between the two emulators doesn't drop a byte.
	bool     pending_valid = false;
	uint8_t  pending_data  = 0xFF;
	int32_t  pending_age   = 0;

	// BGB timestamps are 2 MiHz; this counts real-time GB T-cycles (4 MiHz),
	// so double-speed cycles are halved back into real time before use.
	int64_t  real_cycles   = 0;
	int32_t  ds_remainder  = 0;
	int32_t  ts_send_timer = 0;
	int32_t  poll_timer    = 0;

	bool     handshake_sent = false;   // our version packet is out on this connection
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

// Bring the cable up on loopback, sensing the role: binding the port is
// the probe, and the kernel arbitrates it so there is only ever one host.
LinkRole SerialLinkAutoStart(uint16_t port, char *err, size_t err_cap);

void SerialLinkDisconnect();

// Role of the live session, or None when the cable is unplugged.
LinkRole SerialLinkGetRole();

bool SerialLinkIsEnabled();     // a session is listening, connecting or connected
bool SerialLinkIsConnected();   // a peer is attached and past the version handshake

// Service the socket from outside the emulation loop, for when that loop
// is parked and an accept/connect would otherwise never complete.
void SerialLinkPump();

// Game Boys on the cable including this one (0/1/2). Governs what a
// disconnect means; >2 needs DMG-07 adapter emulation and a star topology.
int SerialLinkPlayerCount();

// One-line state for the OSD, e.g. "Link cable: connected (Master)". The
// suffix appears only once a byte has crossed.
void SerialLinkStatusText(char *buf, size_t cap);

// True once when the driving end changed and is worth re-announcing;
// fills `buf`. Rate-limited so alternating games can't flood the OSD.
bool SerialLinkTakeStatusChange(char *buf, size_t cap);

} // namespace SGB

#endif
