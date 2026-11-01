/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _PADPICTURE_H_
#define _PADPICTURE_H_

#include <stdint.h>

// Input Configuration's controller picture, the same in every frontend: win32/rsrc/pad_*.bmp,
// data/pads/pad_*.png elsewhere. The three pictures share one outline.
#define S9X_PADPIC_WIDTH	794
#define S9X_PADPIC_HEIGHT	360

enum
{
	PADPIC_UP = 1 << 0, PADPIC_DOWN = 1 << 1, PADPIC_LEFT = 1 << 2, PADPIC_RIGHT = 1 << 3,
	PADPIC_A = 1 << 4, PADPIC_B = 1 << 5, PADPIC_X = 1 << 6, PADPIC_Y = 1 << 7,
	PADPIC_L = 1 << 8, PADPIC_R = 1 << 9, PADPIC_START = 1 << 10, PADPIC_SELECT = 1 << 11,
	PADPIC_ALL = (1 << 12) - 1
};

// USA pad (lavender and purple face buttons), European and Super Famicom pads (coloured).
enum
{
	S9X_PADPIC_USA = 0,
	S9X_PADPIC_EUROPE,
	S9X_PADPIC_JAPAN,
	S9X_PADPIC_NUM_STYLES
};

// The button among `allowed` nearest a source-pixel point, within `slack` source pixels of its
// edge so the small arrows are easy to hit; 0 if none.
int S9xPadPictureButtonAt (float x, float y, int allowed, float slack = 8.0f);

// Lights the `lit` buttons and rings the `outlined` ones over a copy of the picture scaled by
// `scale`: w x h pixels of 0x..RRGGBB, `stride` pixels per row, the top byte kept. If
// `premultiplied`, the top byte is alpha (colours premultiplied by it), raised under the lights.
void S9xPadPictureDraw (uint32_t *px, int w, int h, int stride, float scale, int style, int lit, int outlined, bool premultiplied = false);

// Clears the dialog grey around the full-size picture for themed backgrounds: 0x..RRGGBB in,
// premultiplied 0xAARRGGBB out. The outline's soft edge becomes black ink, the L and R labels `label`.
void S9xPadPictureMatte (uint32_t *px, int stride, uint32_t label);

// The Sunsoft Pachinko controller's picture (win32/rsrc/pachinko.bmp): the dial's right half turns
// it stronger, the left half weaker, and the centre stands for the Stop button.
#define S9X_PACHINKOPIC_WIDTH	357
#define S9X_PACHINKOPIC_HEIGHT	360

enum
{
	PACHPIC_STRONGER = 1 << 0, PACHPIC_WEAKER = 1 << 1, PACHPIC_STOP = 1 << 2,
	PACHPIC_ALL = (1 << 3) - 1
};

// As S9xPadPictureButtonAt and S9xPadPictureDraw, for the Pachinko picture.
int S9xPachinkoPictureButtonAt (float x, float y, int allowed, float slack = 8.0f);
void S9xPachinkoPictureDraw (uint32_t *px, int w, int h, int stride, float scale, int lit, int outlined, bool premultiplied = false);

// A power gauge round the dial, `power` 0-1 filled clockwise from its lower left, and the
// gauge's colour (0xRRGGBB) at a power, green through yellow to red.
void S9xPachinkoPictureDrawPower (uint32_t *px, int w, int h, int stride, float scale, float power, bool premultiplied = false);
uint32_t S9xPachinkoPowerColour (float power);
// Where a frontend writes the power as text: the picture's empty top-left corner, in source pixels,
// a "POWER" label of this height over the percentage in the gauge's colour.
#define S9X_PACHINKOPIC_POWER_X		6
#define S9X_PACHINKOPIC_POWER_Y		4
#define S9X_PACHINKOPIC_LABEL_SIZE	17
#define S9X_PACHINKOPIC_VALUE_SIZE	40

// The controller settings' preview dial for the gauge: 0 when the settings open, turned by the
// held PACHPIC_STRONGER/WEAKER parts as S9xControlEOF turns the real one (a step, then every
// frame once held 15), on a 60 Hz clock of its own.
struct S9xPachinkoPreview
{
	void reset ();
	bool turn (int held);	// from a picture timer; true when the dial moved
	float power () const { return dial / 127.0f; }
	int percent () const { return (dial * 100 + 63) / 127; }

	int dial = 0;

  private:
	int hold = 0;
	double last_ms = -1, frames = 0;	// the clock when the turn keys were last seen held (-1: released)
};

#endif
