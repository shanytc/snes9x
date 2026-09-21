/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"

// --- Window latches ---------------------------------------------------------
//
// The PPU does not compare every dot against the current X1/X2. It sets one
// latch when the dot counter reaches X1 and another when it reaches X2, and
// the second one ends the window for the rest of the line - so moving the
// positions after a window has stopped cannot restart it, and moving X1 behind
// the beam never opens it at all. However many times a line rewrites the
// registers, it shows at most one span per window.

static struct
{
	int32	line;          // scanline this state belongs to
	uint32	frame;         // and the frame, so a wrapped V counter cannot
	                       // hand last frame's line its state back
	int32	dot;           // dots already swept
	int16	start[2];      // dot X1 was matched at, -1 = not yet
	int16	stop[2];       // dot X2 was matched at, -1 = not yet
}	window_latch = { -1, 0, 0, { -1, -1 }, { -1, -1 } };

static inline bool WindowLatchIsCurrent (void)
{
	return (window_latch.line  == CPU.V_Counter &&
	        window_latch.frame == IPPU.TotalEmulatedFrames);
}

// The dot the beam has reached. Pixel 0 of the line is dot 22 of the H
// counter, the same origin the mid-line raster events use.
static inline int32 WindowBeamDot (void)
{
	const int32	x = CPU.Cycles / ONE_DOT_CYCLE - 22;

	return (x < 0) ? 0 : (x > 256 ? 256 : x);
}

// Sweep [from, to) with the positions the registers hold now. The two latches
// are independent, so a window whose X2 comes first is simply never open.
static void SweepWindowLatches (int16 *start, int16 *stop, int32 from, int32 to)
{
	const uint8	x1[2] = { PPU.Window1Left,  PPU.Window2Left  };
	const uint8	x2[2] = { PPU.Window1Right, PPU.Window2Right };

	for (int w = 0; w < 2; w++)
	{
		if (start[w] < 0 && x1[w] >= from && x1[w] < to)
			start[w] = x1[w];
		if (stop[w]  < 0 && x2[w] >= from && x2[w] < to)
			stop[w]  = x2[w];
	}
}

// Called before a window position register changes, so the dots the beam has
// already passed are latched against the value that was in force for them.
// Only a write inside a visible line has dots behind it: one in VBlank or
// past HBlank is setting the positions up for lines that have not started,
// and those lines latch from dot 0 like any other.
void S9xLatchWindowSpans (void)
{
	if (CPU.V_Counter < FIRST_VISIBLE_LINE ||
	    CPU.V_Counter >= PPU.ScreenHeight + FIRST_VISIBLE_LINE)
	{
		window_latch.line = -1;
		return;
	}

	if (!WindowLatchIsCurrent())
	{
		window_latch.line     = CPU.V_Counter;
		window_latch.frame    = IPPU.TotalEmulatedFrames;
		window_latch.dot      = 0;
		window_latch.start[0] = window_latch.start[1] = -1;
		window_latch.stop[0]  = window_latch.stop[1]  = -1;
	}

	const int32	dot = WindowBeamDot();

	if (dot > window_latch.dot)
	{
		SweepWindowLatches(window_latch.start, window_latch.stop, window_latch.dot, dot);
		window_latch.dot = dot;
	}
}

// The spans this line actually shows: what the latches already hold, plus the
// rest of the line swept with the positions in force now. A line nothing wrote
// mid-way reduces to the register pair itself.
static void EffectiveWindows (uint8 *left, uint8 *right)
{
	int16	start[2], stop[2];
	int32	from = 0;

	if (WindowLatchIsCurrent())
	{
		start[0] = window_latch.start[0];	start[1] = window_latch.start[1];
		stop[0]  = window_latch.stop[0];	stop[1]  = window_latch.stop[1];
		from     = window_latch.dot;
	}
	else
		start[0] = start[1] = stop[0] = stop[1] = -1;

	SweepWindowLatches(start, stop, from, 256);

	for (int w = 0; w < 2; w++)
	{
		if (start[w] < 0 || (stop[w] >= 0 && stop[w] < start[w]))
		{
			// Never opened: left > right is how the region builder reads "off".
			left[w]  = 1;
			right[w] = 0;
		}
		else
		{
			left[w]  = (uint8) start[w];
			right[w] = (uint8) (stop[w] >= 0 ? stop[w] : 255);
		}
	}
}

// The spans S9xComputeClipWindows is working from, for the helpers it calls.
static uint8	WinLeft[2], WinRight[2];

// --- Widescreen -------------------------------------------------------------
//
// Window positions are register values in the SNES's own 256 columns, and the
// clip regions the renderer walks are in the widened picture's. Everything
// above S9xComputeClipWindows still works in the former; the boundaries are
// translated once, on the way out.

// A window that already ran to a screen edge keeps running to the new one -
// that is what carries a HUD's black bars across the side columns instead of
// stopping them at the old edge. A ROM hack that adapted its own window
// positions asks for "stretch" instead, where every position's distance from
// the centre column is doubled.
static void WideBoundaries (const int16 *windows, int n_regions, int16 *out)
{
	const int	ext   = IPPU.WideExtent;
	const int	width = SNES_WIDTH + 2 * ext;

	for (int i = 0; i <= n_regions; i++)
	{
		int	v = windows[i];

		if (!ext)
			;
		else
		if (v <= 0)
			v = 0;
		else
		if (v >= SNES_WIDTH)
			v = width;
		else
		if (Widescreen.StretchWindow)
		{
			// Regions are bounded by "one past the window's right edge", but
			// what gets stretched is the register value itself, so a right
			// edge is doubled from v - 1 and the +1 put back afterwards.
			const bool	w1 = WinLeft[0] <= WinRight[0];
			const bool	w2 = WinLeft[1] <= WinRight[1];
			const bool	left  = (w1 && v == WinLeft[0]) || (w2 && v == WinLeft[1]);
			const bool	right = !left &&
							    ((w1 && v == WinRight[0] + 1) || (w2 && v == WinRight[1] + 1));

			v = 2 * (right ? v - 1 : v) - 128 + ext + (right ? 1 : 0);

			if (v < 0)     v = 0;
			if (v > width) v = width;
		}
		else
			v += ext;

		out[i] = (int16) v;
	}
}

// bsnes-hd's "ignore window": with the colour window set a certain way, every
// column reads the window state of one fixed column instead of its own, which
// is what lets scenes whose windows would otherwise black out the side
// columns widen at all.
static bool WideIgnoreWindow (bool8 sub)
{
	if (!IPPU.WideExtent || Widescreen.IgnoreWindow == WS_WINDOW_NORMAL)
		return (false);

	const uint8	mask = (Memory.FillRAM[0x2130] >> (sub ? 4 : 6)) & 3;

	return (Widescreen.IgnoreWindow >= WS_WINDOW_ALL ||
			(Widescreen.IgnoreWindow >= WS_WINDOW_OUTSIDE_ALWAYS && mask == 0) ||
			(Widescreen.IgnoreWindow >= WS_WINDOW_OUTSIDE && mask == 2));
}

// Which of the regions the fixed column falls in, in SNES coordinates.
static int WideIgnoreRegion (const int16 *windows, int n_regions)
{
	for (int i = n_regions - 1; i > 0; i--)
		if (Widescreen.IgnoreWindowX >= windows[i])
			return (i);

	return (0);
}

static uint8	region_map[6][6] =
{
	{ 0, 0x01, 0x03, 0x07, 0x0f, 0x1f },
	{ 0,    0, 0x02, 0x06, 0x0e, 0x1e },
	{ 0,    0,    0, 0x04, 0x0c, 0x1c },
	{ 0,    0,    0,    0, 0x08, 0x18 },
	{ 0,    0,    0,    0,    0, 0x10 }
};

static inline uint8 CalcWindowMask (int, uint8, uint8);
static inline void StoreWindowRegions (uint8, struct ClipData *, int, int16 *, uint8 *, bool8, bool8 s = FALSE, int ignore_region = -1);


static inline uint8 CalcWindowMask (int i, uint8 W1, uint8 W2)
{
	if (!PPU.ClipWindow1Enable[i])
	{
		if (!PPU.ClipWindow2Enable[i])
			return (0);
		else
		{
			if (!PPU.ClipWindow2Inside[i])
				return (~W2);
			return (W2);
		}
	}
	else
	{
		if (!PPU.ClipWindow2Enable[i])
		{
			if (!PPU.ClipWindow1Inside[i])
				return (~W1);
			return (W1);
		}
		else
		{
			if (!PPU.ClipWindow1Inside[i])
				W1 = ~W1;
			if (!PPU.ClipWindow2Inside[i])
				W2 = ~W2;

			switch (PPU.ClipWindowOverlapLogic[i])
			{
				case 0: // OR
					return (W1 | W2);

				case 1: // AND
					return (W1 & W2);

				case 2: // XOR
					return (W1 ^ W2);

				case 3: // XNOR
					return (~(W1 ^ W2));
			}
		}
	}

	// Never get here
	return (0);
}

static inline void StoreWindowRegions (uint8 Mask, struct ClipData *Clip, int n_regions, int16 *windows, uint8 *drawing_modes, bool8 sub, bool8 StoreMode0, int ignore_region)
{
	int	ct = 0;

	// "Ignore window": one region's state stands for the whole line.
	if (ignore_region >= 0)
	{
		int	DrawMode = drawing_modes[ignore_region];
		if (sub)
			DrawMode |= 1;
		if (Mask & (1 << ignore_region))
			DrawMode = 0;

		if (StoreMode0 || DrawMode)
		{
			Clip->Left[0]     = windows[0];
			Clip->Right[0]    = windows[n_regions];
			Clip->DrawMode[0] = DrawMode;
			ct = 1;
		}

		Clip->Count = ct;
		return;
	}

	for (int j = 0; j < n_regions; j++)
	{
		int	DrawMode = drawing_modes[j];
		if (sub)
			DrawMode |= 1;
		if (Mask & (1 << j))
			DrawMode = 0;

		if (!StoreMode0 && !DrawMode)
			continue;

		// Stretched window positions can collapse a region against a screen
		// edge; an empty one would hand the renderer a zero-width span.
		if (windows[j] >= windows[j + 1])
			continue;

		if (ct > 0 && Clip->Right[ct - 1] == windows[j] && Clip->DrawMode[ct - 1] == DrawMode)
			Clip->Right[ct - 1] = windows[j + 1]; // This region borders with and has the same drawing mode as the previous region: merge them.
		else
		{
			// Add a new region to the BG
			Clip->Left[ct]     = windows[j];
			Clip->Right[ct]    = windows[j + 1];
			Clip->DrawMode[ct] = DrawMode;
			ct++;
		}
	}

	Clip->Count = ct;
}

void S9xComputeClipWindows (void)
{
	int16	windows[6] = { 0, 256, 256, 256, 256, 256 };
	uint8	drawing_modes[5] = { 0, 0, 0, 0, 0 };
	int		n_regions = 1;
	int		i, j;

	// The spans the line's latches leave, which is the register pair itself
	// unless something moved the positions mid-line.

	EffectiveWindows(WinLeft, WinRight);

	// Calculate window regions. We have at most 5 regions, because we have 6 control points
	// (screen edges, window 1 left & right, and window 2 left & right).

	if (WinLeft[0] <= WinRight[0])
	{
		if (WinLeft[0] > 0)
		{
			windows[2] = 256;
			windows[1] = WinLeft[0];
			n_regions = 2;
		}

		if (WinRight[0] < 255)
		{
			windows[n_regions + 1] = 256;
			windows[n_regions] = WinRight[0] + 1;
			n_regions++;
		}
	}

	if (WinLeft[1] <= WinRight[1])
	{
		for (i = 0; i <= n_regions; i++)
		{
			if (WinLeft[1] == windows[i])
				break;

			if (WinLeft[1] <  windows[i])
			{
				for (j = n_regions; j >= i; j--)
					windows[j + 1] = windows[j];

				windows[i] = WinLeft[1];
				n_regions++;
				break;
			}
		}

		for (; i <= n_regions; i++)
		{
			if (WinRight[1] + 1 == windows[i])
				break;

			if (WinRight[1] + 1 <  windows[i])
			{
				for (j = n_regions; j >= i; j--)
					windows[j + 1] = windows[j];

				windows[i] = WinRight[1] + 1;
				n_regions++;
				break;
			}
		}
	}

	// Get a bitmap of which regions correspond to each window.

	uint8	W1, W2;

	if (WinLeft[0] <= WinRight[0])
	{
		for (i = 0; windows[i] != WinLeft[0]; i++) ;
		for (j = i; windows[j] != WinRight[0] + 1; j++) ;
		W1 = region_map[i][j];
	}
	else
		W1 = 0;

	if (WinLeft[1] <= WinRight[1])
	{
		for (i = 0; windows[i] != WinLeft[1]; i++) ;
		for (j = i; windows[j] != WinRight[1] + 1; j++) ;
		W2 = region_map[i][j];
	}
	else
		W2 = 0;

	// Color Window affects the drawing mode for each region.
	// Modes are: 3=Draw as normal, 2=clip color (math only), 1=no math (draw only), 0=nothing.

	uint8	CW_color = 0, CW_math = 0;
	uint8	CW = CalcWindowMask(5, W1, W2);

	switch (Memory.FillRAM[0x2130] & 0xc0)
	{
		case 0x00:	CW_color = 0;		break;
		case 0x40:	CW_color = ~CW;		break;
		case 0x80:	CW_color = CW;		break;
		case 0xc0:	CW_color = 0xff;	break;
	}

	switch (Memory.FillRAM[0x2130] & 0x30)
	{
		case 0x00:	CW_math  = 0;		break;
		case 0x10:	CW_math  = ~CW;		break;
		case 0x20:	CW_math  = CW;		break;
		case 0x30:	CW_math  = 0xff;	break;
	}

	for (i = 0; i < n_regions; i++)
	{
		if (!(CW_color & (1 << i)))
			drawing_modes[i] |= 1;
		if (!(CW_math  & (1 << i)))
			drawing_modes[i] |= 2;
	}

	// Everything above worked in the SNES's own 256 columns; the regions the
	// renderer walks are in the widened picture's.

	int16	bounds[7];
	WideBoundaries(windows, n_regions, bounds);

	const int	ignore[2] =
	{
		WideIgnoreWindow(FALSE) ? WideIgnoreRegion(windows, n_regions) : -1,
		WideIgnoreWindow(TRUE)  ? WideIgnoreRegion(windows, n_regions) : -1
	};

	// Store backdrop clip window (draw everywhere color window allows)

	StoreWindowRegions(0, &IPPU.Clip[0][5], n_regions, bounds, drawing_modes, FALSE, TRUE, ignore[0]);
	StoreWindowRegions(0, &IPPU.Clip[1][5], n_regions, bounds, drawing_modes, TRUE,  TRUE, ignore[1]);

	// Store per-BG and OBJ clip windows

	for (j = 0; j < 5; j++)
	{
		uint8	W = Settings.DisableGraphicWindows ? 0 : CalcWindowMask(j, W1, W2);
		for (int sub = 0; sub < 2; sub++)
		{
			if (Memory.FillRAM[sub + 0x212e] & (1 << j))
				StoreWindowRegions(W, &IPPU.Clip[sub][j], n_regions, bounds, drawing_modes, sub, FALSE, ignore[sub]);
			else
				StoreWindowRegions(0, &IPPU.Clip[sub][j], n_regions, bounds, drawing_modes, sub, FALSE, ignore[sub]);
		}
	}

	if (IPPU.WideExtent)
	{
		const int	ext = IPPU.WideExtent;

		for (int sub = 0; sub < 2; sub++)
		{
			// "Clip" holds objects inside the columns the SNES itself
			// scanned out; the same for the backdrop when the side columns
			// are meant to stay black rather than take its colour.
			for (int layer = 4; layer <= 5; layer++)
			{
				if (layer == 4 ? (Widescreen.Sprites != WS_OBJ_CLIP) : S9xWideBackdropFills())
					continue;

				struct ClipData	*c = &IPPU.Clip[sub][layer];
				int	ct = 0;

				for (int k = 0; k < c->Count; k++)
				{
					int	l = (c->Left[k]  < ext) ? ext : c->Left[k];
					int	r = (c->Right[k] > ext + SNES_WIDTH) ? ext + SNES_WIDTH : c->Right[k];

					if (l >= r)
						continue;

					c->Left[ct]     = (uint16) l;
					c->Right[ct]    = (uint16) r;
					c->DrawMode[ct] = c->DrawMode[k];
					ct++;
				}

				c->Count = (uint8) ct;
			}
		}
	}
}
