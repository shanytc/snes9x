/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Host stubs for the standalone sgb/ test binaries. audit.cpp resolves BIOS
// files through the host's BIOS Manager; the harness has no configured slots,
// so every lookup comes back empty and audit runs BIOS-less.

#include "../../biosmanager.h"

std::string S9xResolveBiosPath (int)
{
	return std::string();
}

bool8 S9xReadBiosImage (const char *, std::vector<uint8> &out, uint32,
                        S9xBiosAcceptFn, void *)
{
	out.clear();
	return FALSE;
}
