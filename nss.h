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

#define NSS_BIOS_SIZE		0x8000
#define NSS_WRAM_SIZE		0x2000
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

struct SNSSPROM			// Ricoh RP5H01 72-bit key chip
{
	uint8	Data[NSS_PROM_SIZE];	// as dumped: a set bit reads back as "low"
	uint8	Present;
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

struct SNSSRTC			// Seiko Epson S-3520
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
	uint8	INST[NSS_INST_SIZE];	// the selected slot's instruction EPROM
	uint8	BIOSRevision;			// 0 = unknown, 2 = "02", 3 = "03"

	// Latched port outputs
	uint8	Port00W, Port01W, Port03W, Port04W;

	// Derived from Port01W, kept apart because the SNES side reads them hot
	uint8	SlotSelect;
	bool8	SNESHeld;				// reset or halt line asserted
	bool8	PendingSNESReset;		// reset line rose: reboot at a safe point
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

// Main-loop side
void	S9xNSSEndScanline (void);
bool8	S9xNSSSNESHeld (void);
bool8	S9xNSSPendingReset (void);
void	S9xNSSApplySNESReset (void);

// SNES-visible hardware
uint8	S9xNSSReadDIP (void);			// $4100
void	S9xNSSSetJoypadStrobe (uint8 byte);	// $4016 write
void	S9xNSSJoypadRead (void);		// the game polled the pads
bool8	S9xNSSInputDisabled (void);		// pads unplugged: reads come back empty

// OSD overlay
bool8	S9xNSSOSDHires (void);
void	S9xNSSRenderOSD (uint16 *screen, int pitch, int width, int height);

// Front panel. SetButtons holds a set of NSS_BTN_* down; PulseButton taps
// one for a few frames, which is what a menu entry or a hotkey can offer.
void	S9xNSSInsertCoin (int slot);
void	S9xNSSSetButtons (uint16 mask);
void	S9xNSSPulseButton (uint16 mask);
uint16	S9xNSSGetButtons (void);

// Battery-backed settings EEPROM + clock NVRAM, "<rom>.nss" beside the .srm
bool8	S9xNSSLoadNVRAM (void);
bool8	S9xNSSSaveNVRAM (void);

// Savestates: opaque versioned blob embedded as the snapshot's "NSS" block
size_t	S9xNSSStateSize (void);
void	S9xNSSStateSave (uint8 *buf);
bool8	S9xNSSStateLoad (const uint8 *buf, size_t size);

#endif
