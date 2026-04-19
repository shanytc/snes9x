/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_MEMORY_H_
#define _SGB_GB_MEMORY_H_

#include <cstdint>

namespace SGB {

struct Cart;
struct Ppu;
struct Apu;
struct Timer;
struct Joypad;

// GB address space — flat 16-bit. All access goes through this one bus.
//   0x0000-0x3FFF  ROM bank 0
//   0x4000-0x7FFF  ROM bank N (MBC-switched)
//   0x8000-0x9FFF  VRAM
//   0xA000-0xBFFF  External RAM (cart)
//   0xC000-0xDFFF  WRAM
//   0xE000-0xFDFF  Echo RAM (mirror of C000-DDFF)
//   0xFE00-0xFE9F  OAM
//   0xFEA0-0xFEFF  Unusable
//   0xFF00-0xFF7F  I/O
//   0xFF80-0xFFFE  HRAM
//   0xFFFF         IE register
struct Memory
{
	Cart   *cart   = nullptr;
	Ppu    *ppu    = nullptr;
	Apu    *apu    = nullptr;
	Timer  *timer  = nullptr;
	Joypad *joypad = nullptr;

	uint8_t wram[0x2000];
	uint8_t hram[0x7F];
	uint8_t ie;     // 0xFFFF
	uint8_t if_;    // 0xFF0F
};

uint8_t MemRead(Memory &m, uint16_t addr);
void    MemWrite(Memory &m, uint16_t addr, uint8_t value);

// 16-bit helpers assume little-endian, two sequential 8-bit accesses.
uint16_t MemRead16(Memory &m, uint16_t addr);
void     MemWrite16(Memory &m, uint16_t addr, uint16_t value);

void MemReset(Memory &m);

} // namespace SGB

#endif
