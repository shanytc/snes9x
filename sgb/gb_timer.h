/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_TIMER_H_
#define _SGB_GB_TIMER_H_

#include <cstdint>

namespace SGB {

struct Memory;

// DIV at 0xFF04 increments at 16384 Hz.
// TIMA at 0xFF05 increments at a rate selected by TAC bits 1:0:
//   00 = 4096 Hz, 01 = 262144 Hz, 10 = 65536 Hz, 11 = 16384 Hz.
// TIMA overflow reloads from TMA (0xFF06) and raises Timer IRQ.
struct Timer
{
	uint16_t div_counter = 0;   // full 16-bit internal; 0xFF04 exposes the upper byte
	uint8_t  tima = 0;
	uint8_t  tma  = 0;
	uint8_t  tac  = 0;
	bool     tima_overflow_pending = false;  // 4-T-cycle delay before TMA reload
};

void TimerReset(Timer &t);
void TimerStep(Timer &t, Memory &mem, int32_t tcycles);
uint8_t TimerRead(const Timer &t, uint16_t addr);
void    TimerWrite(Timer &t, uint16_t addr, uint8_t value);

} // namespace SGB

#endif
