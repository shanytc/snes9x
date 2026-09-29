/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Famicom Box (hotel SNES) supervisor board. Wraps the HD64180 core
// (hd64180.cpp) with the board's custom I/O (fullsnes "SNES Hotel Boxes"):
// keyswitch/coin/front-panel inputs, the WRIO/RDIO serial link to the SNES,
// joypad sniff/inject latches, the S-3520CF RTC, the MB90082 OSD overlay,
// the C0h/C1h SNES mapping registers and the SNES reset line. The cart
// loader half (GROM image parsing, socket tables) lives in memmap.cpp.
// See docs/sfcbox.md.

#ifndef _SFCBOX_H_
#define _SFCBOX_H_

#include "port.h"
#include <string>
#include <vector>

#define SFCBOX_KROM_SIZE	0x20000	// the Z180's KROM window; 2.00 fills it
#define SFCBOX_KROM1_SIZE	0x10000	// 1.00 is mirrored into both halves
#define SFCBOX_WRAM_SIZE	0x8000
#define SFCBOX_GROM_SIZE	0x8000
#define SFCBOX_OSD_W		24
#define SFCBOX_OSD_H		12
#define SFCBOX_FONT_SIZE	9216	// MB90082.BIN: 256 chars x 18 rows x 2 bytes

struct SSFCBoxRTC		// Seiko S-3520CF
{
	// Pin latches (from port [A0h].W)
	uint8	CS, CLK, DirRead, DataOut;
	uint8	DataIn;		// current output bit (read at [A0h].R bit0)

	// Transfer state
	int32	BitPos;			// bits since /CS assert
	uint8	ShiftIn;		// nibble being collected from the master
	uint8	Index;			// last index nibble received
	uint8	OutNibbles[64];	// scheduled output, indexed by nibble position

	// Chip state
	uint8	Mode;			// register F low bits (0=RTC, 2/3=SRAM pages)
	uint8	Control;		// register D
	uint8	SRAMPages[2][15];

	// Timekeeping (emulated, initialized from the host clock)
	int32	Sec, Min, Hour, Day, Month, Year, Weekday;
	int32	FrameAccum;
};

struct SSFCBoxOSD		// Fujitsu MB90082
{
	uint8	PendingCmd;		// first byte of a pair (bit7 set), 0 = none
	uint8	VRAMChar[SFCBOX_OSD_H][SFCBOX_OSD_W];
	uint8	VRAMColor[SFCBOX_OSD_H][SFCBOX_OSD_W];	// char color | bg<<3 | AT<<6 | blink<<7
	uint8	CursorX, CursorY;
	uint8	FillMode;
	uint8	CharColor, BgColor;
	uint8	DisplayEnable, ColorMode;
	uint8	ExtSync;		// Screen Control 1 IE bit: 1 = genlock to the SNES video
	uint8	LineCtrl[SFCBOX_OSD_H];	// bit0 BK solid, bit1 zoomY, bit2 zoomX, bit3 BC bg shown
	uint8	UnderColor;
	uint8	XOfs, YOfs;
	uint8	FontLoaded;
	uint8	Font[SFCBOX_FONT_SIZE];
};

struct SSFCBox
{
	bool8	Active;

	// Board memories
	uint8	KROM[SFCBOX_KROM_SIZE];
	uint8	KROMVersion;				// 1 or 2 (KROM 1.00 / 2.00), 0 = none loaded
	uint8	WRAM[SFCBOX_WRAM_SIZE];		// 32K battery-backed (low 16K = save area)
	uint8	*GROM[2];					// point into Memory.ROM, NULL = slot empty

	// Cartridge geometry, filled by the loader: per slot, per socket
	// (0=ROM5/menu, 1=ROM1/7/12, 2=ROM3/9, 3=IC20/GSU) an offset into
	// Memory.ROM and a power-of-two size (0 = not populated).
	uint32	RomOffset[2][4];
	uint32	RomSize[2][4];
	uint8	SlotChipset[2];				// GROM[4]: bit0 SRAM, bit1 DSP, bit2 GSU
	bool8	SlotPresent[2];

	// Z180-side latched outputs
	uint8	Port80W, Port81W;
	uint8	MapReg0, MapReg1;			// ports C0h/C1h

	// Inputs
	uint8	Keyswitch;					// rotary position 0-5 (bit index into [80h].R)
	int32	SwitchCycles[2];			// PHI cycles left of a Reset / GAME/TV press
	int32	CoinCycles;					// PHI cycles left of coin-switch closure

	// SNES link + reset
	uint8	WRIOOut;					// last $4201 write
	bool8	SNESHeld;					// reset line low: SNES frozen
	bool8	PendingSNESReset;			// rising edge seen, soft-reset at a safe point

	// KROM WRAM save-area write protect ([A0h].W bit7) and the SRAM
	// window the mapping registers currently grant the SNES
	uint8	WRAMUnlock;
	uint32	SRAMWindowMask, SRAMWindowBase;
	bool8	SRAMHiROM;

	int32	LastVCounter;

	// Joypad latches ([83h]-[87h])
	uint8	JoyCtrl;					// [83h].W
	uint8	JoyData[4];					// captured pads, KROM polarity
	uint8	JoyInject[4];				// [84h-87h].W in manual mode
	uint8	JoyReady;
	int32	JoyShiftPos[2];				// manual-mode serial shift positions
	uint8	Int1Line;
	uint8	VBlankToggle;

	// Watchdog (external logic fed by [81h].W bit6 transitions)
	int32	WatchdogCycles;

	// Z180 <-> SNES clock ratio bookkeeping
	int64	CycleRemainder;

	struct SSFCBoxRTC	RTC;
	struct SSFCBoxOSD	OSD;
};

extern struct SSFCBox	SFCBox;

// Loader side (memmap.cpp calls these)
bool8	S9xSFCBoxLoadKROM (void);		// KROM (+MB90082.BIN) from their BIOS Manager slots

// KROM versions the BIOS Manager's KROM file holds: bit0 = 1.00, bit1 = 2.00.
uint8	S9xSFCBoxKROMVersions (void);
// Settings.SFCBoxKROMVersion if the file holds it, else what it does hold;
// writes the answer back so the menu shows the version that will boot.
uint8	S9xSFCBoxResolveKROMVersion (void);
void	S9xSFCBoxPowerOn (void);		// full board reset; SNES ends up held
void	S9xSFCBoxDeactivate (void);

// Main-loop side
void	S9xSFCBoxEndScanline (void);	// run the Z180 slice + board timers
bool8	S9xSFCBoxSNESHeld (void);
bool8	S9xSFCBoxPendingReset (void);
void	S9xSFCBoxApplySNESReset (void);	// performs the deferred SNES soft reset

// SNES-visible hardware
void	S9xSFCBoxSetWRIO (uint8 byte);
uint8	S9xSFCBoxGetRDIO (void);
bool8	S9xSFCBoxJoypadOverride (int pad, uint16 *value);
void	S9xSFCBoxJoypadAccessed (void);
uint8	S9xGetSFCBoxSRAM (uint32 address);
void	S9xSetSFCBoxSRAM (uint8 byte, uint32 address);
bool8	S9xSFCBoxOSDHires (void);		// character plane visible: double lores frames first
void	S9xSFCBoxRenderOSD (uint16 *screen, int pitch, int width, int height);

// Front panel
enum { SFCBOX_RESET_SWITCH, SFCBOX_TVGAME_SWITCH };
void	S9xSFCBoxInsertCoin (void);
void	S9xSFCBoxPressSwitch (int sw);	// one press of a front-panel push switch
bool8	S9xSFCBoxTVMode (void);			// TV indicator lit: RF out carries the antenna, not the SNES

// "Super Famicom Box - <game>", or just "Super Famicom Box" on the menu
const char *S9xSFCBoxTitle (void);
// The games in a slot, "Super Mario Kart / Mario Collection / Star Fox"; "" when empty
const char *S9xSFCBoxSlotName (int slot);

// Battery-backed KROM work RAM (+ RTC SRAM pages), "<rom>.box" sidecar
bool8	S9xSFCBoxLoadNVRAM (void);
bool8	S9xSFCBoxSaveNVRAM (void);

// Savestates: opaque versioned blob (Z180 + supervisor board), embedded in
// the snapshot as the "BOX" block. PostLoadState rebuilds the SNES map from
// the restored mapping registers — it must run BEFORE the FillRAM and GSU
// restores (its FxReset clears the GSU register space).
size_t	S9xSFCBoxStateSize (void);
void	S9xSFCBoxStateSave (uint8 *buf);
bool8	S9xSFCBoxStateLoad (const uint8 *buf, size_t size);
bool8	S9xSFCBoxStateMatchesKROM (const uint8 *buf, size_t size);	// taken on the running KROM version?
void	S9xSFCBoxPostLoadState (void);

// Rebuilds the SNES memory map from MapReg0/MapReg1 (memmap.cpp)
void	S9xSFCBoxRemap (void);

// Cartridge images, each in the merged layout (GROM, ROMs, DSP-1 space).
bool8	S9xSFCBoxValidGROM (const uint8 *grom, uint32 avail);
uint32	S9xSFCBoxCartLength (const uint8 *image, uint32 avail);	// 0 = not a cart
bool8	S9xSFCBoxIsMainCart (const uint8 *grom);	// the PSS-61, which slot 1 needs
// A MAME set (.zip): 1 = assembled into `out`, 0 = not a set, -1 = set with ROMs missing.
int		S9xSFCBoxReadZipSet (const char *path, std::vector<uint8> &out);
// The carts in a MAME set or a merged image, in file order: 1 = found, 0 = none, -1 = broken set.
int		S9xSFCBoxReadCarts (const char *path, std::vector<uint8> carts[2]);

// A slot (0 = main, 1 = optional) chosen from the menu for the box loaded
// from `base`: a cart file, or "" for an empty slot. Read by the next load of
// `base`; a load of any other file clears both.
void	S9xSFCBoxSetSlot (int slot, const char *base, const char *path);
bool8	S9xSFCBoxSlotOverride (int slot, const char *base, std::string &path);

#endif
