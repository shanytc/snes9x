/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "biosmanager.h"
#include "memmap.h"

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
	{ "GameBoy",       "Game Boy",          "dmg_boot.bin", 0x100,   "optional, adds the boot logo" },
	{ "GameBoyColor",  "Game Boy Color",    "cgb_boot.bin", 0,       "optional, boot logo and mono colours" },
	{ "SGB1",          "Super Game Boy",    "sgb.sfc",      0,       NULL },
	{ "SGB2",          "Super Game Boy 2",  "sgb2.sfc",     0,       NULL },
	{ "SGB1BootROM",   "SGB boot ROM",      "sgb.boot.rom", 0x100,   "optional, built-in is used" },
	{ "SGB2BootROM",   "SGB2 boot ROM",     "sgb2.boot.rom",0x100,   "optional, built-in is used" },
	{ "SFCBoxKROM",    "SFC Box (KROM)",    "KROM1.BIN",    0x10000, NULL },
	{ "SFCBoxFont",    "SFC Box (MB90082)", "MB90082.BIN",  9216,    NULL },
	{ "BSX",           "Satellaview / BS-X","BS-X.bin",     0x100000,NULL },
	{ "SufamiTurbo",   "Sufami Turbo",      "STBIOS.bin",   0x40000, NULL },
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
	static const char *exts[] = { ".bin", ".rom", ".sfc", ".gb", ".gbc", NULL };
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

// The size half of the check, zip-aware. Kept separate so the status can say
// which test failed.
static bool8 SizeOkForPath (int slot)
{
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

// What an image is, judged from its own bytes, so a slot can say what the file
// turned out to be instead of only that it was wrong.
enum BiosImageKind
{
	KIND_UNKNOWN = 0,
	KIND_DMG_BOOT, KIND_CGB_BOOT, KIND_SGB1_BOOT, KIND_SGB2_BOOT,
	KIND_SGB1_CART, KIND_SGB2_CART
};

static const char *KindName (int kind)
{
	switch (kind)
	{
		case KIND_DMG_BOOT:  return ("Game Boy boot ROM");
		case KIND_CGB_BOOT:  return ("Game Boy Color boot ROM");
		case KIND_SGB1_BOOT: return ("Super Game Boy boot ROM");
		case KIND_SGB2_BOOT: return ("Super Game Boy 2 boot ROM");
		case KIND_SGB1_CART: return ("Super Game Boy image");
		case KIND_SGB2_CART: return ("Super Game Boy 2 image");
		default:             return ("unrecognised image");
	}
}

// A boot ROM opens LD SP,$FFFE; the SGB one then sets P1 to $30, and its $FD
// byte is the A it hands the cart — $01 on SGB1, $FF on SGB2.
static int ClassifyImage (const uint8 *d, uint32 n, uint32 full)
{
	uint8 sgb_mode = 0;
	if (S9xIsSGBBIOSImage(d, n, &sgb_mode))
		return (sgb_mode == 2) ? KIND_SGB2_CART : KIND_SGB1_CART;

	if (n >= 7 && d[0] == 0x31 && d[1] == 0xFE && d[2] == 0xFF)
	{
		if (full == 0x100 && n >= 0x100)
		{
			if (d[3] != 0x3E || d[4] != 0x30 || d[5] != 0xE0 || d[6] != 0x00)
				return (KIND_DMG_BOOT);
			return (d[0xFD] == 0xFF) ? KIND_SGB2_BOOT : KIND_SGB1_BOOT;
		}
		if (full == 0x800 || full == 0x900) return (KIND_CGB_BOOT);
	}
	return (KIND_UNKNOWN);
}

// Slots the loader identifies by content as well as size. The rest are known
// only by an exact byte count, so size is the whole test there.
static int ExpectedKind (int slot)
{
	switch (slot)
	{
		case S9X_BIOS_GB:        return (KIND_DMG_BOOT);
		case S9X_BIOS_GBC:       return (KIND_CGB_BOOT);
		case S9X_BIOS_SGB1:      return (KIND_SGB1_CART);
		case S9X_BIOS_SGB2:      return (KIND_SGB2_CART);
		case S9X_BIOS_SGB1_BOOT: return (KIND_SGB1_BOOT);
		case S9X_BIOS_SGB2_BOOT: return (KIND_SGB2_BOOT);
		default:                 return (KIND_UNKNOWN);
	}
}

// What tells one SGB dump from another: the SNES header version and
// destination. Every retail image says Japan; only the 1994 Europe beta
// carries a different destination code.
static std::string SgbRevision (const uint8 *img)
{
	static const char *dest[] = {
		"Japan", "USA", "Europe", "Scandinavia", "Finland", "Netherlands",
		"Spain", "Germany", "Italy", "China", "Indonesia", "Korea"
	};
	const unsigned ver = img[0x7FDB], d = img[0x7FD9];
	char buf[64];
	snprintf(buf, sizeof buf, "v1.%u %s", ver,
	         d < sizeof dest / sizeof dest[0] ? dest[d] : "region ?");
	return (std::string(buf));
}

struct KindProbe { int want; int seen; };

static bool AcceptKind (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	KindProbe *p = (KindProbe *) ctx;
	const int  k = ClassifyImage(data, size, full_size);
	if (p->seen == KIND_UNKNOWN) p->seen = k;
	return (k == p->want);
}

// "need 256 bytes" beats "unexpected size" when the fix is to find another dump.
static std::string SizeWanted (int slot)
{
	char buf[64];
	if (slot == S9X_BIOS_GBC) return ("wrong size: need 2048 or 2304 bytes");
	if (kSlots[slot].size == 0) return ("empty file");
	snprintf(buf, sizeof buf, "wrong size: need %u bytes", (unsigned) kSlots[slot].size);
	return (std::string(buf));
}

// The Super Game Boy slots hold a cart, not a fixed-size blob, so size alone
// says almost nothing: FindSGB_BIOS accepts an image only if it carries the
// right console's header, and a file that fails that would read as OK here
// while the menu entry stayed greyed with no explanation.
static bool AcceptSGBSlot (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	(void) full_size;
	uint8 got = 0;
	return size >= 0x8000 && S9xIsSGBBIOSImage(data, size, &got) &&
	       got == *(const uint8 *) ctx;
}

S9xBiosPathStatus S9xCheckBiosPath (int slot, std::string *detail)
{
	if (detail) detail->clear();
	if (!SlotValid(slot) || !g_paths[slot][0])
		return (S9X_BIOS_PATH_UNSET);
	if (FileSize(g_paths[slot]) < 0)
		return (S9X_BIOS_PATH_MISSING);
	if (!SizeOkForPath(slot))
	{
		if (detail) *detail = SizeWanted(slot);
		return (S9X_BIOS_PATH_BAD_SIZE);
	}

	// Every slot, size-only ones included: an SGB cart is exactly Sufami
	// Turbo's 262144 bytes. Only ever rejects on positive identification.
	const int want = ExpectedKind(slot);
	KindProbe          probe = { want, KIND_UNKNOWN };
	std::vector<uint8> img;
	if (!S9xReadBiosImage(g_paths[slot], img, 0x8000, AcceptKind, &probe))
	{
		// Say it is wrong, not just what it is, or the row reads as a caption
		// for whatever was dropped on it.
		if (detail)
			*detail = probe.seen != KIND_UNKNOWN
			       ? std::string("wrong file: ") + KindName(probe.seen)
			       : want != KIND_UNKNOWN
			       ? std::string("not a ") + KindName(want)
			       : std::string("unreadable");
		return (S9X_BIOS_PATH_BAD_IMAGE);
	}

	// Nine plausible Super Game Boy dumps look alike in a file picker, so
	// say which one this is once it has been accepted.
	if (detail && img.size() >= 0x8000 &&
	    (want == KIND_SGB1_CART || want == KIND_SGB2_CART))
		*detail = SgbRevision(img.data());

	return (S9X_BIOS_PATH_OK);
}

bool8 S9xBiosPathUsable (int slot)
{
	return (S9xCheckBiosPath(slot, NULL) == S9X_BIOS_PATH_OK) ? TRUE : FALSE;
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
