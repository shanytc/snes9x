/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _WIDESCREEN_H_
#define _WIDESCREEN_H_

#include "port.h"

// Widescreen
//
// The SNES scans out 256 columns and the renderer has always stopped there.
// Nothing in the PPU actually needs it to: the tilemaps are wider than the
// screen, the Mode 7 matrix is defined everywhere, and object coordinates are
// 9 bits. So the renderer can simply keep walking past both edges and draw
// what the hardware had in hand but never showed.
//
// What it cannot invent is a game that expects the wider view - scroll
// clamps, sprites culled off the old edges, HUDs windowed to x=0..255. That
// part is the ROM's job, which is why widescreen ROM hacks exist (Vitor
// Vilela's Super Mario World Widescreen being the one this was built
// against). widescreen.cpp keeps the hacks we know: the retail game each one
// patches, the patch itself, and the settings its author made it for, in the
// values bsnes-hd defined so a hack's published settings carry over as they
// are. A hack that comes in several widths is several rows with one retail
// image, and the user picks the width.
//
// Loading the retail game offers the choice. Picking a width patches the
// cart in memory into that hack and runs its settings; picking none puts
// the retail cart back. Any other game is left alone: its extra columns
// would be genuine PPU output, but everything it culled or clamped at the
// old edges would show.

enum
{
	WS_MODE_OFF		= 0,	// never widen
	WS_MODE_ON		= 1,	// widen every scene
	WS_MODE_MODE7	= 2		// widen only while a Mode 7 background is up
};

enum
{
	WS_OBJ_SAFE		= 0,	// only objects that reach the classic 256 columns
	WS_OBJ_UNSAFE	= 1,	// objects living entirely in a side column too
	WS_OBJ_CLIP		= 2,	// clip every object to the classic 256 columns
	WS_OBJ_DISABLE	= 3		// draw no objects at all
};

// Per-background settings, in bsnes-hd's values.
enum
{
	WS_BG_OFF		= 0,	// background stays inside the classic 256 columns
	WS_BG_ON		= 1,
	WS_BG_AUTO_HV	= 2,	// off when it looks like a HUD (see S9xWideBGLine)
	WS_BG_AUTO_H	= 3,
	WS_BG_CROP		= 10,	// drop the 8 columns next to each old edge
	WS_BG_CROP_AUTO	= 11,
	WS_BG_DISABLE	= 20,	// do not draw this background at all
	WS_BG_ABOVE		= 1000,	// + scanline: widen only above that line
	WS_BG_BELOW		= 2000	// + scanline: widen only from that line down
};

enum
{
	WS_WINDOW_NORMAL		= 0,	// window positions apply as written
	WS_WINDOW_OUTSIDE		= 1,	// "outside" windows read one fixed column
	WS_WINDOW_OUTSIDE_ALWAYS	= 2,
	WS_WINDOW_ALL			= 3
};

struct SWidescreen
{
	uint8	Mode;			// WS_MODE_*
	uint16	Aspect;			// <= 200: columns per side; above: h * 100 + v
	uint8	Sprites;		// WS_OBJ_*
	uint16	BG[4];			// WS_BG_*
	bool8	StretchWindow;	// double every window position's distance from 128
	uint8	IgnoreWindow;	// WS_WINDOW_*
	uint8	IgnoreWindowX;	// the column "ignore window" reads instead
	bool8	Backdrop;		// side columns take the backdrop colour, not black
	bool8	Overscan;		// both only feed the aspect ratio -> columns math
	bool8	AspectCorrection;
};

// The settings in force: the user's choice over the loaded hack's row.
// Everything outside the options UI reads this one. In Settings.Widescreen,
// Mode is the switch and Aspect the width picked, as columns on each side.
extern struct SWidescreen	Widescreen;

void S9xSetWidescreenDefaults (struct SWidescreen *ws);
// Re-derive the effective settings from Settings.Widescreen and the loaded
// cart; call after changing any of the user's widescreen settings.
void S9xUpdateWidescreen (void);
// Columns added on each side by the current settings, 0 when off.
int  S9xWidescreenColumns (void);
// Whether the side columns take the backdrop colour rather than staying black.
bool S9xWideBackdropFills (void);

// A byte run in the patched image that this emulator needs changed: `Old` is
// checked before `New` goes in, so a mismatched image is left alone.
struct SWidescreenEdit
{
	uint32	Offset;
	uint8	Len;			// up to 4
	uint8	Old[4];
	uint8	New[4];
};

// A widescreen ROM hack we know, at one width: the retail image it patches,
// the patch, and the settings it was made for.
struct SWidescreenGame
{
	uint32		SourceCRC32;	// the retail image, as CMemory::ROMCRC32 has it
	uint32		TargetCRC32;	// the image the patch makes of it
	const char	*Title;			// header title of both, as CMemory::ROMName trims it
	const char	*Name;			// the width, as offered to the user
	const uint8	*Patch;			// BPS, applied to the retail image in memory
	uint32		PatchSize;
	const struct SWidescreenEdit	*Edits;	// applied to the patched image, if any
	uint8		EditCount;
	uint8		Mode;			// WS_MODE_ON or WS_MODE_MODE7
	uint16		Aspect;			// columns on each side; the row's identity too
	uint8		Sprites;		// WS_OBJ_*
	uint16		BG[4];			// WS_BG_*
	bool8		StretchWindow;
	uint8		IgnoreWindow;	// WS_WINDOW_*
	uint8		IgnoreWindowX;
};

// The row whose retail or patched image this is, NULL for any other image.
// A NULL title matches on the CRC alone; a non-zero column count picks that
// width among the rows of one retail image, and 0 takes the first.
const struct SWidescreenGame *S9xFindWidescreenGame (uint32 crc32, const char *title, uint16 columns);
// Note the loaded cart: its row, if any, and whether the load path patched it
// (CMemory::InitROM). Only a cart with a row is offered the choice.
void S9xSetWidescreenGame (uint32 crc32, const char *title, bool8 patched_here);
const struct SWidescreenGame *S9xWidescreenGame (void);
// Whether the image in memory is a hack rather than the retail game.
bool S9xWidescreenPatched (void);
// The widths on offer for the loaded cart: every row of its retail image,
// in table order. Returns how many were written.
int  S9xWidescreenVariants (const struct SWidescreenGame **rows, int max);
// Put the loaded hack's edits into the image; false if any old byte differs.
bool S9xApplyWidescreenGameEdits (uint8 *rom, uint32 size);
// Whether the user's choice now disagrees with the cart in memory. The port
// then loads the game again, and the load path patches it or leaves it be.
bool S9xWidescreenReloadNeeded (void);

#endif
