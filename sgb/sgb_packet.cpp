/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// SGB command-packet sniffer.
//
// The SGB cart snoops the GB's write pattern to $FF00 and reconstructs
// command bytes from the transitions of P14/P15. This file implements
// that state machine in a self-contained way — see sgb_packet.h for the
// protocol description.

#include "sgb_packet.h"

namespace SGB {

namespace {

SgbCommandCallback g_command_cb = nullptr;

// Packet assembly reset — called after a complete command is dispatched
// or when we re-enter Idle and want a clean slate for the next packet.
void ResetPacketBuf(PacketState &ps)
{
	ps.bit_count  = 0;
	ps.byte_count = 0;
	std::memset(ps.packet_buf, 0, sizeof ps.packet_buf);
}

// Collect one bit into the current byte (LSB-first). When 16 bytes land
// we finalize the packet — either stashing it as the first in a chain
// or appending to an in-flight chain, dispatching when complete.
void AppendBit(PacketState &ps, uint8_t bit)
{
	if (bit) ps.packet_buf[ps.byte_count] = static_cast<uint8_t>(ps.packet_buf[ps.byte_count] | (1u << ps.bit_count));

	if (++ps.bit_count < 8) return;
	ps.bit_count = 0;

	if (++ps.byte_count < 16) return;

	// Packet complete — move it into the chain.
	if (ps.pkts_received == 0)
	{
		ps.cmd        = static_cast<uint8_t>(ps.packet_buf[0] >> 3);
		ps.total_pkts = static_cast<uint8_t>(ps.packet_buf[0] & 0x07);
		if (ps.total_pkts == 0) ps.total_pkts = 1;
		std::memcpy(ps.chain_buf, ps.packet_buf, 16);
	}
	else
	{
		std::memcpy(ps.chain_buf + ps.pkts_received * 16, ps.packet_buf, 16);
	}

	++ps.pkts_received;
	++ps.packets_received;

	if (ps.pkts_received >= ps.total_pkts)
	{
		if (g_command_cb)
		{
			g_command_cb(ps.cmd, ps.chain_buf,
			             static_cast<uint32_t>(ps.total_pkts) * 16u);
		}
		++ps.commands_received;
		ps.last_cmd       = ps.cmd;
		ps.pkts_received  = 0;
		ps.total_pkts     = 0;
	}

	// Either way, go Idle waiting for the next RESET pulse.
	ResetPacketBuf(ps);
	ps.phase = PacketPhase::Idle;
}

} // anonymous

void PacketReset(PacketState &ps)
{
	ps = PacketState{};
	ps.prev_joyser_val = 0x30;
}

void PacketFeed(PacketState &ps, uint8_t value)
{
	const uint8_t cur  = static_cast<uint8_t>(value & 0x30);
	const uint8_t prev = static_cast<uint8_t>(ps.prev_joyser_val & 0x30);
	ps.prev_joyser_val = value;

	// Both lines going low is a RESET pulse — starts a fresh packet.
	// Mid-chain RESETs are legitimate (they delimit chained packets)
	// so we don't touch pkts_received here.
	if (cur == 0x00 && prev != 0x00)
	{
		ps.phase = PacketPhase::InReset;
		ResetPacketBuf(ps);
		return;
	}

	if (ps.phase == PacketPhase::InReset)
	{
		if (cur != 0x00)
			ps.phase = PacketPhase::Receiving;
		return;
	}

	if (ps.phase != PacketPhase::Receiving) return;

	// Bit encoding: from both-high (0x30) we watch which line drops.
	//   P14 falls (0x30 → 0x20) = bit 0
	//   P15 falls (0x30 → 0x10) = bit 1
	// The "back to 0x30" edge between bits is ignored; we don't need it.
	if (prev == 0x30 && cur == 0x20)      AppendBit(ps, 0);
	else if (prev == 0x30 && cur == 0x10) AppendBit(ps, 1);
}

void SetSgbCommandCallback(SgbCommandCallback cb)
{
	g_command_cb = cb;
}

} // namespace SGB
