/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _BIOSMANAGER_H_
#define _BIOSMANAGER_H_

#include "port.h"
#include <string>

// User-assigned BIOS file paths, edited through File -> BIOS Manager. A slot
// with a path set is tried before the legacy by-name search in BIOS_DIR, so
// files can live anywhere under any name.
enum S9xBiosSlot
{
	S9X_BIOS_GB = 0,        // dmg_boot.bin — Game Boy boot ROM
	S9X_BIOS_GBC,           // cgb_boot.bin — Game Boy Color boot ROM
	S9X_BIOS_SGB1,          // sgb.sfc      — Super Game Boy
	S9X_BIOS_SGB2,          // sgb2.sfc     — Super Game Boy 2
	S9X_BIOS_SFCBOX_KROM,   // KROM1.BIN    — Super Famicom Box supervisor
	S9X_BIOS_SFCBOX_FONT,   // MB90082.BIN  — Super Famicom Box OSD font
	S9X_BIOS_BSX,           // BS-X.bin     — Satellaview
	S9X_BIOS_SUFAMI,        // STBIOS.bin   — Sufami Turbo
	S9X_NUM_BIOS_SLOTS
};

struct S9xBiosSlotInfo
{
	const char *key;       // config key under [BIOS]
	const char *label;     // dialog row label
	const char *filename;  // conventional name, shown as the hint
	uint32      size;      // expected byte count, 0 = any
};

#define S9X_BIOS_PATH_MAX 512

const S9xBiosSlotInfo *S9xGetBiosSlotInfo (int slot);

// Empty string when the slot is unassigned. Set with an empty path to clear.
const char *S9xGetBiosPath (int slot);
void        S9xSetBiosPath (int slot, const char *path);

// Raw buffer for config backends that bind a char array (win32 wconfig).
char *S9xGetBiosPathBuffer (int slot);

// FALSE when the slot is unassigned, the file is unreadable, or its size
// doesn't match. The dialog flags these; loaders still try the path and fall
// back on failure, so a wrong-sized file is a warning rather than a block.
bool8 S9xBiosPathUsable (int slot);

// Assigned path when it is readable, otherwise "". Loaders call this first and
// fall through to their own by-name search when it comes back empty.
std::string S9xResolveBiosPath (int slot);

#endif
