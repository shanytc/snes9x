/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _MEMMAP_H_
#define _MEMMAP_H_

#define MEMMAP_BLOCK_SIZE	(0x1000)
#define MEMMAP_NUM_BLOCKS	(0x1000000 / MEMMAP_BLOCK_SIZE)
#define MEMMAP_SHIFT		(12)
#define MEMMAP_MASK			(MEMMAP_BLOCK_SIZE - 1)

#include <string>
#include <vector>
#include <cstdint>

struct CMemory
{
	enum
	{ MAX_ROM_SIZE = 0x1000000 };

	enum file_formats
	{ FILE_ZIP, FILE_JMA, FILE_DEFAULT };

	enum
	{ NOPE, YEAH, BIGFIRST, SMALLFIRST };

	enum
	{ MAP_TYPE_I_O, MAP_TYPE_ROM, MAP_TYPE_RAM };

	enum
	{
		MAP_CPU,
		MAP_PPU,
		MAP_LOROM_SRAM,
		MAP_LOROM_SRAM_B,
		MAP_HIROM_SRAM,
		MAP_DSP,
		MAP_SA1RAM,
		MAP_BWRAM,
		MAP_BWRAM_BITMAP,
		MAP_BWRAM_BITMAP2,
		MAP_SPC7110_ROM,
		MAP_SPC7110_DRAM,
		MAP_RONLY_SRAM,
		MAP_C4,
		MAP_OBC_RAM,
		MAP_SETA_DSP,
		MAP_SETA_RISC,
		MAP_BSX,
		MAP_SGB_ICD2,
		MAP_EVENT,
		MAP_SFCBOX_SRAM,
		MAP_NONE,
		MAP_LAST
	};

	uint8	NSRTHeader[32];
	int32	HeaderCount;

	uint8	RAM[0x20000];
	std::vector<uint8_t> ROMStorage;
	uint8   *ROM;
	std::vector<uint8_t> SRAMStorage;
	uint8	*SRAM;
	const size_t SRAM_SIZE = 0x80000;
	uint8	VRAM[0x10000];
	uint8	*FillRAM;
	uint8	*BWRAM;
	uint8	*C4RAM;
	uint8	*OBC1RAM;
	uint8	*BSRAM;
	uint8	*BIOSROM;

	uint8	*Map[MEMMAP_NUM_BLOCKS];
	uint8	*WriteMap[MEMMAP_NUM_BLOCKS];
	uint8	BlockIsRAM[MEMMAP_NUM_BLOCKS];
	uint8	BlockIsROM[MEMMAP_NUM_BLOCKS];
	uint8	ExtendedFormat;

	std::string ROMFilename;
	char	ROMName[ROM_NAME_LEN];
	char	ROMId[5];
	int32	CompanyId;
	uint8	ROMRegion;
	uint8	ROMSpeed;
	uint8	ROMType;
	uint8	ROMSize;
	uint32	ROMChecksum;
	uint32	ROMComplementChecksum;
	uint32	ROMCRC32;
	unsigned char ROMSHA256[32];
	int32	ROMFramesPerSecond;

	bool8	HiROM;
	bool8	LoROM;
	uint8	SRAMSize;
	uint32	SRAMMask;
	uint32	CalculatedSize;
	uint32	CalculatedChecksum;

	// ports can assign this to perform some custom action upon loading a ROM (such as adjusting controls)
	void	(*PostRomInitFunc) (void);

	bool8	Init (void);
	void	Deinit (void);

	int		ScoreHiROM (bool8, int32 romoff = 0);
	int		ScoreLoROM (bool8, int32 romoff = 0);
	int		First512BytesCountZeroes() const;
	uint32	HeaderRemove (uint32, uint8 *);
	uint32	FileLoader (uint8 *, const char *, uint32);
    bool8   LoadROMMem (const uint8 *, uint32, const char* optional_rom_filename = NULL);
	bool8	LoadROM (const char *);
	bool8	LoadROMWithSGBBIOS (const char *gb_path, const char *bios_path,
	                            bool skip_gb_boot_rom = false);
	bool8	LoadROMWithSGBBIOSBytes (const uint8 *gb_bytes, uint32 gb_size,
	                                  const char *gb_path, const char *bios_path,
	                                  bool skip_gb_boot_rom = false);
	// Detect+load a Game Boy ROM from a memory buffer, routing it into the
	// SGB subsystem. Returns 1 = handled (loaded as GB/SGB), 0 = not a GB
	// ROM, -1 = GB ROM but the load failed.
	int		LoadGBFromBytes (const uint8 *rom, uint32 size, const char *filename);
    bool8	LoadROMInt (int32);
    bool8   LoadMultiCartMem (const uint8 *, uint32, const uint8 *, uint32, const uint8 *, uint32);
	bool8	LoadMultiCart (const char *, const char *);
    bool8	LoadMultiCartInt ();
	// Carts that need a BIOS beside them (Sufami Turbo, the Satellaview
	// shell) so File -> Load Game handles them. 1 = loaded, 0 = failed,
	// -1 = not one of these formats.
	int		LoadBIOSPairedCart (const char *filename, int32 size);
	bool8	LoadSufamiTurbo ();
	bool8	LoadBSCart ();
	bool8	LoadSFCBox (int32);
	bool8	LoadGNEXT ();
	bool8	LoadSRAM (const char *);
	bool8	SaveSRAM (const char *);
	void	ClearSRAM (bool8 onlyNonSavedSRAM = 0);
	bool8	LoadSRTC (void);
	bool8	SaveSRTC (void);
	bool8	SaveMPAK (const char *);

	void	ParseSNESHeader (uint8 *);
	void	InitROM (void);

	uint32	map_mirror (uint32, uint32);
	void	map_lorom (uint32, uint32, uint32, uint32, uint32);
	void	map_hirom (uint32, uint32, uint32, uint32, uint32);
	void	map_lorom_offset (uint32, uint32, uint32, uint32, uint32, uint32);
	void	map_hirom_offset (uint32, uint32, uint32, uint32, uint32, uint32);
	void	map_space (uint32, uint32, uint32, uint32, uint8 *);
	void	map_index (uint32, uint32, uint32, uint32, int, int);
	void	map_System (void);
	void	map_WRAM (void);
	void	map_LoROMSRAM (void);
	void	map_HiROMSRAM (void);
	void	map_DSP (void);
	void	map_C4 (void);
	void	map_OBC1 (void);
	void	map_SetaRISC (void);
	void	map_SetaDSP (void);
	void	map_WriteProtectROM (void);
	void	Map_Initialize (void);
	void	Map_LoROMMap (void);
	void	Map_SGBLoROMMap (void);
	void	Map_NoMAD1LoROMMap (void);
	void	Map_JumboLoROMMap (void);
	void	Map_ROM24MBSLoROMMap (void);
	void	Map_SRAM512KLoROMMap (void);
	void	Map_SufamiTurboLoROMMap (void);
	void	Map_SufamiTurboPseudoLoROMMap (void);
	void	Map_SuperFXLoROMMap (void);
	void	Map_SuperFX3LoROMMap (void);
	void	Map_SetaDSPLoROMMap (void);
	void	Map_SDD1LoROMMap (void);
	void	Map_SDD1DecompressedMap (void);
	void	Map_SA1LoROMMap (void);
	void	Map_BSSA1LoROMMap (void);
	void	Map_HiROMMap (void);
	void	Map_ExtendedHiROMMap (void);
	void	Map_SPC7110HiROMMap (void);
	void	Map_BSCartLoROMMap(uint8);
	void	Map_BSCartHiROMMap(void);

	uint16	checksum_calc_sum (uint8 *, uint32);
	uint16	checksum_mirror_sum (uint8 *, uint32 &, uint32 mask = 0x800000);
	void	Checksum_Calculate (void);

	bool8	match_na (const char *);
	bool8	match_nn (const char *);
	bool8	match_nc (const char *);
	bool8	match_id (const char *);
	void	ApplyROMFixes (void);
    std::string SafeString(std::string s, bool allow_jis = false);
	void	CheckForAnyPatch (const char *, bool8, int32 &);

	void	MakeRomInfoText (char *);
	std::string GetMultilineROMInfo();

	const char *	MapType (void);
	const char *	StaticRAMSize (void);
	const char *	Size (void);
	const char *	Revision (void);
	const char *	KartContents (void);
	const char *	Country (void);
	const char *	PublishingCompany (void);
};

struct SMulti
{
	int		cartType;
	int32	cartSizeA, cartSizeB;
	int32	sramSizeA, sramSizeB;
	uint32	sramMaskA, sramMaskB;
	uint32	cartOffsetA, cartOffsetB;
	uint8	*sramA, *sramB;
	char	fileNameA[PATH_MAX + 1], fileNameB[PATH_MAX + 1];
};

extern CMemory	Memory;
extern SMulti	Multi;

inline bool S9xInterlaceField()
{
	return (Memory.FillRAM[0x213F] & 0x80) >> 7;
}

void S9xAutoSaveSRAM (void);
bool8 LoadZip(const char *, uint32 *, uint8 *, uint32);
bool8 S9xSGBBIOSAvailable(uint8 mode, const char *gb_rom_path);

// TRUE when `data` is a Super Game Boy BIOS cart image, judged the way the
// loader judges it: *out_mode comes back 1 for SGB1, 2 for SGB2.
bool8 S9xIsSGBBIOSImage(const uint8 *data, uint32 size, uint8 *out_mode);

// Re-show the load banner ("<game>" (NTSC) via Super Game Boy 2  [sgb2.sfc]).
// No-op unless Game Boy content is loaded.
void S9xAnnounceGBBios(void);

// TRUE when the BIOS Manager changed a path after the loaded cart took its
// BIOS. The paths are only read by a load, so a hard reset wants one then.
bool8 S9xBiosChangedSinceLoad(void);

// Which console GB content runs on (Settings.GBBootPolicy). The Automatic
// entries pick from what the cart supports, breaking ties in the stated
// direction — that tie is what "triple boot" carts (DMG + CGB + SGB) hit.
//
// These are saved as integers in every port's config, so a new entry goes on
// the END and the menus order themselves with S9xGBBootPolicyMenuOrder.
enum S9xGBBootPolicy
{
	S9X_GBBOOT_GB = 0,       // force Game Boy
	S9X_GBBOOT_GBC,          // force Game Boy Color
	S9X_GBBOOT_SGB,          // force Super Game Boy (SGB1)
	S9X_GBBOOT_SGB_GBC,      // SGB1 border + real CGB color (hack, not real HW)
	S9X_GBBOOT_SGB2,         // force Super Game Boy 2
	// 5 and 6 were "Automatic, prefer GB" and "prefer GBC". One Automatic
	// replaced all three; they survive only so a saved config still loads, and
	// S9xNormalizeGBBootPolicy folds them into S9X_GBBOOT_AUTO.
	S9X_GBBOOT_AUTO_GB_LEGACY,
	S9X_GBBOOT_AUTO_GBC_LEGACY,
	S9X_GBBOOT_AUTO,         // read the cart header: SGB > GBC > GB — default
	S9X_GBBOOT_SGB2_GBC,     // SGB2 border + real CGB color (hack, not real HW)
	S9X_NUM_GBBOOT_POLICIES
};

// Menu order, which is not enum order: the four real consoles, then the two
// color hacks, then Automatic. Ports walk this order and start a new group —
// a separator — wherever S9xGBBootPolicyGroup changes. Shorter than the enum,
// which still carries the two retired Automatic values.
extern const uint8 S9xGBBootPolicyMenuOrder[];
extern const int   S9xGBBootPolicyMenuCount;
int S9xGBBootPolicyGroup(int policy);

// A saved policy folded onto one a menu still offers: out-of-range values and
// the two retired Automatic entries both become S9X_GBBOOT_AUTO.
uint8 S9xNormalizeGBBootPolicy(int policy);

// The consoles a port offers as a hotkey: the four real ones and the two color
// hacks, in menu order. `key` is the name a config file stores the binding
// under, so every port spells them the same. Automatic is not here — it is a
// rule, not a console, and picking it by key would say nothing about what runs.
struct S9xGBModelHotkey
{
	const char *key;     // config key / shortcut name
	const char *label;   // short label for a bindings list
	uint8       policy;  // S9xGBBootPolicy it selects
};
extern const S9xGBModelHotkey S9xGBModelHotkeys[];
extern const int              S9xGBModelHotkeyCount;


enum S9xGBConsole { S9X_GBCON_GB = 0, S9X_GBCON_GBC, S9X_GBCON_SGB };

// Resolve Settings.GBBootPolicy against the cart's header. `cgb_flag` is $0143,
// `sgb_flag` is $0146 and `old_licensee` is $014B — the Super Game Boy ignores
// command packets unless the latter two are $03 and $33, so both are needed to
// know whether a cart is really SGB-enhanced. `sgb_available` says whether an
// SGB BIOS was found, since SGB can't be picked without one. `out_force_cgb`
// comes back true for the SGB+GBC hack, where the SGB BIOS runs with CGB
// hardware enabled.
S9xGBConsole S9xResolveGBConsole(uint8 cgb_flag, uint8 sgb_flag, uint8 old_licensee,
                                 bool8 sgb_available, bool8 *out_force_cgb);

// Display name for each policy, for the Emulation -> Game Boy Model menu.
const char *S9xGBBootPolicyName(int policy);

// Whether a policy can deliver the console it names. Only the Super Game Boy
// entries can fail: without their BIOS they quietly run the cart as GBC/GB
// instead, so a menu should grey them out. `gb_rom_path` joins the BIOS search
// (the cart's own directory is one of the places it looks); NULL is fine.
// Why an entry cannot be picked, so a menu can say "(Missing BIOS)" only when
// that is the actual reason rather than for anything unavailable.
enum S9xGBPolicyBlock
{
	S9X_GBPOLICY_OK = 0,
	S9X_GBPOLICY_NO_BIOS,   // the console it names has no BIOS installed
	S9X_GBPOLICY_CART       // the loaded cart cannot use it
};
S9xGBPolicyBlock S9xGBBootPolicyBlocked(int policy, const char *gb_rom_path);

bool8 S9xGBBootPolicyAvailable(int policy, const char *gb_rom_path);
// Content-sniff a buffer for a Game Boy cart (Nintendo logo at 0x0104, incl.
// the Sachen scrambled variant). Lets in-memory callers route GB carts away
// from the SNES/BS-X/Sufami load paths.
bool S9xRomBytesAreGb(const uint8 *rom, int32 size);

// NEC uPD78214 SNES-EVENT board: status/select registers, session timer,
// board work-RAM and the four-image game window. Shared by the two known
// event carts, distinguished by SPF94::board:
//   EVENT_BOARD_PF94 — PowerFest '94  (status $10:6000, select $20:6000)
//   EVENT_BOARD_CC92 — Campus Challenge '92 (status $C0:0000, select $E0:0000)
// Active only when the event cart (master program / combined image) is loaded.
#define EVENT_BOARD_PF94	0
#define EVENT_BOARD_CC92	1

struct SPF94
{
	bool8	active;
	uint8	board;
	uint8	select, status;
	bool8	timerOn;
	uint32	timerStart, timerFrames;
	uint32	romOff[4], romSize[4];
};

extern SPF94 PF94;

uint8 S9xGetEvent (uint32 Address);
void S9xSetEvent (uint8 Byte, uint32 Address);

// Super Famicom Box shared-SRAM window (sfcbox.cpp)
uint8 S9xGetSFCBoxSRAM (uint32 Address);
void S9xSetSFCBoxSRAM (uint8 Byte, uint32 Address);
void S9xPF94Reset (void);
void S9xPF94PostLoadState (void);
void S9xPF94LoadGames (void);
int S9xPF94TimeRemaining (void);
int S9xEventTimerMinutes (void);
int S9xEventTimerDisplay (void);

enum s9xwrap_t
{
	WRAP_NONE,
	WRAP_BANK,
	WRAP_PAGE
};

enum s9xwriteorder_t
{
	WRITE_01,
	WRITE_10
};

#include "getset.h"

#endif
