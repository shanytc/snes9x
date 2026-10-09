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
// `scale`: w x h pixels of 0x..RRGGBB, `stride` pixels per row, the top byte kept.
void S9xPadPictureDraw (uint32_t *px, int w, int h, int stride, float scale, int style, int lit, int outlined);

#endif
