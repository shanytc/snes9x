/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Host stubs for the standalone sgb/ test binaries. The Super Game Boy Color
// compositor (BIOS mode only, never reached here) matches the SNES PPU's
// brightness LUT and per-line render state.

#include "../../snes9x.h"
#include "../../memmap.h"
#include "../../ppu.h"
#include "../../gfx.h"

uint8 mul_brightness[16][32] = {};

void S9xApplyColorAdjustments (uint8 &, uint8 &, uint8 &, int) {}

void S9xGetLineRenderState (int, uint8 &brightness, uint16 &backdrop)
{
	brightness = 0;
	backdrop   = 0;
}
