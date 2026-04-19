/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_timer.h"
#include "gb_memory.h"

namespace SGB {

void TimerReset(Timer &t)
{
	t.div_counter = 0;
	t.tima = 0;
	t.tma  = 0;
	t.tac  = 0;
	t.tima_overflow_pending = false;
}

void TimerStep(Timer & /*t*/, Memory & /*mem*/, int32_t /*tcycles*/)
{
	// P5 wires up DIV/TIMA increments and overflow → IRQ.
}

uint8_t TimerRead(const Timer &t, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF04: return static_cast<uint8_t>(t.div_counter >> 8);
		case 0xFF05: return t.tima;
		case 0xFF06: return t.tma;
		case 0xFF07: return t.tac | 0xF8;
	}
	return 0xFF;
}

void TimerWrite(Timer &t, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF04: t.div_counter = 0; break;
		case 0xFF05: t.tima = value;    break;
		case 0xFF06: t.tma  = value;    break;
		case 0xFF07: t.tac  = value & 0x07; break;
	}
}

} // namespace SGB
