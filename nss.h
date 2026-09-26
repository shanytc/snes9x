/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Nintendo Super System (NSS-01-CPU) supervisor board. An otherwise stock
// NTSC SNES with a 4MHz Z80 next to it that owns the coin slots, the timer
// and the menu: it holds the 65816 in reset until a game is paid for, drives
// an M50458 on-screen display superimposed over the SNES picture, and checks
// the cartridge's RP5H01 key chip before it will run anything.
//
// This file wraps the Z80 core (z80.cpp) with that board: ports 00h-07h,
// the 8K work RAM with its write-protect window, the M6M80011 settings
// EEPROM, the S-3520 clock, the RP5H01 and the OSD. The cartridge loader
// (image layout, MAME set assembly) lives in memmap.cpp beside it.
// See docs/nss.md.

#ifndef _NSS_H_
#define _NSS_H_

#include "port.h"
#include <string>
#include <vector>

#define NSS_BIOS_SIZE		0x8000
#define NSS_WRAM_SIZE		0x2000
#define NSS_BACKUP_BASE		0x1000	// 9000h-9FFFh: battery-backed, holds the bookkeeping
#define NSS_BACKUP_SIZE		0x1000
#define NSS_INST_SIZE		0x8000	// whole EPROM; only the top 8K is wired
#define NSS_INST_WINDOW		0x2000	// C000h-DFFFh
#define NSS_PROM_SIZE		0x10
#define NSS_EEPROM_WORDS	0x40	// M6M80011: 64 x 16 bits
#define NSS_RTC_NVRAM		15		// S-3520 SRAM nibbles, packed two per byte

#define NSS_OSD_W			24
#define NSS_OSD_H			12
#define NSS_OSD_CELLS		(NSS_OSD_W * NSS_OSD_H)
#define NSS_OSD_REGS		8
#define NSS_FONT_SIZE		0x1200	// 128 glyphs x 18 rows x 2 bytes

#define NSS_PHI				4000000	// Z84C0006 clock

#define NSS_SLOTS			3		// cartridge connectors CN11/12/13
#define NSS_SLOT_SRAM		0x8000	// biggest battery any NSS cart carries
#define NSS_SLOT_NAME		32
#define NSS_SLOT_PATH		512

// Front-panel and coin-door inputs, as a bit set the ports read straight out
// of. These are momentary switches: the UI raises a bit while its key or
// button is down, and the coin inputs are pulsed through NSS.CoinPulse.
enum
{
	NSS_BTN_GAME1		= 0x0001,
	NSS_BTN_GAME2		= 0x0002,
	NSS_BTN_GAME3		= 0x0004,
	NSS_BTN_INSTRUCTIONS= 0x0008,
	NSS_BTN_PAGEDOWN	= 0x0010,
	NSS_BTN_PAGEUP		= 0x0020,
	NSS_BTN_RESTART		= 0x0040,
	NSS_BTN_SERVICE		= 0x0100
};

// One cartridge connector. Everything a slot holds is per-socket: the SNES
// program, the Z80 instruction EPROM, the key chip and the battery.
struct SNSSSlot
{
	uint8	Present;
	uint8	*Prg;					// the SNES program, heap-owned
	uint32	PrgSize;
	uint8	ROMSizeByte;			// cart header [7FD7h]
	uint8	SRAMSizeByte;			// cart header [7FD8h]
	uint8	INST[NSS_INST_SIZE];
	uint8	PROM[NSS_PROM_SIZE];
	uint8	PROMPresent;
	uint8	SRAM[NSS_SLOT_SRAM];
	uint8	SRAMValid;				// always set now; kept for the savestate layout
	uint32	CRC;
	char	Name[NSS_SLOT_NAME];	// cart header title, for the menu
	char	Path[NSS_SLOT_PATH];
	uint8	HiROM;					// retail cart on an adapter: its own map
	uint8	DSP1;					// ... and its DSP-1
};

struct SNSSPROM			// Ricoh RP5H01 72-bit key chip, live pin state
{
	uint8	Counter;				// 6/7-bit address counter
	uint8	SevenBit;				// test pin: 1 = 7-bit addressing
	uint8	LastClock;
	uint8	Reset;					// reset pin held, counter pinned at zero
};

struct SNSSEEPROM		// Mitsubishi M6M80011, the coinage/settings store
{
	uint16	Data[NSS_EEPROM_WORDS];
	uint8	CS, Clock, DataIn, DataOut;
	uint8	State;			// 0 = awaiting command, else the command in progress
	uint8	BitPos;
	uint32	Shift;
	uint8	Addr;
	uint8	WriteEnable;
	uint8	Dirty;
};

struct SNSSRTC			// Seiko Instruments S-3520CF (MAME: "Seiko Epson")
{
	uint8	CS, Clock, Dir, DataIn, DataOut;
	uint8	Shift;
	uint8	BitPos;
	uint8	Mode;			// register F low bits: 0/1 = clock, 2/3 = SRAM pages
	uint8	Control1, Control2;
	uint8	NVRAM[NSS_RTC_NVRAM];

	// BCD, as the chip stores them
	uint8	Sec, Min, Hour, Day, Month, Year, Weekday;
	int32	FrameAccum;
};

struct SNSSOSD			// Mitsubishi M50458-001SP
{
	uint8	CS, Clock, DataIn;
	uint8	BitPos;
	uint16	Shift;
	uint8	HaveAddr;		// the first word after /CS is the address
	uint16	Addr;

	uint16	VRAM[NSS_OSD_CELLS];
	uint16	Reg[NSS_OSD_REGS];

	uint8	FontLoaded;
	uint8	Font[NSS_FONT_SIZE];
	uint32	BlinkFrame;
};

struct SNSS
{
	bool8	Active;

	// Board memories
	uint8	BIOS[NSS_BIOS_SIZE];
	uint8	WRAM[NSS_WRAM_SIZE];
	uint8	BIOSRevision;			// 0 = unknown, 2 = "02", 3 = "03"

	struct SNSSSlot	Slot[NSS_SLOTS];
	int8	MappedSlot;				// which one the SNES side currently sees, -1 none

	// Latched port outputs
	uint8	Port00W, Port01W, Port03W, Port04W;

	// Derived from Port01W, kept apart because the SNES side reads them hot
	uint8	SlotSelect;
	bool8	SNESHeld;				// reset or halt line asserted
	bool8	PendingSNESReset;		// reset line rose: reboot at a safe point
	bool8	PendingAPUReset;		// reset line fell: silence the APU with it
	bool8	InputDisabled;			// joypads unplugged (demo mode)
	bool8	SoundMuted;

	// SNES-side state the Z80 samples
	uint8	GameOverFlag;			// $4016 write bit2
	uint8	JoyReadFlag;			// 1 = the game has not polled the pads
	uint8	DipSwitches;			// cartridge DIP block, read at $4100

	// Inputs
	uint16	Buttons;				// NSS_BTN_*
	uint16	PulseButtons;			// tapped from the UI, released by PulseLeft
	int32	PulseLeft;
	int32	CoinPulse[2];			// frames of switch closure left

	uint8	NMIEnable, WRAMUnlock;
	int32	LastVCounter;
	int64	CycleRemainder;

	struct SNSSPROM		PROM;
	struct SNSSEEPROM	EEPROM;
	struct SNSSRTC		RTC;
	struct SNSSOSD		OSD;
};

extern struct SNSS	NSS;

// Loader side (memmap.cpp calls these)
bool8	S9xNSSLoadBIOS (void);			// BIOS + OSD charset from their BIOS Manager slots
void	S9xNSSPowerOn (void);			// full board reset; the SNES ends up held
void	S9xNSSDeactivate (void);

// Cartridge slots. Slot 1 is filled by the ordinary File -> Load Game; the
// other two take a cart the way the cabinet does, by putting one in the
// socket, which reboots the machine so the supervisor rescans.
bool8	S9xNSSInsertCart (int slot, const char *path);
void	S9xNSSEjectCart (int slot);
// The cabinet always holds at least one cartridge, so the last one stays.
bool8	S9xNSSCanEject (int slot);
// Splits a merged image straight into a socket (the loader fills slot 1).
bool8	S9xNSSLoadSlot (int slot, const uint8 *image, uint32 size, const char *path);
// Puts a socket's cartridge under the SNES: program, size, battery and map.
void	S9xNSSMapSlot (int slot);
// The mapped socket's battery is live in Memory.SRAM, not in its own buffer.
// Anything that moves or drops a cartridge takes a copy first.
void	S9xNSSStashMappedSRAM (void);
bool8	S9xNSSSlotPresent (int slot);
// True while a paid game is actually running — the supervisor has let the
// 65816 go and plugged the pads back in. Its menu is not up, so the game
// buttons do nothing until the game ends or Restart abandons it.
bool8	S9xNSSGameRunning (void);
const char *S9xNSSSlotName (int slot);
const char *S9xNSSSlotPath (int slot);
// Which slot's cartridge the SNES side is mapped to, or -1.
int		S9xNSSMappedSlot (void);

// A MAME-style cartridge set (prg chips + instruction EPROM + security.prm)
// flattened into the fullsnes merged layout. Returns 0 when `path` is not one
// (and leaves `dest` alone), or NSS_ZIPSET_UNREADABLE when it is one but could
// not be read — by then `dest` holds a half-written cartridge, so the caller
// has to fail the load rather than fall through.
#define NSS_ZIPSET_UNREADABLE	0xffffffffu
uint32	S9xNSSAssembleZipSet (const char *path, uint8 *dest, uint32 maxsize);
// Splits a merged image: keeps the instruction EPROM and key, hands back the
// SNES program size so the ordinary cart loader can take it from there.
bool8	S9xNSSTakeCartTail (const uint8 *image, uint32 size, uint32 *prg_size);
// Reads a cartridge image from disk, either shape, into `out`.
bool8	S9xNSSReadCartImage (const char *path, std::vector<uint8> &out);

// Main-loop side
void	S9xNSSEndScanline (void);
bool8	S9xNSSSNESHeld (void);
bool8	S9xNSSPendingReset (void);
void	S9xNSSApplySNESReset (void);
// The APU sits on the same reset line as the 65816, so it stops when the
// supervisor pulls a game rather than when it lets the next one go.
bool8	S9xNSSPendingAPUReset (void);
void	S9xNSSApplyAPUReset (void);

// SNES-visible hardware
uint8	S9xNSSReadDIP (void);			// $4100
// What the cartridge in play does with DIP switch `sw` (0-7) in its current
// position, e.g. "Lives: 5" or "Unused"; "" for a cartridge we have no
// table for, NULL when its board has no switch block at all.
const char *S9xNSSDipSwitchLabel (int sw);
void	S9xNSSSetJoypadStrobe (uint8 byte);	// $4016 write
void	S9xNSSJoypadRead (void);		// the game polled the pads
bool8	S9xNSSInputDisabled (void);		// pads unplugged: reads come back empty

// OSD overlay
bool8	S9xNSSOSDHires (void);
void	S9xNSSRenderOSD (uint16 *screen, int pitch, int width, int height);

// How long a tapped button is held down for. The supervisor polls the panel
// once a frame, so a few frames is plenty for most of them — but it debounces
// Restart for about half a second before it will abandon a paid game, and a
// tap that does not outlast that is simply ignored.
#define NSS_BTN_TAP_FRAMES	10
#define NSS_BTN_HOLD_FRAMES	40

// Front panel. SetButtons holds a set of NSS_BTN_* down; PulseButton taps
// one, which is what a menu entry or a hotkey can offer.
void	S9xNSSInsertCoin (int slot);
void	S9xNSSSetButtons (uint16 mask);
void	S9xNSSPulseButton (uint16 mask);
uint16	S9xNSSGetButtons (void);

// The cabinet's EEPROM, clock NVRAM and bookkeeping: one "NSS.srm" in the save folder
bool8	S9xNSSLoadNVRAM (void);
bool8	S9xNSSSaveNVRAM (void);
// Every cartridge's battery, each in "<its own file>.srm"
bool8	S9xNSSLoadBatteries (void);
bool8	S9xNSSSaveBatteries (void);

// Savestates: opaque versioned blob embedded as the snapshot's "NSS" block
size_t	S9xNSSStateSize (void);
void	S9xNSSStateSave (uint8 *buf);
bool8	S9xNSSStateLoad (const uint8 *buf, size_t size);

#endif
