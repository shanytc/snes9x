/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "biosmanager.h"

#include <cstdio>
#include <cstring>

// Sizes match the loaders: sfcbox.h SFCBOX_KROM_SIZE / SFCBOX_FONT_SIZE,
// bsx.cpp BIOS_SIZE, memmap.cpp's 0x40000 STBIOS read. 0 = don't care (the
// SGB carts ship in two sizes, the CGB boot ROM in two layouts).
static const S9xBiosSlotInfo kSlots[S9X_NUM_BIOS_SLOTS] =
{
	{ "GameBoy",       "Game Boy",          "dmg_boot.bin", 0x100    },
	{ "GameBoyColor",  "Game Boy Color",    "cgb_boot.bin", 0        },
	{ "SGB1",          "Super Game Boy",    "sgb.sfc",      0        },
	{ "SGB2",          "Super Game Boy 2",  "sgb2.sfc",     0        },
	{ "SFCBoxKROM",    "SFC Box (KROM)",    "KROM1.BIN",    0x10000  },
	{ "SFCBoxFont",    "SFC Box (MB90082)", "MB90082.BIN",  9216     },
	{ "BSX",           "Satellaview / BS-X","BS-X.bin",     0x100000 },
	{ "SufamiTurbo",   "Sufami Turbo",      "STBIOS.bin",   0x40000  },
};

static char g_paths[S9X_NUM_BIOS_SLOTS][S9X_BIOS_PATH_MAX];

static bool SlotValid (int slot)
{
	return slot >= 0 && slot < S9X_NUM_BIOS_SLOTS;
}

const S9xBiosSlotInfo *S9xGetBiosSlotInfo (int slot)
{
	return SlotValid(slot) ? &kSlots[slot] : NULL;
}

const char *S9xGetBiosPath (int slot)
{
	return SlotValid(slot) ? g_paths[slot] : "";
}

void S9xSetBiosPath (int slot, const char *path)
{
	if (!SlotValid(slot)) return;
	if (!path) path = "";
	strncpy(g_paths[slot], path, S9X_BIOS_PATH_MAX - 1);
	g_paths[slot][S9X_BIOS_PATH_MAX - 1] = '\0';
}

char *S9xGetBiosPathBuffer (int slot)
{
	return SlotValid(slot) ? g_paths[slot] : NULL;
}

// Byte count, or -1 when unreadable.
static long FileSize (const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return (-1);
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return (-1); }
	const long n = ftell(f);
	fclose(f);
	return (n);
}

bool8 S9xBiosPathUsable (int slot)
{
	if (!SlotValid(slot) || !g_paths[slot][0]) return (FALSE);

	const long n = FileSize(g_paths[slot]);
	if (n < 0) return (FALSE);

	// The CGB boot ROM ships with or without its cart-header window.
	if (slot == S9X_BIOS_GBC)
		return (n == 0x900 || n == 0x800) ? TRUE : FALSE;

	if (kSlots[slot].size && (uint32) n != kSlots[slot].size) return (FALSE);
	return (TRUE);
}

std::string S9xResolveBiosPath (int slot)
{
	if (!SlotValid(slot) || !g_paths[slot][0])  return (std::string());
	if (FileSize(g_paths[slot]) < 0)            return (std::string());
	return (std::string(g_paths[slot]));
}
