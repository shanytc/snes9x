/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_SGB_PACKET_H_
#define _SGB_SGB_PACKET_H_

#include <cstdint>
#include <cstring>

namespace SGB {

// All 26 standard SGB command IDs. Stored in the top 5 bits of the
// first byte of packet 0.
enum class SgbCmd : uint8_t
{
	PAL01    = 0x00,
	PAL23    = 0x01,
	PAL03    = 0x02,
	PAL12    = 0x03,
	ATTR_BLK = 0x04,
	ATTR_LIN = 0x05,
	ATTR_DIV = 0x06,
	ATTR_CHR = 0x07,
	SOUND    = 0x08,
	SOU_TRN  = 0x09,
	PAL_SET  = 0x0A,
	PAL_TRN  = 0x0B,
	ATRC_EN  = 0x0C,
	TEST_EN  = 0x0D,
	ICON_EN  = 0x0E,
	DATA_SND = 0x0F,
	DATA_TRN = 0x10,
	MLT_REQ  = 0x11,
	JUMP     = 0x12,
	CHR_TRN  = 0x13,
	PCT_TRN  = 0x14,
	ATTR_TRN = 0x15,
	ATTR_SET = 0x16,
	MASK_EN  = 0x17,
	OBJ_TRN  = 0x18
};

// Packet sniffer state. Fed one byte at a time from $FF00 writes via
// PacketFeed(); emits a decoded command through the installed callback
// after all chained packets for a single command have landed.
//
// Protocol summary:
//   Each packet = 128 bits = 16 bytes, sent LSB-first within each byte.
//   Packet 0 byte 0 holds the 5-bit command (bits 7:3) and the 3-bit
//   chained-packet count (bits 2:0). Counts of 0 or 1 mean single-packet;
//   2..7 chain more packets of 16 raw bytes each (up to 7 × 16 = 112
//   bytes total — CHR_TRN and similar big transfers need this).
//
//   Transitions observed on P14 (bit 4) and P15 (bit 5) of the value
//   written to 0xFF00:
//     both high → both low      RESET (start of a new packet)
//     both low  → any non-zero  RESET complete, next bits go on the wire
//     both high → P14 low       bit 0
//     both high → P15 low       bit 1
enum class PacketPhase : uint8_t
{
	Idle,       // waiting for a RESET pulse
	InReset,    // both select lines low — waiting for RESET to end
	Receiving   // collecting bits into packet_buf
};

struct PacketState
{
	PacketPhase phase           = PacketPhase::Idle;
	uint8_t     prev_joyser_val = 0x30;

	uint8_t     bit_count       = 0;   // 0..7 within packet_buf[byte_count]
	uint8_t     byte_count      = 0;   // 0..15 within current packet
	uint8_t     packet_buf[16]  = {0};

	uint8_t     cmd            = 0;    // command byte from packet 0
	uint8_t     total_pkts     = 0;    // packet count declared in packet 0
	uint8_t     pkts_received  = 0;    // how many full packets we've collected
	uint8_t     chain_buf[7 * 16] = {0};

	// Diagnostics.
	uint32_t    commands_received = 0;
	uint32_t    packets_received  = 0;
	uint32_t    last_cmd          = 0xFF;  // 0xFF = none yet
};

// Called when a complete multi-packet command has been assembled.
// `data` points into the state's chain_buf — copy if you need to
// outlive the call. `len` is total_pkts * 16.
using SgbCommandCallback = void (*)(uint8_t cmd, const uint8_t *data, uint32_t len);

void PacketReset(PacketState &ps);
void PacketFeed (PacketState &ps, uint8_t value);

// Install a process-global callback. P6b/c/d hook this to dispatch into
// the palette / border / sound-FX handlers. Pass nullptr to disable.
void SetSgbCommandCallback(SgbCommandCallback cb);

} // namespace SGB

#endif
