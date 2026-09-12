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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#ifndef __WIN32__
#  include <dirent.h>
#  include <sys/stat.h>
#endif

// Filenames each loader's by-name search tries, in order, when its slot is
// blank. Kept here so the BIOS Manager can run the very same search.
static const char *const kNamesGB[] = {
	"dmg_boot.bin", "DMG_boot.bin", "dmg_bios.bin", "gb_bios.bin",
	"dmg.boot.rom", "dmg_boot.rom", "DMG_ROM.bin", NULL
};
static const char *const kNamesGBC[] = {
	"cgb_boot.bin", "CGB_boot.bin", "cgb_bios.bin", "gbc_bios.bin",
	"cgb.boot.rom", "cgb_boot.rom", "CGB_ROM.bin", NULL
};
static const char *const kNamesSGB1[] = {
	"sgb.sfc", "SGB.sfc", "sgb1.sfc", "SGB1.sfc",
	"Super Game Boy (World).sfc", NULL
};
static const char *const kNamesSGB2[] = {
	"sgb2.sfc", "SGB2.sfc", "Super Game Boy 2 (Japan).sfc", NULL
};
static const char *const kNamesSGB1Boot[] = {
	"sgb.boot.rom", "sgb1.boot.rom", "sgb_bios.bin", "sgb_boot.bin",
	"Super Game Boy SGB-CPU (World) (Enhancement Chip).bin", NULL
};
static const char *const kNamesSGB2Boot[] = {
	"sgb2.boot.rom", "sgb2_bios.bin", "sgb2_boot.bin",
	"Super Game Boy 2 SGB2-CPU (Japan) (Enhancement Chip).bin", NULL
};
static const char *const kNamesKROM[]   = { "KROM1.BIN", "KROM.BIN", "krom1.bin", NULL };
static const char *const kNamesFont[]   = { "MB90082.BIN", NULL };
static const char *const kNamesBSX[]    = { "BS-X.bin", "BS-X.bios", NULL };
static const char *const kNamesSufami[] = { "STBIOS.bin", NULL };

// Sizes match the loaders: sfcbox.h SFCBOX_KROM_SIZE / SFCBOX_FONT_SIZE,
// bsx.cpp BIOS_SIZE, memmap.cpp's 0x40000 STBIOS read. 0 = don't care (the
// SGB carts ship in two sizes, the CGB boot ROM in two layouts).
static const S9xBiosSlotInfo kSlots[S9X_NUM_BIOS_SLOTS] =
{
	{ "GameBoy",       "Game Boy",          kNamesGB,       0x100,   "Optional, adds the boot logo" },
	{ "GameBoyColor",  "Game Boy Color",    kNamesGBC,      0,       "Optional, adds boot logo and GB colors" },
	{ "SGB1",          "Super Game Boy",    kNamesSGB1,     0,       NULL },
	{ "SGB2",          "Super Game Boy 2",  kNamesSGB2,     0,       NULL },
	{ "SGB1BootROM",   "SGB boot ROM",      kNamesSGB1Boot, 0x100,   "Optional, built-in is used" },
	{ "SGB2BootROM",   "SGB2 boot ROM",     kNamesSGB2Boot, 0x100,   "Optional, built-in is used" },
	{ "SFCBoxKROM",    "SFC Box (KROM)",    kNamesKROM,     0x10000, NULL },
	{ "SFCBoxFont",    "SFC Box (MB90082)", kNamesFont,     9216,    NULL },
	{ "BSX",           "Satellaview / BS-X",kNamesBSX,      0x100000,NULL },
	{ "SufamiTurbo",   "Sufami Turbo",      kNamesSufami,   0x40000, NULL },
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

std::string S9xBiosPathsFingerprint (void)
{
	std::string out;
	for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
	{
		out += g_paths[slot];
		out += '\n';
	}
	return (out);
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
	KIND_SGB1_CART, KIND_SGB2_CART, KIND_BSX_BIOS, KIND_SUFAMI_BIOS
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
		case KIND_BSX_BIOS:  return ("Satellaview BIOS");
		case KIND_SUFAMI_BIOS: return ("Sufami Turbo BIOS");
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

	// The Satellaview and Sufami Turbo BIOSes carry their titles in the SNES
	// header and at the top of the image, the same tests their loaders run.
	if (full == 0x100000 && n >= 0x7FD5 &&
	    memcmp(d + 0x7FC0, "Satellaview BS-X     ", 21) == 0)
		return (KIND_BSX_BIOS);
	if (full == 0x40000 && n >= 0x1E &&
	    memcmp(d, "BANDAI SFC-ADX", 14) == 0 && memcmp(d + 0x10, "SFC-ADX BACKUP", 14) == 0)
		return (KIND_SUFAMI_BIOS);

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

// Slots whose image carries a signature. The SFC Box ROMs are known only by
// an exact byte count, so size is the whole test there, and the by-name
// search never matches them by content.
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
		case S9X_BIOS_BSX:       return (KIND_BSX_BIOS);
		case S9X_BIOS_SUFAMI:    return (KIND_SUFAMI_BIOS);
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

	// Every slot, the size-only SFC Box ones included, so a known kind of
	// file in the wrong slot is named; those two reject only on a positive
	// identification.
	const int want = ExpectedKind(slot);
	KindProbe          probe = { want, KIND_UNKNOWN };
	std::vector<uint8> img;
	if (!S9xReadBiosImage(g_paths[slot], img, 0x8000, AcceptKind, &probe))
	{
		// Say it is wrong, not just what it is, or the row reads as a caption
		// for whatever was dropped on it.
		if (detail)
			*detail = probe.seen != KIND_UNKNOWN
			       ? std::string("Wrong BIOS (") + KindName(probe.seen) + ") selected."
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
// The by-name search

// What sits directly inside `dir`: regular files and folders, each sorted so
// the search order is the same run to run and machine to machine.
static void ListDir (const std::string &dir, std::vector<std::string> &files,
                     std::vector<std::string> &subs)
{
	files.clear();
	subs.clear();
#ifdef __WIN32__
	WIN32_FIND_DATAA fd;
	HANDLE           h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
		((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? subs : files).push_back(fd.cFileName);
	}
	while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	while (struct dirent *e = readdir(d))
	{
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		struct stat st;
		if (stat((dir + SLASH_STR + e->d_name).c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode))      subs.push_back(e->d_name);
		else if (S_ISREG(st.st_mode)) files.push_back(e->d_name);
	}
	closedir(d);
#endif
	std::sort(files.begin(), files.end());
	std::sort(subs.begin(), subs.end());
}

std::vector<std::string> S9xBiosSearchDirs (void)
{
	// Breadth first, so a file nearer the root wins over a deeper namesake.
	std::vector<std::string> dirs(1, S9xGetDirectory(BIOS_DIR));
	std::vector<std::string> files, subs;
	size_t                   level_begin = 0;
	for (int depth = 0; depth < MAX_BIOS_DEEP_SEARCH; depth++)
	{
		const size_t level_end = dirs.size();
		for (size_t i = level_begin; i < level_end; i++)
		{
			ListDir(dirs[i], files, subs);
			for (size_t s = 0; s < subs.size(); s++)
				dirs.push_back(dirs[i] + SLASH_STR + subs[s]);
		}
		level_begin = level_end;
	}
	return (dirs);
}

struct Candidate
{
	std::string path;
	bool        by_content;   // found by scanning rather than by a listed name
};

// Every path the search for `slot` would open, in order. The listed names
// come first, as plain files and packed (dmg_boot.bin also as dmg_boot.zip
// and dmg_boot.bin.zip), across every folder in `dirs`, so a conventional
// name beats any other dump. Then, for a slot whose image carries a
// signature, every other file under the BIOS folder, so a dump under any
// name still counts once its contents say what it is.
static std::vector<Candidate> Candidates (int slot, const std::vector<std::string> &dirs)
{
	std::vector<Candidate> out;
	for (size_t d = 0; d < dirs.size(); d++)
		for (const char *const *n = kSlots[slot].names; *n; n++)
		{
			const std::string        name(*n);
			const size_t             dot = name.find_last_of('.');
			std::vector<std::string> forms(1, name);
			if (dot != std::string::npos && dot > 0) forms.push_back(name.substr(0, dot) + ".zip");
			forms.push_back(name + ".zip");
			for (size_t f = 0; f < forms.size(); f++)
			{
				const Candidate c = { dirs[d].empty() ? forms[f] : dirs[d] + SLASH_STR + forms[f], false };
				if (FileSize(c.path.c_str()) >= 0) out.push_back(c);
			}
		}

	if (ExpectedKind(slot) == KIND_UNKNOWN) return (out);
	const std::vector<std::string> tree = S9xBiosSearchDirs();
	std::vector<std::string>       files, subs;
	for (size_t d = 0; d < tree.size(); d++)
	{
		ListDir(tree[d], files, subs);
		for (size_t f = 0; f < files.size(); f++)
		{
			const Candidate c = { tree[d] + SLASH_STR + files[f], true };
			out.push_back(c);
		}
	}
	return (out);
}

// A file found by content gets the slot's signature test before the caller's
// own filter, which for a size-only slot would take any file of that length.
struct ContentGate { S9xBiosAcceptFn accept; void *ctx; int kind; };

static bool AcceptContent (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	const ContentGate *g = (const ContentGate *) ctx;
	if (ClassifyImage(data, size, full_size) != g->kind) return (false);
	return !g->accept || g->accept(data, size, full_size, g->ctx);
}

std::string S9xFindBiosByName (int slot, const std::vector<std::string> &dirs,
                               std::vector<uint8> &out, uint32 max_size,
                               S9xBiosAcceptFn accept, void *ctx)
{
	out.clear();
	if (!SlotValid(slot)) return (std::string());
	const std::vector<Candidate> cands = Candidates(slot, dirs);
	ContentGate                  gate  = { accept, ctx, ExpectedKind(slot) };
	for (size_t i = 0; i < cands.size(); i++)
	{
		const bool ok = cands[i].by_content
		              ? S9xReadBiosImage(cands[i].path.c_str(), out, max_size, AcceptContent, &gate)
		              : S9xReadBiosImage(cands[i].path.c_str(), out, max_size, accept, ctx);
		if (ok) return (cands[i].path);
	}
	out.clear();
	return (std::string());
}

std::string S9xFindBiosInBiosDir (int slot, std::string *detail)
{
	if (detail) detail->clear();
	if (!SlotValid(slot)) return (std::string());

	// Each candidate goes through the assigned-path check, which already asks
	// for the signature, so "found" means what "OK" does on a typed path.
	// Borrows the slot and puts it back.
	const std::string            root  = S9xGetDirectory(BIOS_DIR) + SLASH_STR;
	const std::vector<Candidate> cands = Candidates(slot, S9xBiosSearchDirs());
	char saved[S9X_BIOS_PATH_MAX];
	memcpy(saved, g_paths[slot], sizeof saved);
	std::string hit;
	for (size_t i = 0; i < cands.size() && hit.empty(); i++)
	{
		const std::string &p = cands[i].path;
		S9xSetBiosPath(slot, p.c_str());
		if (S9xCheckBiosPath(slot, detail) == S9X_BIOS_PATH_OK)
			hit = p.compare(0, root.size(), root) == 0 ? p.substr(root.size()) : p;
	}
	memcpy(g_paths[slot], saved, sizeof saved);
	if (hit.empty() && detail) detail->clear();
	return (hit);
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
