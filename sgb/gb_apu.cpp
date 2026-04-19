/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_apu.h"

#include <cstring>

namespace SGB {

void ApuReset(Apu &a)
{
	std::memset(&a, 0, sizeof a);
	a.ch4.lfsr = 0x7FFF;
}

void ApuStep(Apu & /*a*/, int32_t /*tcycles*/)
{
	// P4 wires up the 4 channels and frame sequencer.
}

uint8_t ApuRead(Apu & /*a*/, uint16_t /*addr*/)
{
	return 0xFF;
}

void ApuWrite(Apu & /*a*/, uint16_t /*addr*/, uint8_t /*value*/)
{
}

int32_t ApuDrain(Apu & /*a*/, int16_t * /*out*/, int32_t /*max_samples*/)
{
	return 0;
}

} // namespace SGB
