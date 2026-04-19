/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_mbc.h"

namespace SGB {

void MbcReset(MbcState &s)
{
	s.rom_bank   = 1;
	s.ram_bank   = 0;
	s.ram_enable = false;
	s.mbc1_mode  = false;
	s.rtc_latch  = false;
	for (int i = 0; i < 5; ++i) { s.rtc_regs[i] = 0; s.rtc_latched[i] = 0; }
}

uint8_t MbcRead(MbcState & /*s*/, const std::vector<uint8_t> & /*rom*/, const std::vector<uint8_t> & /*sram*/, uint16_t /*addr*/)
{
	// P2 implements per-MBC read handling.
	return 0xFF;
}

void MbcWrite(MbcState & /*s*/, std::vector<uint8_t> & /*sram*/, uint16_t /*addr*/, uint8_t /*value*/)
{
	// P2 implements per-MBC write handling.
}

} // namespace SGB
