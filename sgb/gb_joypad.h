/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_JOYPAD_H_
#define _SGB_GB_JOYPAD_H_

#include <cstdint>

namespace SGB {

struct Memory;

// GB joypad is a 4-bit active-low matrix at 0xFF00, selected by bits 4/5.
//   bit 4 = 0 selects direction keys (Right, Left, Up, Down)
//   bit 5 = 0 selects buttons          (A, B, Select, Start)
// SGB reuses this register for command-packet transmission from the SNES —
// handled in sgb_packet.cpp, not here.
struct Joypad
{
	uint8_t select    = 0x30;   // upper nibble — P14/P15 select lines
	uint8_t dpad      = 0x0F;   // low nibble, active-low — 1 = released
	uint8_t btns      = 0x0F;
	uint8_t prev_mask = 0;      // last GB_* mask — used for IRQ edge detection

	// ICD2 (SGB BIOS) joypad bridge. When sgb_active, JoypadRead pulls
	// the selected nibble from sgb_pads[sgb_index] instead of dpad/btns,
	// AND — critically — exposes ~sgb_index in the low nibble when the
	// GB selects neither line ($30 idle). That rotation byte is what
	// SGB-detect probes (DK Japan after MLT_REQ, etc.) read back to
	// confirm SGB hardware. Without it, the low nibble is always 0x0F
	// and the game silently falls back to "plain DMG" — no border, no
	// palette commands. sgb_pads[N] mirrors the byte the BIOS wrote to
	// $6004+N (active-low, low nibble = R/L/U/D, high nibble =
	// A/B/Sel/Start).
	bool    sgb_active = false;
	uint8_t sgb_index  = 0;
	uint8_t sgb_pads[4] = {0xFF, 0xFF, 0xFF, 0xFF};
};

enum : uint8_t
{
	GB_RIGHT  = 0x01,
	GB_LEFT   = 0x02,
	GB_UP     = 0x04,
	GB_DOWN   = 0x08,
	GB_A      = 0x10,
	GB_B      = 0x20,
	GB_SELECT = 0x40,
	GB_START  = 0x80
};

void JoypadReset(Joypad &j);

// Apply a new pressed-button mask. Raises IRQ_JOYPAD in mem.if_ for any
// button that transitioned released → pressed since the previous call.
void    JoypadSet(Joypad &j, Memory &mem, uint8_t mask);
uint8_t JoypadRead(const Joypad &j);
void    JoypadWrite(Joypad &j, uint8_t value);

} // namespace SGB

#endif
