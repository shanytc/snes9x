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
// With no peer the port keeps its original stub behaviour, so unlinked
// games are bit-for-bit unaffected.

#include "gb_serial.h"
#include "gb_link.h"
#include "gb_memory.h"
#include "gb_cpu.h"

#include <cstdio>
#include <cstring>

namespace SGB {

namespace {

// BGB command bytes.
constexpr uint8_t kCmdVersion        = 1;
constexpr uint8_t kCmdJoypad         = 101;
constexpr uint8_t kCmdSync1          = 104;
constexpr uint8_t kCmdSync2          = 105;
constexpr uint8_t kCmdSync3          = 106;
constexpr uint8_t kCmdStatus         = 108;
constexpr uint8_t kCmdWantDisconnect = 109;

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

// How often to volunteer our timestamp so the peer can pace itself (~1 ms).
constexpr int32_t kTimestampInterval = 4096;

// How long a peer's transfer waits for our game to arm (~1 ms), covering
// the skew between two free-running emulators.
constexpr int32_t kPendingHoldCycles = 4096;

// Ceiling on the master's stall waiting for a reply - bounded so a frozen
// peer can't take this instance down with it.
constexpr int kMasterWaitMs = 500;

// After one timeout the peer is presumed paused: drop to a token wait, or
// every byte would cost the full ceiling.
constexpr int kStalledWaitMs = 20;
constexpr int kWaitSliceMs   = 5;

// The Serial being stepped and its bus, so the UI can pump the socket
// while the emulation thread is parked. Only ever one GB core.
Serial *g_active     = nullptr;
Memory *g_active_mem = nullptr;

// Peer session state. Never serialized: it belongs to the connection.
struct LinkSession
{
	bool     handshaked        = false;
	bool     peer_running      = false;
	bool     peer_reconnect_ok = false;
	uint32_t peer_timestamp    = 0;
	uint32_t transfers         = 0;
	bool     stalled           = false;   // a master wait timed out

	// Who drove the last transfer, chosen per byte by the games via SC
	// bit 0: 0 = nothing yet, 1 = us, 2 = the peer.
	uint8_t  driving           = 0;
	uint32_t unanswered        = 0;   // polls we clocked that nobody answered
	uint8_t  reported          = 0;   // last value handed to the host UI
	int64_t  next_report       = 0;   // real-cycle gate on re-announcing
};

// Games alternating master/slave per byte would repaint the OSD forever;
// ~3 s of GB time between announcements keeps it readable.
constexpr int64_t kRoleReportInterval = 3 * 4194304;

LinkSession g_session;
LinkRole    g_role = LinkRole::None;

SerialByteCallback g_serial_cb = nullptr;

inline int32_t BitPeriod(const Serial &s, uint8_t sc)
{
	return (s.cgb && (sc & 0x02)) ? kBitPeriodFast : kBitPeriodNormal;
}

inline uint32_t Timestamp(const Serial &s)
{
	// 2 MiHz clocks, low 31 bits — the high bit is always 0 per the protocol.
	return static_cast<uint32_t>((s.real_cycles >> 1) & 0x7FFFFFFF);
}

void SendPacket(uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint32_t i1)
{
	LinkPacket p;
	p.b1 = b1;
	p.b2 = b2;
	p.b3 = b3;
	p.b4 = b4;
	p.i1 = i1;
	LinkSend(p);
}

void SendVersion() { SendPacket(kCmdVersion, 1, 4, 0, 0); }

void SendStatus()
{
	SendPacket(kCmdStatus,
	           static_cast<uint8_t>(kStatusRunning | kStatusSupportReconnect),
	           0, 0, 0);
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
	if (s.peer_supplied)
	{
		++g_session.transfers;
		g_session.driving    = 1;
		g_session.unanswered = 0;
	}
	else if (s.peer_valid)
	{
		// We drove the clock but the other game was not listening.
		++g_session.unanswered;
	}
	s.peer_valid    = false;
	s.peer_supplied = false;
	s.peer_data     = 0xFF;
}

// Finish a transfer the peer clocked for us.
void CompletePassive(Serial &s, Memory &mem, uint8_t data)
{
	mem.serial_data    = data;
	mem.serial_control = static_cast<uint8_t>(mem.serial_control & 0x7F);
	mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);

	s.passive = false;
	++g_session.transfers;
	g_session.driving    = 2;
	g_session.unanswered = 0;
}

void HandlePacket(Serial &s, Memory &mem, const LinkPacket &p)
{
	switch (p.b1)
	{
		case kCmdVersion:
			// A mismatched protocol reads as bogus transfers, which can
			// corrupt a trade. Drop instead.
			if (p.b2 != 1 || p.b3 != 4 || p.b4 != 0)
			{
				LinkStop();
				g_session.handshaked = false;
				return;
			}
			g_session.handshaked = true;
			SendStatus();
			return;

		case kCmdStatus:
			g_session.peer_running      = (p.b2 & kStatusRunning) != 0 &&
			                              (p.b2 & kStatusPaused) == 0;
			g_session.peer_reconnect_ok = (p.b2 & kStatusSupportReconnect) != 0;
			// Must not be answered with another status, or the two sides
			// ping-pong forever.
			return;

		case kCmdSync1:
			if (s.passive)
			{
				// Armed and waiting to be clocked — hand back our byte.
				SendPacket(kCmdSync2, mem.serial_data, 0x80, 0, 0);
				CompletePassive(s, mem, p.b2);
			}
			else if (s.active)
			{
				// Both ends clocking: nothing to give, so ack. Hardware
				// produces garbage here too.
				SendPacket(kCmdSync3, 1, 0, 0, 0);
			}
			else
			{
				// Peer got here first; hold briefly, since our game very
				// likely arms within a few hundred cycles.
				s.pending_valid = true;
				s.pending_data  = p.b2;
				s.pending_age   = 0;
			}
			return;

		case kCmdSync2:
			// Only while we are actually clocking: a reply that arrives after
			// we gave up would otherwise be shifted into the next transfer.
			if (!s.active) return;
			s.peer_data     = p.b2;
			s.peer_valid    = true;
			s.peer_supplied = true;
			return;

		case kCmdSync3:
			if (p.b2 == 1)
			{
				// Peer had nothing armed, so the wire stayed high. Releases
				// our transfer but is not an exchange with anyone.
				if (!s.active) return;
				s.peer_data     = 0xFF;
				s.peer_valid    = true;
				s.peer_supplied = false;
			}
			else
			{
				g_session.peer_timestamp = p.i1;
			}
			return;

		case kCmdJoypad:
		case kCmdWantDisconnect:
			// Remote control is out of scope, and we never auto-reconnect.
			return;

		default:
			return;
	}
}

// Drain everything the transport has buffered, and drive the connection
// state machine (accept / connect completion, version handshake).
void ServiceLink(Serial &s, Memory &mem)
{
	const LinkState state = LinkPoll();

	if (state != LinkState::Connected)
	{
		if (g_session.handshaked)
		{
			// Release any waiting transfer, or the game hangs on SC bit 7.
			g_session.handshaked = false;
			s.pending_valid      = false;
			if (s.active) { s.peer_valid = false; CompleteActive(s, mem); }
			s.passive = false;
		}
		// The next arrival needs a fresh version packet.
		s.handshake_sent = false;
		return;
	}

	if (!s.handshake_sent)
	{
		// Both sides open with a version packet and verify the reply.
		SendVersion();
		s.handshake_sent    = true;
		g_session.transfers = 0;
		g_session.stalled   = false;
	}

	LinkPacket p;
	while (LinkRecv(p)) HandlePacket(s, mem, p);
}

// Master side: the eight bit periods elapsed but the peer's byte has not
// landed yet. Give it a bounded window, servicing the socket throughout.
void WaitForPeer(Serial &s, Memory &mem)
{
	const int budget = g_session.stalled ? kStalledWaitMs : kMasterWaitMs;

	int waited = 0;
	while (!s.peer_valid && waited < budget)
	{
		if (!LinkIsConnected()) return;
		LinkWaitReadable(kWaitSliceMs);
		waited += kWaitSliceMs;
		ServiceLink(s, mem);
	}
	g_session.stalled = !s.peer_valid;
}

} // anonymous

void SetSerialCallback(SerialByteCallback cb) { g_serial_cb = cb; }

void SerialReset(Serial &s, bool cgb)
{
	// A reset does not unplug the cable, so only the transfer machinery is
	// rebuilt; handshake_sent belongs to the connection, not the console.
	const bool still_linked = s.handshake_sent && LinkIsConnected();

	const Serial fresh;
	s                = fresh;
	s.cgb            = cgb;
	s.handshake_sent = still_linked;

	g_active          = &s;
	g_session.stalled = false;
}

void SerialAfterStateLoad(Serial &s, Memory &mem)
{
	g_active     = &s;
	g_active_mem = &mem;

	s.active        = false;
	s.passive       = false;
	s.bits_left     = 0;
	s.bit_timer     = 0;
	s.peer_valid    = false;
	s.peer_supplied = false;
	s.peer_data     = 0xFF;
	s.pending_valid = false;
	s.pending_age   = 0;

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

void SerialWriteSC(Serial &s, Memory &mem, uint8_t value)
{
	mem.serial_control = value;

	if (!LinkIsConnected())
	{
		// Unlinked stub path: internal clock completes at once, external
		// leaves bit 7 set, which is how games spot a missing cable.
		s.active = s.passive = false;
		if ((value & 0x81) == 0x81)
		{
			if (g_serial_cb) g_serial_cb(mem.serial_data);
			mem.serial_data    = 0xFF;
			mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);
			mem.serial_control = static_cast<uint8_t>(value & 0x7F);
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

		if (s.pending_valid)
		{
			// The peer is clocking too; ack its byte rather than swallow it.
			s.pending_valid = false;
			SendPacket(kCmdSync3, 1, 0, 0, 0);
		}

		uint8_t control = 0x81;
		if (value & 0x02)     control = static_cast<uint8_t>(control | 0x02);
		if (mem.double_speed) control = static_cast<uint8_t>(control | 0x04);
		SendPacket(kCmdSync1, mem.serial_data, control, 0, Timestamp(s));
		return;
	}

	// External clock: arm and wait to be clocked by the peer.
	s.active  = false;
	s.passive = true;

	if (s.pending_valid)
	{
		// The peer's byte was already here — settle it immediately.
		const uint8_t data = s.pending_data;
		s.pending_valid    = false;
		SendPacket(kCmdSync2, mem.serial_data, 0x80, 0, 0);
		CompletePassive(s, mem, data);
	}
}

void SerialStep(Serial &s, Memory &mem, int32_t tcycles)
{
	if (tcycles <= 0) return;
	g_active     = &s;
	g_active_mem = &mem;

	// Timestamps are real time, so undo the double-speed doubling that
	// the CPU-domain cycle count carries.
	if (mem.double_speed)
	{
		s.ds_remainder += tcycles;
		s.real_cycles  += s.ds_remainder >> 1;
		s.ds_remainder &= 1;
	}
	else
	{
		s.real_cycles += tcycles;
	}

	if (!SerialLinkIsEnabled()) return;

	s.poll_timer -= tcycles;
	if (s.poll_timer <= 0)
	{
		s.poll_timer = kPollInterval;
		ServiceLink(s, mem);
	}

	if (!LinkIsConnected()) return;

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
			if (!s.peer_valid)
			{
				ServiceLink(s, mem);
				if (!s.peer_valid) WaitForPeer(s, mem);
			}
			CompleteActive(s, mem);
		}
	}

	if (s.pending_valid)
	{
		s.pending_age += tcycles;
		if (s.pending_age >= kPendingHoldCycles)
		{
			// Our game never armed its side; tell the peer so it stops waiting.
			s.pending_valid = false;
			SendPacket(kCmdSync3, 1, 0, 0, 0);
		}
	}

	s.ts_send_timer -= tcycles;
	if (s.ts_send_timer <= 0)
	{
		s.ts_send_timer = kTimestampInterval;
		SendPacket(kCmdSync3, 0, 0, 0, Timestamp(s));
	}
}

// ---- Link session control --------------------------------------------------

LinkRole SerialLinkAutoStart(uint16_t port, char *err, size_t err_cap)
{
	SerialLinkDisconnect();

	// Listener first: winning the bind means we are the first instance up,
	// losing it means the other one is already waiting.
	if (LinkStartServer(port, err, err_cap))
	{
		g_role = LinkRole::Server;
		return g_role;
	}

	if (LinkStartClient("127.0.0.1", port, err, err_cap))
	{
		g_role = LinkRole::Client;
		return g_role;
	}

	// Port was taken but nothing answered: the other instance was on its
	// way out, so take it over rather than failing.
	if (LinkStartServer(port, err, err_cap))
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
	if (LinkIsConnected() && g_session.peer_reconnect_ok)
		SendPacket(kCmdWantDisconnect, 0, 0, 0, 0);

	LinkStop();
	g_session = LinkSession();
	g_role    = LinkRole::None;

	if (g_active)
	{
		g_active->handshake_sent = false;
		g_active->pending_valid  = false;
		g_active->peer_valid     = false;
		g_active->active         = false;
		g_active->passive        = false;
	}
}

bool SerialLinkIsEnabled()   { return LinkGetState() != LinkState::Off; }
bool SerialLinkIsConnected() { return LinkIsConnected() && g_session.handshaked; }

void SerialLinkPump()
{
	if (g_active && g_active_mem) ServiceLink(*g_active, *g_active_mem);
	else                          LinkPoll();
}

// What the OSD is currently saying: 0 nothing yet, 1 we drive, 2 the peer
// drives, 3 we drive but the other game never answers.
uint8_t LinkDisplayState()
{
	if (g_session.driving) return g_session.driving;
	return g_session.unanswered ? 3 : 0;
}

int SerialLinkPlayerCount()
{
	if (SerialLinkIsConnected()) return 2;
	return SerialLinkIsEnabled() ? 1 : 0;
}

bool SerialLinkTakeStatusChange(char *buf, size_t cap)
{
	if (!buf || cap == 0) return false;

	if (!SerialLinkIsConnected())
	{
		g_session.reported = 0;
		return false;
	}
	const uint8_t st = LinkDisplayState();
	if (st == 0 || st == g_session.reported) return false;

	// Rate-limit re-announcements; the first is always let through.
	const int64_t now = g_active ? g_active->real_cycles : 0;
	if (g_session.reported != 0 && now < g_session.next_report) return false;

	g_session.reported    = st;
	g_session.next_report = now + kRoleReportInterval;
	SerialLinkStatusText(buf, cap);
	return true;
}

void SerialLinkStatusText(char *buf, size_t cap)
{
	if (!buf || cap == 0) return;

	switch (LinkGetState())
	{
		case LinkState::Off:
		{
			const char *err = LinkLastError();
			if (err && *err) std::snprintf(buf, cap, "Link cable: %s", err);
			else             std::snprintf(buf, cap, "Link cable: disconnected");
			return;
		}
		case LinkState::Listening:
			std::snprintf(buf, cap, "Link cable: waiting for the other instance");
			return;
		case LinkState::Connecting:
			std::snprintf(buf, cap, "Link cable: connecting...");
			return;
		case LinkState::Connected:
			// Socket up but no version packet: distinguish from a real link,
			// or a silent game looks like a broken connection.
			if (!g_session.handshaked)
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
				                 : (st == 3) ? " - waiting for the other game" : "";
				std::snprintf(buf, cap, "Link cable: connected%s%s", role,
				              g_session.stalled ? " - peer not responding" : "");
			}
			return;
	}
}

} // namespace SGB
