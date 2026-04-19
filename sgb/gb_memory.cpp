/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_memory.h"
#include "gb_cart.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_mbc.h"

#include <cstring>

namespace SGB {

void MemReset(Memory &m)
{
	std::memset(m.wram, 0, sizeof m.wram);
	std::memset(m.hram, 0, sizeof m.hram);
	m.ie  = 0;
	m.if_ = 0xE1;  // bits 5-7 are always set
}

uint8_t MemRead(Memory & /*m*/, uint16_t /*addr*/)
{
	// P2 implements the full address decoder.
	return 0xFF;
}

void MemWrite(Memory & /*m*/, uint16_t /*addr*/, uint8_t /*value*/)
{
	// P2 implements the full address decoder.
}

uint16_t MemRead16(Memory &m, uint16_t addr)
{
	uint16_t lo = MemRead(m, addr);
	uint16_t hi = MemRead(m, static_cast<uint16_t>(addr + 1));
	return static_cast<uint16_t>(lo | (hi << 8));
}

void MemWrite16(Memory &m, uint16_t addr, uint16_t value)
{
	MemWrite(m, addr,                              static_cast<uint8_t>(value & 0xFF));
	MemWrite(m, static_cast<uint16_t>(addr + 1),  static_cast<uint8_t>((value >> 8) & 0xFF));
}

} // namespace SGB
