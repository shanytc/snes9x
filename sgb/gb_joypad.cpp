/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_joypad.h"

namespace SGB {

void JoypadReset(Joypad &j)
{
	j.select = 0x30;
	j.dpad   = 0x0F;
	j.btns   = 0x0F;
}

void JoypadSet(Joypad &j, uint8_t mask)
{
	// Input mask is active-high pressed; GB register bits are active-low.
	uint8_t dpad = 0;
	if (mask & GB_RIGHT) dpad |= 0x01;
	if (mask & GB_LEFT)  dpad |= 0x02;
	if (mask & GB_UP)    dpad |= 0x04;
	if (mask & GB_DOWN)  dpad |= 0x08;

	uint8_t btns = 0;
	if (mask & GB_A)      btns |= 0x01;
	if (mask & GB_B)      btns |= 0x02;
	if (mask & GB_SELECT) btns |= 0x04;
	if (mask & GB_START)  btns |= 0x08;

	j.dpad = static_cast<uint8_t>(~dpad & 0x0F);
	j.btns = static_cast<uint8_t>(~btns & 0x0F);
}

uint8_t JoypadRead(const Joypad &j)
{
	uint8_t result = j.select | 0xC0;
	const bool sel_dpad = (j.select & 0x10) == 0;
	const bool sel_btns = (j.select & 0x20) == 0;
	uint8_t low = 0x0F;
	if (sel_dpad) low &= j.dpad;
	if (sel_btns) low &= j.btns;
	return result | low;
}

void JoypadWrite(Joypad &j, uint8_t value)
{
	j.select = value & 0x30;
}

} // namespace SGB
