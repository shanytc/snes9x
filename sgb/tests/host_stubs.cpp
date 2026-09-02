/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Host stubs for the standalone sgb/ test binaries. audit.cpp resolves BIOS
// files through the host's BIOS Manager; the harness has no configured slots,
// so every lookup comes back empty and audit runs BIOS-less.

#include "../../biosmanager.h"
#include "../../snes9x.h"
#include "../../memmap.h"
#include "../../ppu.h"
#include "../../gfx.h"

std::string S9xResolveBiosPath (int)
{
	return std::string();
}

// The Super Game Boy Color compositor (BIOS mode only, never reached here)
// matches the SNES PPU's brightness LUT and per-line render state.
uint8 mul_brightness[16][32] = {};

void S9xApplyColorAdjustments (uint8 &, uint8 &, uint8 &, int) {}

void S9xGetLineRenderState (int, uint8 &brightness, uint16 &backdrop)
{
	brightness = 0;
	backdrop   = 0;
}

bool8 S9xReadBiosImage (const char *, std::vector<uint8> &out, uint32,
                        S9xBiosAcceptFn, void *)
{
	out.clear();
	return FALSE;
}
