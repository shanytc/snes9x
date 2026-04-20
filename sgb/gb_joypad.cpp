/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_joypad.h"
#include "gb_memory.h"
#include "gb_cpu.h"

namespace SGB {

void JoypadReset(Joypad &j)
{
	j.select    = 0x30;
	j.dpad      = 0x0F;
	j.btns      = 0x0F;
	j.prev_mask = 0;
}

void JoypadSet(Joypad &j, Memory &mem, uint8_t mask)
{
	// Any bit in `mask` that was 0 last call and 1 now is a newly-pressed
	// button — the falling edge of that line on the matrix. Each such
	// edge raises the Joypad IRQ (and wakes from STOP via gb_cpu.cpp).
	const uint8_t newly_pressed = static_cast<uint8_t>(mask & ~j.prev_mask);
	j.prev_mask = mask;

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

	if (newly_pressed != 0)
		mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_JOYPAD);
}

uint8_t JoypadRead(const Joypad &j)
{
	uint8_t result = static_cast<uint8_t>(j.select | 0xC0);
	const bool sel_dpad = (j.select & 0x10) == 0;
	const bool sel_btns = (j.select & 0x20) == 0;
	uint8_t low = 0x0F;
	if (sel_dpad) low &= j.dpad;
	if (sel_btns) low &= j.btns;
	return static_cast<uint8_t>(result | low);
}

void JoypadWrite(Joypad &j, uint8_t value)
{
	j.select = static_cast<uint8_t>(value & 0x30);
}

} // namespace SGB
