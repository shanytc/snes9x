/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _WIDESCREEN_H_
#define _WIDESCREEN_H_

#include "port.h"
#include <string>

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
// against). They ship a .bso file naming the settings they were made for,
// so we read the same file, with the same letters and values, that bsnes-hd
// defined and those hacks are distributed with.
//
// Games without a hack still render - the extra columns are genuine PPU
// output - but anything the game culled or clamped at the old edges shows as
// artifacts, which is what the per-background and object settings are for.

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

// Per-background settings, in the encoding .bso files use.
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

// The settings in force: the user's, with any .bso override for this game
// applied on top. Everything outside the options UI reads this one.
extern struct SWidescreen	Widescreen;

void S9xSetWidescreenDefaults (struct SWidescreen *ws);
// Re-derive the effective settings from Settings.Widescreen and the override
// text; call after changing any of the user's widescreen settings.
void S9xUpdateWidescreen (void);
// Hand over the contents of a .bso file (empty string to drop the override).
void S9xSetWidescreenOverride (const std::string &text);
// Columns added on each side by the current settings, 0 when off.
int  S9xWidescreenColumns (void);
// Whether the side columns take the backdrop colour rather than staying black.
bool S9xWideBackdropFills (void);

#endif
