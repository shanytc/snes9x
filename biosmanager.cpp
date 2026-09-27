/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "biosmanager.h"
#include "memmap.h"
#include "superdisc.h"
#include "upd7725.h"
#include "hg51b.h"
#include "xband.h"

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

// Conventional filenames per slot: [0] is the dialog's placeholder, and the
// libretro port seeds a blank slot from them in its system directory.
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
static const char *const kNamesKROM[]   = { "KROM1.BIN", "KROM.BIN", "krom1.bin", "sfcbox.zip", "krom1.zip",
                                            "krom2.00.ic1", "krom1.ic1", NULL };
static const char *const kNamesFont[]   = { "MB90082.BIN", NULL };
static const char *const kNamesBSX[]    = { "BS-X.bin", "BS-X.bios", NULL };
static const char *const kNamesSufami[] = { "STBIOS.bin", NULL };
static const char *const kNamesNSS[]    = { "nss-ic14.02.ic14", "nss.zip", "nss-c.ic14",
                                            "nss-v3.ic14", "NSS-v03a.bin", NULL };
static const char *const kNamesNSSFont[]= { "m50458_char.bin", "m50458.zip", "m50458-001sp", NULL };
static const char *const kNamesSuperDisc[] = { "SDBR_v0.95.sfc", "SDBR_v0.95_unheadered.sfc",
                                               "Super Disc System Cartridge (Prototype).zip", NULL };
static const char *const kNamesDSP1[]  = { "dsp1.bin", "dsp1.rom", "DSP1 (World) (Enhancement Chip).bin", NULL };
static const char *const kNamesDSP1B[] = { "dsp1b.bin", "dsp1b.rom", "DSP1 B (World) (Enhancement Chip).bin", NULL };
static const char *const kNamesDSP2[]  = { "dsp2.bin", "dsp2.rom", "DSP2 (World) (Enhancement Chip).bin", NULL };
static const char *const kNamesDSP3[]  = { "dsp3.bin", "dsp3.rom", "DSP3 (Japan) (Enhancement Chip).bin", NULL };
static const char *const kNamesDSP4[]  = { "dsp4.bin", "dsp4.rom", "DSP4 (World) (Enhancement Chip).bin", NULL };
static const char *const kNamesCX4[]   = { "cx4.bin", "cx4.data.rom", "CX4 (World) (Enhancement Chip).bin", NULL };
static const char *const kNamesXBand[] = { "X-Band Modem BIOS (USA).sfc", "X-Band Modem BIOS (USA).zip",
                                           "X-Band Modem BIOS (U).smc", "XBand Modem BIOS (U).smc", NULL };

// Behind each row's info icon: a heading, then one "name — detail — CRC32" line
// per file (the dialogs' table); No-Intro dumps follow (S9xBiosSlotInfoText).
// CRC32s are No-Intro's or MAME's; a generic name's is the dump in the BIOS folder
// it was checked against, and a .zip's is the whole archive's (as distributed).
// A "(built-in)" row is the copy sgb.cpp runs when the slot is empty.
static const char kInfoGB[] = "Supports the following Game Boy Boot ROMs:\ndmg_boot.bin — 256 bytes — 59C8598E";
static const char kInfoGBC[] = "Supports the following Game Boy Color Boot ROMs:\ncgb_boot.bin — 2304 bytes (SameBoy) — 1D67E99E";
static const char kInfoSGB1[] = "Supports the following Super Game Boy cartridge ROMs:\nsgb.sfc — v1.2 — 8A4A174F";
static const char kInfoSGB2[] = "Supports the following Super Game Boy 2 cartridge ROMs:\nsgb2.sfc — v1.16 — CB176E45";
static const char kInfoSGB1Boot[] = "Supports the following Super Game Boy SGB-CPU boot ROMs:\n"
                                    "(built-in) — 256 bytes (SameBoy) — 6AF31430\n"
                                    "sgb.boot.rom — 256 bytes — EC8A83B9\n"
                                    "Super Game Boy SGB-CPU (World) (Enhancement Chip).bin — 256 bytes — EC8A83B9";
static const char kInfoSGB2Boot[] = "Supports the following Super Game Boy 2 SGB2-CPU boot ROMs:\n"
                                    "(built-in) — 256 bytes (SameBoy) — F4E2EAE2\n"
                                    "sgb2.boot.rom — 256 bytes — 53D0DD63\n"
                                    "Super Game Boy 2 SGB2-CPU (Japan) (Enhancement Chip).bin — 256 bytes — 53D0DD63";
static const char kInfoKROM[] = "Supports the following Super Famicom Box KROM releases:\n"
                                "KROM1.BIN — v1.00 — C9010002\n"
                                "krom1.zip — v1.00 — E52F16F9\n"
                                "krom2.00.ic1 — v2.00 — E31B5580\n"
                                "sfcbox.zip — v1.00 + v2.00 (MAME set) — 2DD4E948";
static const char kInfoFont[] = "Supports the following MB90082 OSD character ROMs (MAME's sfcbox.zip has none):\n"
                                "MB90082.BIN — 9216 bytes — 1F0A5EFE";
static const char kInfoBSX[] = "Supports the following Satellaview BS-X cartridge ROMs:\nBS-X.bin — 1 MB — F51F07A0";
static const char kInfoSufami[] = "Supports the following Sufami Turbo base unit ROMs:\nSTBIOS.bin — 256 KB — 9B4CA911";
static const char kInfoNSS[] = "Supports the following Nintendo Super System BIOS releases:\n"
                               "nss.zip — MAME set: the three dumps below — C732AB81\n"
                               "nss-ic14.02.ic14 — 32 KB — E06CB58F\n"
                               "nss-v3.ic14 — 32 KB — AC385B53\n"
                               "nss-c.ic14 — 32 KB — A8E202B3\n"
                               "NSS-TEST.BIN — No$Cash test BIOS — 15616021";
static const char kInfoNSSFont[] = "Supports the following M50458 OSD character ROMs:\n"
                                   "m50458.zip — MAME set: both dumps below — 1CD87DF4\n"
                                   "m50458_char.bin — 4608 bytes — 011CC342\n"
                                   "m50458-001sp — 4608 bytes — 444F597D";
static const char kInfoSuperDisc[] = "Supports the following Super Disc BIOS cartridge ROMs:\n"
                                     "SDBR_v0.95.sfc — 128 KB, with or without a copier header — 3B64A370";
static const char kInfoDSP1[] = "Supports the following DSP-1 firmware dumps (Pilotwings needs this revision):\n"
                                "dsp1.bin — 8192 bytes — E359F184";
static const char kInfoDSP1B[] = "Supports the following DSP-1B firmware dumps (the other DSP-1 games use it):\n"
                                 "dsp1b.bin — 8192 bytes — 465C4E1C";
static const char kInfoDSP2[] = "Supports the following DSP-2 firmware dumps:\n"
                                "dsp2.bin — 8192 bytes — 9A984974";
static const char kInfoDSP3[] = "Supports the following DSP-3 firmware dumps:\n"
                                "dsp3.bin — 8192 bytes — D4A38EE7";
static const char kInfoDSP4[] = "Supports the following DSP-4 firmware dumps:\n"
                                "dsp4.bin — 8192 bytes — E15384C0";
static const char kInfoCX4[] = "Supports the following Cx4 data ROM dumps (Mega Man X2 and X3):\n"
                               "cx4.bin — 3072 bytes — B6E76A6A";
static const char kInfoXBand[] = "Supports the following XBAND modem BIOS ROMs:\n"
                                 "X-Band Modem BIOS (USA).sfc — 1 MB, with or without a copier header — A8B868A0";

// No-Intro dumps each slot accepts, all passing its size and signature checks.
static const char *const kNoIntroGB[] = {
	"Nintendo Game Boy Boot ROM (Japan) (En).gb — 256 bytes — C2F5CC97",
	"Nintendo Game Boy Boot ROM (World) (Rev 1).gb — 256 bytes — 59C8598E",
	"Nintendo Game Boy Pocket Boot ROM (World).gb — 256 bytes — E6920754", NULL
};
static const char *const kNoIntroGBC[] = {
	"Nintendo Game Boy Color Boot ROM (Japan) (En).gbc — 2304 bytes — E8EF5318",
	"Nintendo Game Boy Color Boot ROM (World).gbc — 2304 bytes — 41884E46",
	"Nintendo Game Boy Color Boot ROM (World) (Rev 1).gbc — 2304 bytes", NULL
};
static const char *const kNoIntroSGB1[] = {
	"Super Game Boy (Europe) (Beta) (1994-03-24).sfc — v1.0 — DACCC879",
	"Super Game Boy (Japan).sfc — v1.0 — 2E35EDBB",
	"Super Game Boy (Japan, USA) (Beta) (SYS-SGB X2).sfc — v1.0 — 4D93E5B5",
	"Super Game Boy (Japan, USA) (En) (Beta) (1994-03-06).sfc — v1.0 — 4D93E5B5",
	"Super Game Boy (Japan, USA) (En) (Beta) (1994-03-23) (Alt).sfc — v1.0 — F5FFB691",
	"Super Game Boy (Japan, USA) (En) (Beta) (1994-03-23).sfc — v1.0 — 5F99275B",
	"Super Game Boy (Japan, USA) (En) (Rev 1).sfc — v1.1 — 27A03C98",
	"Super Game Boy (Japan, USA) (En).sfc — v1.0 — 2E35EDBB",
	"Super Game Boy (Japan, USA) (Rev 1).sfc — v1.1 — 27A03C98",
	"Super Game Boy (World) (Rev 2).sfc — v1.2 — 8A4A174F", NULL
};
static const char *const kNoIntroSGB2[] = { "Super Game Boy 2 (Japan).sfc — v1.16 — CB176E45", NULL };
static const char *const kNoIntroBSX[] = {
	"BS-X - Sore wa Namae o Nusumareta Machi no Monogatari (Japan) (Rev 1).sfc — 1 MB — F51F07A0", NULL
};
static const char *const kNoIntroSufami[] = { "Sufami Turbo (Japan).sfc — 256 KB — 9B4CA911", NULL };
static const char *const kNoIntroDSP1[]  = { "DSP1 (World) (Enhancement Chip).bin — 8192 bytes — E359F184", NULL };
static const char *const kNoIntroDSP1B[] = { "DSP1 B (World) (Enhancement Chip).bin — 8192 bytes — 465C4E1C", NULL };
static const char *const kNoIntroDSP2[]  = { "DSP2 (World) (Enhancement Chip).bin — 8192 bytes — 9A984974", NULL };
static const char *const kNoIntroDSP3[]  = { "DSP3 (Japan) (Enhancement Chip).bin — 8192 bytes — D4A38EE7", NULL };
static const char *const kNoIntroDSP4[]  = { "DSP4 (World) (Enhancement Chip).bin — 8192 bytes — E15384C0", NULL };
static const char *const kNoIntroCX4[]   = { "CX4 (World) (Enhancement Chip).bin — 3072 bytes — B6E76A6A", NULL };

// Sizes match the loaders: sfcbox.h SFCBOX_KROM_SIZE / SFCBOX_FONT_SIZE,
// bsx.cpp BIOS_SIZE, memmap.cpp's 0x40000 STBIOS read, nss.h NSS_BIOS_SIZE /
// NSS_FONT_SIZE, superdisc.h SDISC_BIOS_SIZE, upd7725.h, hg51b.h, xband.h XBAND_ROM_SIZE. 0 = don't care (the SGB carts
// ship in two sizes, the CGB boot ROM in two layouts).
static const S9xBiosSlotInfo kSlots[S9X_NUM_BIOS_SLOTS] =
{
	{ "GameBoy",      "Game Boy",                       kNamesGB,        0x100,    "Optional, adds the boot logo",           kInfoGB,        kNoIntroGB },
	{ "GameBoyColor", "Game Boy Color",                 kNamesGBC,       0,        "Optional, adds boot logo and GB colors", kInfoGBC,       kNoIntroGBC },
	{ "SGB1",         "Super Game Boy",                 kNamesSGB1,      0,        NULL,                                     kInfoSGB1,      kNoIntroSGB1 },
	{ "SGB2",         "Super Game Boy 2",               kNamesSGB2,      0,        NULL,                                     kInfoSGB2,      kNoIntroSGB2 },
	{ "SGB1BootROM",  "Super Game Boy Boot ROM",        kNamesSGB1Boot,  0x100,    "Optional, built-in is used",             kInfoSGB1Boot,  NULL },
	{ "SGB2BootROM",  "Super Game Boy 2 Boot ROM",      kNamesSGB2Boot,  0x100,    "Optional, built-in is used",             kInfoSGB2Boot,  NULL },
	{ "SFCBoxKROM",   "Super Famicom Box",              kNamesKROM,      0x10000,  NULL,                                     kInfoKROM,      NULL },
	{ "SFCBoxFont",   "Super Famicom Box OSD Font",     kNamesFont,      9216,     NULL,                                     kInfoFont,      NULL },
	{ "BSX",          "Satellaview / BS-X",             kNamesBSX,       0x100000, NULL,                                     kInfoBSX,       kNoIntroBSX },
	{ "SufamiTurbo",  "Sufami Turbo",                   kNamesSufami,    0x40000,  NULL,                                     kInfoSufami,    kNoIntroSufami },
	{ "NSS",          "Nintendo Super System",          kNamesNSS,       0x8000,   NULL,                                     kInfoNSS,       NULL },
	{ "NSSFont",      "Nintendo Super System OSD Font", kNamesNSSFont,   0x1200,   NULL,                                     kInfoNSSFont,   NULL },
	{ "SuperDisc",    "Super Disc",                     kNamesSuperDisc, 0x20000,  NULL,                                     kInfoSuperDisc, NULL },
	{ "DSP1",         "DSP-1",                          kNamesDSP1,      UPD7725_FIRMWARE_SIZE, "Optional, built-in native chip",         kInfoDSP1,      kNoIntroDSP1 },
	{ "DSP1B",        "DSP-1B",                         kNamesDSP1B,     UPD7725_FIRMWARE_SIZE, "Optional, built-in native chip",         kInfoDSP1B,     kNoIntroDSP1B },
	{ "DSP2",         "DSP-2",                          kNamesDSP2,      UPD7725_FIRMWARE_SIZE, "Optional, built-in native chip",         kInfoDSP2,      kNoIntroDSP2 },
	{ "DSP3",         "DSP-3",                          kNamesDSP3,      UPD7725_FIRMWARE_SIZE, "Optional, built-in native chip",         kInfoDSP3,      kNoIntroDSP3 },
	{ "DSP4",         "DSP-4",                          kNamesDSP4,      UPD7725_FIRMWARE_SIZE, "Optional, built-in native chip",         kInfoDSP4,      kNoIntroDSP4 },
	{ "CX4",          "Cx4",                            kNamesCX4,       HG51B_DATAROM_SIZE,    "Optional, built-in native chip",         kInfoCX4,       kNoIntroCX4 },
	{ "XBAND",        "XBAND",                          kNamesXBand,     XBAND_ROM_SIZE, NULL,                                  kInfoXBand,     NULL },
};

static char g_paths[S9X_NUM_BIOS_SLOTS][S9X_BIOS_PATH_MAX];
static uint32 g_modes[S9X_NUM_BIOS_SLOTS];	// S9xChipMode, for the chip slots

static bool SlotValid (int slot)
{
	return slot >= 0 && slot < S9X_NUM_BIOS_SLOTS;
}

const S9xBiosSlotInfo *S9xGetBiosSlotInfo (int slot)
{
	return SlotValid(slot) ? &kSlots[slot] : NULL;
}

std::string S9xBiosSlotInfoText (int slot)
{
	const S9xBiosSlotInfo *info = S9xGetBiosSlotInfo(slot);
	if (!info || !info->info) return (std::string());
	std::string text(info->info);
	for (const char *const *n = info->nointro; n && *n; n++)
		text += std::string("\n") + *n;
	return (text);
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

bool8 S9xBiosSlotHasChipMode (int slot)
{
	return slot >= S9X_BIOS_DSP1 && slot <= S9X_BIOS_CX4;
}

int S9xGetChipMode (int slot)
{
	return (S9xBiosSlotHasChipMode(slot) && g_modes[slot] <= S9X_CHIP_FIRMWARE) ? (int) g_modes[slot] : S9X_CHIP_AUTO;
}

void S9xSetChipMode (int slot, int mode)
{
	if (S9xBiosSlotHasChipMode(slot) && mode >= S9X_CHIP_AUTO && mode <= S9X_CHIP_FIRMWARE)
		g_modes[slot] = (uint32) mode;
}

int S9xChipModeInEffect (int slot)
{
	const int mode = S9xGetChipMode(slot);
	return (mode != S9X_CHIP_AUTO) ? mode : S9X_CHIP_NATIVE;
}

uint32 *S9xGetChipModeBuffer (int slot)
{
	return S9xBiosSlotHasChipMode(slot) ? &g_modes[slot] : NULL;
}

static const S9xBiosFamily kFamilies[S9X_BIOS_NUM_FAMILIES] =
{
	{ "Commercial & Arcade Boards",
	  { S9X_BIOS_SFCBOX_KROM, S9X_BIOS_SFCBOX_FONT, S9X_BIOS_NSS, S9X_BIOS_NSS_FONT, -1 } },
	{ "Expansions & Peripherals",
	  { S9X_BIOS_SGB1, S9X_BIOS_SGB1_BOOT, S9X_BIOS_SGB2, S9X_BIOS_SGB2_BOOT,
	    S9X_BIOS_BSX, S9X_BIOS_SUFAMI, S9X_BIOS_SUPERDISC, S9X_BIOS_XBAND, -1 } },
	{ "CoProcessors & Special Chips",
	  { S9X_BIOS_DSP1, S9X_BIOS_DSP1B, S9X_BIOS_DSP2, S9X_BIOS_DSP3, S9X_BIOS_DSP4, S9X_BIOS_CX4, -1 } },
	{ "Handheld Systems",
	  { S9X_BIOS_GB, S9X_BIOS_GBC, -1 } },
};

const S9xBiosFamily *S9xGetBiosFamily (int family)
{
	return (family >= 0 && family < S9X_BIOS_NUM_FAMILIES) ? &kFamilies[family] : NULL;
}

const char *S9xBiosGroupName (int group)
{
	return (group == S9X_BIOS_GROUP_DSP) ? "DSP" : "";
}

S9xBiosNesting S9xGetBiosNesting (int slot)
{
	switch (slot)
	{
		case S9X_BIOS_SFCBOX_FONT:	return { S9X_BIOS_SFCBOX_KROM, FALSE, NULL };
		case S9X_BIOS_NSS_FONT:		return { S9X_BIOS_NSS, FALSE, NULL };
		case S9X_BIOS_SGB1_BOOT:	return { S9X_BIOS_SGB1, TRUE, "Boot ROM" };
		case S9X_BIOS_SGB2_BOOT:	return { S9X_BIOS_SGB2, TRUE, "Boot ROM" };
		case S9X_BIOS_DSP1:
		case S9X_BIOS_DSP1B:
		case S9X_BIOS_DSP2:
		case S9X_BIOS_DSP3:
		case S9X_BIOS_DSP4:			return { S9X_BIOS_UNDER_GROUP + S9X_BIOS_GROUP_DSP, TRUE, NULL };
		default:					return { -1, TRUE, NULL };
	}
}

// The chipbench tables (s9x-harness/chipbench): same-binary runs, the firmware equal to native.
static const S9xBiosBenchScene kBench[] =
{
	{ S9X_BIOS_DSP1,  "Pilotwings",             { 0.784f, 0.813f, 1.049f }, {  91, 310, 2463 } },
	{ S9X_BIOS_DSP1B, "Super Mario Kart 2P",    { 0.780f, 0.788f, 0.846f }, {  17,  43,  378 } },
	{ S9X_BIOS_DSP1B, "Super Mario Kart 1P",    { 0.755f, 0.712f, 0.755f }, {  26,  71,  594 } },
	{ S9X_BIOS_DSP1B, "Lock On",                { 0.855f, 0.866f, 1.114f }, {  67, 208, 1533 } },
	{ S9X_BIOS_DSP1B, "Super Air Diver",        { 0.818f, 0.876f, 1.131f }, {  84, 321, 2428 } },
	{ S9X_BIOS_DSP1B, "Ballz",                  { 0.962f, 0.962f, 1.010f }, {  29,  65,  598 } },
	{ S9X_BIOS_DSP1B, "Suzuka 8 Hours",         { 0.781f, 0.799f, 0.879f }, {  12,  42,  339 } },
	{ S9X_BIOS_DSP2,  "Dungeon Master play",    { 0.567f, 0.598f, 0.759f }, { 169, 572, 3741 } },
	{ S9X_BIOS_DSP2,  "Dungeon Master attract", { 0.728f, 0.724f, 0.718f }, {   0,  11,   10 } },
	{ S9X_BIOS_DSP3,  "SD Gundam GX battle",    { 0.991f, 0.785f, 0.859f }, { 128, 312, 1801 } },
	{ S9X_BIOS_DSP3,  "SD Gundam GX attract",   { 0.839f, 0.841f, 0.839f }, {   6,  48,  108 } },
	{ S9X_BIOS_DSP4,  "Top Gear 3000",          { 0.733f, 0.757f, 0.794f }, {  87, 410, 1269 } },
	{ S9X_BIOS_DSP4,  "Top Gear 3000 JP",       { 0.730f, 0.745f, 0.795f }, { 105, 474, 1455 } },
	{ S9X_BIOS_CX4,   "Mega Man X2 attract",    { 0.616f, 0.617f, 0.700f }, {  35, 144, 1850 } },
	{ S9X_BIOS_CX4,   "Mega Man X2 wireframe",  { 0.556f, 0.524f, 1.081f }, {  28,  99, 1374 } },
	{ S9X_BIOS_CX4,   "Mega Man X3 attract",    { 0.655f, 0.675f, 0.702f }, {   7,  55,  455 } },
	{ S9X_BIOS_CX4,   "Mega Man X3 gameplay",   { 0.884f, 0.877f, 0.963f }, {  37, 173, 2166 } },
};

const S9xBiosBenchScene *S9xGetBiosBench (int *count)
{
	if (count)
		*count = (int) (sizeof(kBench) / sizeof(kBench[0]));
	return (kBench);
}

int S9xBiosBenchScenes (int slot)
{
	int	n = 0;
	for (const S9xBiosBenchScene &b : kBench)
		n += b.slot == slot;
	return (n);
}

std::string S9xBiosPathsFingerprint (void)
{
	std::string out;
	for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
	{
		out += g_paths[slot];
		if (S9xBiosSlotHasChipMode(slot))
			out += (char) ('0' + S9xChipModeInEffect(slot));
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
	if (slot == S9X_BIOS_SUPERDISC) return (n == SDISC_BIOS_SIZE || n == SDISC_BIOS_SIZE + 0x200);
	if (slot == S9X_BIOS_SFCBOX_KROM) return (n == 0x10000 || n == 0x20000);	// KROM 1.00 / 2.00
	if (slot == S9X_BIOS_XBAND)     return (n == XBAND_ROM_SIZE || n == XBAND_ROM_SIZE + 0x200);
	return (kSlots[slot].size == 0 || n == kSlots[slot].size);
}

bool8 S9xBiosHasImageOfSize (int slot, uint32 size)
{
	if (!SlotValid(slot) || !g_paths[slot][0])
		return (FALSE);

	if (IsZipFile(g_paths[slot]))
	{
#ifdef UNZIP_SUPPORT
		unzFile file = unzOpen(g_paths[slot]);
		if (!file) return (FALSE);

		bool found = false;
		for (int pos = unzGoToFirstFile(file); pos == UNZ_OK && !found; pos = unzGoToNextFile(file))
		{
			unz_file_info info;
			if (unzGetCurrentFileInfo(file, &info, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK)
				found = info.uncompressed_size == size;
		}
		unzClose(file);
		return found ? TRUE : FALSE;
#else
		return (FALSE);
#endif
	}

	return (FileSize(g_paths[slot]) == (long) size) ? TRUE : FALSE;
}

// The size half of the check, zip-aware. Kept separate so the status can say
// which test failed.
static bool8 SizeOkForPath (int slot)
{
	// Judge an archive off the central directory rather than inflating it â€”
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
	KIND_SGB1_CART, KIND_SGB2_CART, KIND_BSX_BIOS, KIND_SUFAMI_BIOS,
	KIND_NSS_BIOS, KIND_NSS_FONT, KIND_SUPERDISC_BIOS,
	KIND_DSP1_FIRMWARE, KIND_DSP1B_FIRMWARE, KIND_DSP2_FIRMWARE, KIND_DSP3_FIRMWARE,
	KIND_DSP4_FIRMWARE, KIND_NECDSP_FIRMWARE, KIND_CX4_DATAROM, KIND_XBAND_BIOS
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
		case KIND_NSS_BIOS:  return ("Nintendo Super System BIOS");
		case KIND_NSS_FONT:  return ("NSS OSD charset");
		case KIND_SUPERDISC_BIOS: return ("Super Disc BIOS");
		case KIND_DSP1_FIRMWARE: return ("DSP-1 firmware");
		case KIND_DSP1B_FIRMWARE: return ("DSP-1B firmware");
		case KIND_DSP2_FIRMWARE: return ("DSP-2 firmware");
		case KIND_DSP3_FIRMWARE: return ("DSP-3 firmware");
		case KIND_DSP4_FIRMWARE: return ("DSP-4 firmware");
		case KIND_NECDSP_FIRMWARE: return ("other DSP firmware");
		case KIND_CX4_DATAROM: return ("Cx4 data ROM");
		case KIND_XBAND_BIOS:     return ("XBAND BIOS");
		default:             return ("unrecognised image");
	}
}

static uint32 ImageCRC32 (const uint8 *d, uint32 n)
{
	uint32 crc = 0xffffffff;
	for (uint32 i = 0; i < n; i++)
	{
		crc ^= d[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32) (-(int32) (crc & 1)));
	}
	return (~crc);
}

static bool IsNoCashNSSTest (const uint8 *d, uint32 n, uint32 full)
{
	return full == 0x8000 && n >= 0x8000 && ImageCRC32(d, 0x8000) == S9X_NSS_NOCASH_TEST_CRC;
}

// A boot ROM opens LD SP,$FFFE; the SGB one then sets P1 to $30, and its $FD
// byte is the A it hands the cart â€” $01 on SGB1, $FF on SGB2.
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

	// The Super Disc BIOS, with or without a copier header.
	if (full == SDISC_BIOS_SIZE && n >= 0x8000 && S9xSuperDiscIsBIOS(d, SDISC_BIOS_SIZE))
		return (KIND_SUPERDISC_BIOS);
	if (full == SDISC_BIOS_SIZE + 0x200 && n >= 0x8200 && S9xSuperDiscIsBIOS(d + 0x200, SDISC_BIOS_SIZE))
		return (KIND_SUPERDISC_BIOS);

	// DSP-n firmware all shares one layout, so the dump is known by its CRC.
	if (full == UPD7725_FIRMWARE_SIZE && n >= UPD7725_FIRMWARE_SIZE && S9xUPD7725IsFirmware(d, n))
	{
		static const struct { uint32 crc; int kind; } dumps[] = {
			{ 0xE359F184, KIND_DSP1_FIRMWARE }, { 0x465C4E1C, KIND_DSP1B_FIRMWARE },
			{ 0x9A984974, KIND_DSP2_FIRMWARE }, { 0xD4A38EE7, KIND_DSP3_FIRMWARE },
			{ 0xE15384C0, KIND_DSP4_FIRMWARE }
		};
		const uint32 crc = ImageCRC32(d, n);
		for (size_t i = 0; i < sizeof(dumps) / sizeof(dumps[0]); i++)
			if (dumps[i].crc == crc)
				return (dumps[i].kind);
		return (KIND_NECDSP_FIRMWARE);
	}

	if (full == HG51B_DATAROM_SIZE && n >= HG51B_DATAROM_SIZE && S9xHG51BIsDataROM(d, HG51B_DATAROM_SIZE))
		return (KIND_CX4_DATAROM);

	// XBAND names itself in its HiROM header, so this needs 64K in hand.
	if (full == XBAND_ROM_SIZE && S9xXBandIsBIOS(d, n))
		return (KIND_XBAND_BIOS);
	if (full == XBAND_ROM_SIZE + 0x200 && n > 0x200 && S9xXBandIsBIOS(d + 0x200, n - 0x200))
		return (KIND_XBAND_BIOS);

	// The NSS supervisor BIOS is 32K of Z80 code whose reset path opens
	// LD A,I / JP Z,nnnn; its OSD charset is 128 glyphs of 18 rows with the
	// twelve dots left-aligned at bit 11, so every row word's top nibble is
	// clear.
	if (full == 0x8000 && n >= 3 && d[0] == 0xED && d[1] == 0x57 && d[2] == 0xCA)
		return (KIND_NSS_BIOS);
	// nocash's test opens JP 1300h instead; it is known by checksum.
	if (IsNoCashNSSTest(d, n, full))
		return (KIND_NSS_BIOS);
	if (full == 0x1200 && n >= 0x1200)
	{
		uint32 ink = 0;
		for (uint32 i = 0; i < 0x1200; i += 2)
		{
			if (d[i] & 0xF0) { ink = 0; break; }
			if (d[i] | d[i + 1]) ink++;
		}
		if (ink > 256) return (KIND_NSS_FONT);
	}

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
		case S9X_BIOS_NSS:       return (KIND_NSS_BIOS);
		case S9X_BIOS_NSS_FONT:  return (KIND_NSS_FONT);
		case S9X_BIOS_SUPERDISC: return (KIND_SUPERDISC_BIOS);
		case S9X_BIOS_DSP1:      return (KIND_DSP1_FIRMWARE);
		case S9X_BIOS_DSP1B:     return (KIND_DSP1B_FIRMWARE);
		case S9X_BIOS_DSP2:      return (KIND_DSP2_FIRMWARE);
		case S9X_BIOS_DSP3:      return (KIND_DSP3_FIRMWARE);
		case S9X_BIOS_DSP4:      return (KIND_DSP4_FIRMWARE);
		case S9X_BIOS_CX4:       return (KIND_CX4_DATAROM);
		case S9X_BIOS_XBAND:     return (KIND_XBAND_BIOS);
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
	if (slot == S9X_BIOS_SFCBOX_KROM) return ("wrong size: need 65536 or 131072 bytes");
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
	const uint32 probe_len = (slot == S9X_BIOS_XBAND) ? 0x10200 : 0x8200;
	if (!S9xReadBiosImage(g_paths[slot], img, probe_len, AcceptKind, &probe))
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
	if (detail && want == KIND_NSS_BIOS &&
	    IsNoCashNSSTest(img.data(), (uint32) img.size(), (uint32) img.size()))
		*detail = "No$Cash Test Bios";
	if (detail && slot == S9X_BIOS_SFCBOX_KROM)
	{
		const bool v1 = S9xBiosHasImageOfSize(slot, 0x10000) != 0;
		const bool v2 = S9xBiosHasImageOfSize(slot, 0x20000) != 0;
		*detail = (v1 && v2) ? "KROM 1.00 + 2.00" : v2 ? "KROM 2.00" : "KROM 1.00";
	}

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
