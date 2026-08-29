/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "biosmanager.h"

#ifdef UNZIP_SUPPORT
#  ifdef SYSTEM_ZIP
#    include <minizip/unzip.h>
#  else
#    include "unzip/unzip.h"
#  endif
#endif

#include <cctype>
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

// Local-file-header magic, so a BIOS packed as .zip is spotted by content
// rather than by extension.
static bool IsZipFile (const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return (false);
	uint8 sig[4] = { 0 };
	const size_t n = fread(sig, 1, sizeof sig, f);
	fclose(f);
	return n == sizeof sig && sig[0] == 'P' && sig[1] == 'K' &&
	       sig[2] == 0x03 && sig[3] == 0x04;
}

// Extensions a BIOS image inside a .zip is expected to carry. When an archive
// holds at least one such member the others are ignored, which keeps a readme
// or a save file out of the running; an archive with none falls back to
// judging every member on its contents.
static bool HasBiosMemberExt (const char *name)
{
	static const char *exts[] = { ".bin", ".sfc", ".gb", ".gbc", NULL };
	const size_t len = strlen(name);
	for (int i = 0; exts[i]; i++)
	{
		const size_t l = strlen(exts[i]);
		if (len <= l) continue;
		const char *tail = name + len - l;
		size_t k = 0;
		while (k < l && tolower((unsigned char) tail[k]) == exts[i][k]) k++;
		if (k == l) return (true);
	}
	return (false);
}

// Image sizes this slot will take. The CGB boot ROM ships with or without its
// cart-header window; a 0 in kSlots means the slot doesn't care.
static bool SizeOkForSlot (int slot, uint32 n)
{
	if (n == 0) return (false);
	if (slot == S9X_BIOS_GBC) return (n == 0x900 || n == 0x800);
	return (kSlots[slot].size == 0 || n == kSlots[slot].size);
}

bool8 S9xBiosPathUsable (int slot)
{
	if (!SlotValid(slot) || !g_paths[slot][0]) return (FALSE);

	// Judge an archive off the central directory rather than inflating it —
	// the dialog re-checks on every keystroke.
	if (IsZipFile(g_paths[slot]))
	{
#ifdef UNZIP_SUPPORT
		unzFile file = unzOpen(g_paths[slot]);
		if (!file) return (FALSE);

		bool any_named = false, ok_named = false, ok_other = false;
		for (int pos = unzGoToFirstFile(file); pos == UNZ_OK; pos = unzGoToNextFile(file))
		{
			unz_file_info info;
			char          name[260] = { 0 };   // minizip doesn't terminate a full buffer
			if (unzGetCurrentFileInfo(file, &info, name, sizeof name - 1, NULL, 0, NULL, 0) != UNZ_OK)
				continue;

			const bool named = HasBiosMemberExt(name);
			if (named) any_named = true;
			if (SizeOkForSlot(slot, (uint32) info.uncompressed_size))
			{
				if (named) ok_named = true;
				else       ok_other = true;
			}
		}
		unzClose(file);
		return (any_named ? ok_named : ok_other) ? TRUE : FALSE;
#else
		return (FALSE);
#endif
	}

	const long n = FileSize(g_paths[slot]);
	if (n < 0) return (FALSE);
	return SizeOkForSlot(slot, (uint32) n) ? TRUE : FALSE;
}

std::string S9xResolveBiosPath (int slot)
{
	if (!SlotValid(slot) || !g_paths[slot][0])  return (std::string());
	if (FileSize(g_paths[slot]) < 0)            return (std::string());
	return (std::string(g_paths[slot]));
}

// ---------------------------------------------------------------------------
// Reading an image, from a plain file or out of a .zip

#ifdef UNZIP_SUPPORT
// Inflate every member (capped at max_size) and keep the largest one `accept`
// takes, preferring members named like a BIOS image so a readme or a second
// dump packed alongside doesn't win.
static bool8 ReadBiosZip (const char *path, std::vector<uint8> &out, uint32 max_size,
                          S9xBiosAcceptFn accept, void *ctx)
{
	unzFile file = unzOpen(path);
	if (!file) return (FALSE);

	std::vector<uint8> best, other, buf;
	for (int pos = unzGoToFirstFile(file); pos == UNZ_OK; pos = unzGoToNextFile(file))
	{
		unz_file_info info;
		char          name[260] = { 0 };   // minizip doesn't terminate a full buffer
		if (unzGetCurrentFileInfo(file, &info, name, sizeof name - 1, NULL, 0, NULL, 0) != UNZ_OK)
			continue;
		if (info.uncompressed_size == 0)   // directory entry or empty member
			continue;

		const uint32 want = (info.uncompressed_size < (uLong) max_size)
								? (uint32) info.uncompressed_size : max_size;
		if (unzOpenCurrentFile(file) != UNZ_OK)
			continue;
		buf.assign(want, 0);
		const int got = unzReadCurrentFile(file, buf.data(), want);
		unzCloseCurrentFile(file);
		if (got <= 0)
			continue;
		buf.resize((size_t) got);

		if (accept && !accept(buf.data(), (uint32) got, (uint32) info.uncompressed_size, ctx))
			continue;

		std::vector<uint8> &pick = HasBiosMemberExt(name) ? best : other;
		if (buf.size() > pick.size())
			pick.swap(buf);
	}
	unzClose(file);

	if (best.empty()) best.swap(other);
	if (best.empty()) return (FALSE);
	out.swap(best);
	return (TRUE);
}
#endif

bool8 S9xReadBiosImage (const char *path, std::vector<uint8> &out, uint32 max_size,
                        S9xBiosAcceptFn accept, void *ctx)
{
	out.clear();
	if (!path || !*path || !max_size) return (FALSE);

#ifdef UNZIP_SUPPORT
	if (IsZipFile(path))
		return ReadBiosZip(path, out, max_size, accept, ctx);
#else
	if (IsZipFile(path)) return (FALSE);
#endif

	const long fsz = FileSize(path);
	if (fsz <= 0) return (FALSE);
	const uint32 want = ((uint32) fsz < max_size) ? (uint32) fsz : max_size;

	FILE *f = fopen(path, "rb");
	if (!f) return (FALSE);
	std::vector<uint8> buf(want, 0);
	const size_t n = fread(buf.data(), 1, want, f);
	fclose(f);
	if (n == 0) return (FALSE);
	buf.resize(n);

	if (accept && !accept(buf.data(), (uint32) n, (uint32) fsz, ctx)) return (FALSE);
	out.swap(buf);
	return (TRUE);
}
