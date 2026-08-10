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

// Game Boy serial port (link cable) at 0xFF01/0xFF02.
//
// SB (0xFF01) and SC (0xFF02) themselves stay in Memory — they were
// already there and the save-state layout depends on it. This struct is
// only the transfer machinery: the shift-clock countdown, which side is
// driving it, and the bytes in flight to and from a linked peer.
//
// With no peer connected the port keeps its historical stub behaviour
// (internal-clock writes complete immediately reading back $FF, external
// clock never completes), so unlinked games behave exactly as before.
// Once a peer is connected the transfer is timed properly and the byte
// actually crosses the wire, via the BGB link protocol in gb_link.cpp.
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

	// A peer's transfer that landed before our game armed its own side.
	// Held for a short window so a small skew between the two emulators
	// doesn't turn into a dropped byte.
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

// Fires each time the CPU starts an internal-clock transfer (write $81 to
// 0xFF02); the byte is whatever SB held at that moment. The GB test
// harness captures Blargg's pass/fail text through it. nullptr disables.
using SerialByteCallback = void (*)(uint8_t byte);
void SetSerialCallback(SerialByteCallback cb);

void SerialReset(Serial &s, bool cgb);

// A save state captures one side of the cable but not the peer, so the
// transfer machinery is rebuilt rather than restored: in-flight bytes are
// dropped and, if SC still says a transfer is running, the countdown is
// re-armed so the game can't hang waiting on a bit 7 that never clears.
void SerialAfterStateLoad(Serial &s, Memory &mem);

// Bill T-cycles in the CPU clock domain — call it right next to TimerStep
// so double-speed doubles the shift clock the way hardware does.
void SerialStep(Serial &s, Memory &mem, int32_t tcycles);

// 0xFF01 / 0xFF02 access. SB writes are plain stores while no transfer is
// in flight; SC writes are what start one.
void    SerialWriteSC(Serial &s, Memory &mem, uint8_t value);
uint8_t SerialReadSC(const Serial &s, const Memory &mem);

// ---- Link session control (host UI facade) ---------------------------------
// Host side of the connection: listen on `port`, or connect out to
// `host:port`. Both return false and fill `err` when the socket can't be
// set up. Only one session exists at a time; starting a new one drops any
// existing link.
bool SerialLinkListen(uint16_t port, char *err, size_t err_cap);
bool SerialLinkConnect(const char *host, uint16_t port, char *err, size_t err_cap);
void SerialLinkDisconnect();

bool SerialLinkIsEnabled();     // a session is listening, connecting or connected
bool SerialLinkIsConnected();   // a peer is attached and past the version handshake

// Service the socket from outside the emulation loop. SerialStep normally
// does this, but a modal settings dialog blocks the emulation thread and
// an accept/connect would otherwise never complete while it is open.
void SerialLinkPump();

// One-line human-readable state for the UI / OSD, e.g.
// "Connected to 192.168.1.5:8765 (412 bytes)".
void SerialLinkStatusText(char *buf, size_t cap);

} // namespace SGB

#endif
