/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_JOYPAD_H_
#define _SGB_GB_JOYPAD_H_

#include <cstdint>

namespace SGB {

// GB joypad is a 4-bit active-low matrix at 0xFF00, selected by bits 4/5.
//   bit 4 = 0 selects direction keys (Right, Left, Up, Down)
//   bit 5 = 0 selects buttons          (A, B, Select, Start)
// SGB reuses this register for command-packet transmission from the SNES —
// handled in sgb_packet.cpp, not here.
struct Joypad
{
	uint8_t select = 0x30;   // upper nibble — P14/P15 select lines
	uint8_t dpad   = 0x0F;   // low nibble, active-low — 1 = released
	uint8_t btns   = 0x0F;
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
void JoypadSet(Joypad &j, uint8_t mask);  // mask uses GB_* flags; 1 = pressed
uint8_t JoypadRead(const Joypad &j);
void    JoypadWrite(Joypad &j, uint8_t value);

} // namespace SGB

#endif
