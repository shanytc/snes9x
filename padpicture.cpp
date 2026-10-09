/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "padpicture.h"
#include <math.h>
#include <algorithm>

namespace
{

// Each button's shape in source pixels, traced from the pictures. n == 1: a circle (centre,
// radius); n == 0: a capsule (end centres, radius); else a polygon of n points.
struct PadPicShape { int button; int n; float pt[16][2]; };

const PadPicShape kPadPicShapes[] =
{
	{ PADPIC_UP,     6, { { 169, 146 }, { 178, 129 }, { 179, 129 }, { 187, 143 }, { 188, 148 }, { 169, 148 } } },
	{ PADPIC_DOWN,   6, { { 169, 208 }, { 188, 208 }, { 188, 211 }, { 179, 227 }, { 178, 227 }, { 169, 210 } } },
	{ PADPIC_LEFT,   5, { { 130, 176 }, { 148, 167 }, { 148, 187 }, { 146, 187 }, { 130, 178 } } },
	{ PADPIC_RIGHT,  5, { { 209, 167 }, { 228, 177 }, { 228, 178 }, { 212, 187 }, { 209, 187 } } },
	{ PADPIC_A,      1, { { 694.3f, 191.3f }, { 24.5f } } },
	{ PADPIC_B,      1, { { 615.8f, 247.5f }, { 24.5f } } },
	{ PADPIC_X,      1, { { 633.5f, 129.2f }, { 24.5f } } },
	{ PADPIC_Y,      1, { { 555.1f, 185.4f }, { 24.5f } } },
	{ PADPIC_L,      6, { { 124,  21 }, { 137,  17 }, { 165,  13 }, { 251,  13 }, { 252,  24 }, { 125,  29 } } },
	{ PADPIC_R,      8, { { 545,  14 }, { 546,  13 }, { 626,  13 }, { 663,  18 }, { 672,  21 }, { 671,  30 }, { 545,  24 }, { 544,  23 } } },
	{ PADPIC_SELECT, 0, { { 315.6f, 222.2f }, { 350.1f, 196.0f }, { 10.5f } } },
	{ PADPIC_START,  0, { { 397.5f, 222.8f }, { 431.9f, 196.5f }, { 10.5f } } }
};

struct PadPicColour { int r, g, b; };

float PadPicSegmentDistance (float x, float y, const float *a, const float *b)
{
	const float dx = b[0] - a[0], dy = b[1] - a[1];
	float t = ((x - a[0]) * dx + (y - a[1]) * dy) / (dx * dx + dy * dy);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return hypotf(x - a[0] - t * dx, y - a[1] - t * dy);
}

// Signed distance (source pixels) from the shape's edge; negative inside.
float PadPicDistance (const PadPicShape &b, float x, float y)
{
	if (b.n == 1)
		return hypotf(x - b.pt[0][0], y - b.pt[0][1]) - b.pt[1][0];
	if (b.n == 0)
		return PadPicSegmentDistance(x, y, b.pt[0], b.pt[1]) - b.pt[2][0];

	float d = 1e9f;
	bool inside = false;
	for (int i = 0, j = b.n - 1; i < b.n; j = i++)
	{
		const float *p = b.pt[i], *q = b.pt[j];
		const float e = PadPicSegmentDistance(x, y, q, p);
		if (e < d)
			d = e;
		if ((p[1] > y) != (q[1] > y) && x < (q[0] - p[0]) * (y - p[1]) / (q[1] - p[1]) + p[0])
			inside = !inside;
	}
	return inside ? -d : d;
}

// A held face button takes the real pad's colour: lavender/purple (USA), or the
// European and Super Famicom pads' blue X, green Y, red A, yellow B.
bool PadPicFaceColour (int button, int style, PadPicColour &c)
{
	const bool sfc = (style != S9X_PADPIC_USA);
	switch (button)
	{
		case PADPIC_X: c = sfc ? PadPicColour { 40, 105, 200 } : PadPicColour { 185, 182, 232 }; return true;
		case PADPIC_Y: c = sfc ? PadPicColour { 60, 150, 105 } : PadPicColour { 185, 182, 232 }; return true;
		case PADPIC_A: c = sfc ? PadPicColour { 190, 50, 50 } : PadPicColour { 80, 72, 172 }; return true;
		case PADPIC_B: c = sfc ? PadPicColour { 245, 210, 70 } : PadPicColour { 80, 72, 172 }; return true;
	}
	return false;
}

}

int S9xPadPictureButtonAt (float x, float y, int allowed, float slack)
{
	int button = 0;
	float nearest = slack;
	for (const PadPicShape &b : kPadPicShapes)
	{
		if (!(allowed & b.button))
			continue;
		const float d = PadPicDistance(b, x, y);
		if (d < nearest)
		{
			nearest = d;
			button = b.button;
		}
	}
	return button;
}

void S9xPadPictureDraw (uint32_t *px, int w, int h, int stride, float scale, int style, int lit, int outlined)
{
	const float s = scale;
	const PadPicColour outline = { 255, 200, 0 };
	for (const PadPicShape &b : kPadPicShapes)
	{
		const bool on = (lit & b.button) != 0;
		const float ring = (outlined & b.button) ? 2.0f : 0.0f;	// display pixels outside the edge
		if (!on && !ring)
			continue;

		// Face buttons in the pad's colours, d-pad arrows and Start/Select yellow, the shoulders a blue tint
		PadPicColour fill, rim;
		float fillAlpha = 1.0f, rimWidth = 1.0f;
		if (PadPicFaceColour(b.button, style, fill))
			rim = { fill.r * 7 / 10, fill.g * 7 / 10, fill.b * 7 / 10 };
		else if (b.button & (PADPIC_UP | PADPIC_DOWN | PADPIC_LEFT | PADPIC_RIGHT | PADPIC_START | PADPIC_SELECT))
			fill = { 255, 210, 0 }, rim = { 200, 140, 0 };
		else
			fill = { 80, 170, 255 }, rim = { 40, 130, 230 }, fillAlpha = 120 / 255.0f, rimWidth = 2.0f;

		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		const float r = b.n == 1 ? b.pt[1][0] : b.n == 0 ? b.pt[2][0] : 0;
		for (int i = 0; i < (b.n == 1 ? 1 : b.n == 0 ? 2 : b.n); i++)
		{
			x0 = (std::min)(x0, b.pt[i][0] - r);
			y0 = (std::min)(y0, b.pt[i][1] - r);
			x1 = (std::max)(x1, b.pt[i][0] + r);
			y1 = (std::max)(y1, b.pt[i][1] + r);
		}
		const int left = (std::max)(0, (int) (x0 * s) - 3), top = (std::max)(0, (int) (y0 * s) - 3);
		const int right = (std::min)(w - 1, (int) (x1 * s) + 3), bottom = (std::min)(h - 1, (int) (y1 * s) + 3);

		for (int y = top; y <= bottom; y++)
			for (int x = left; x <= right; x++)
			{
				int inFill = 0, inRim = 0, inRing = 0;
				for (int sy = 0; sy < 4; sy++)
					for (int sx = 0; sx < 4; sx++)
					{
						const float d = PadPicDistance(b, (x + (sx + 0.5f) / 4) / s, (y + (sy + 0.5f) / 4) / s) * s;
						if (d >= 0)
							inRing += (d < ring);
						else if (on)
							(d < -rimWidth ? inFill : inRim)++;
					}
				if (!inFill && !inRim && !inRing)
					continue;

				const float cf = inFill / 16.0f * fillAlpha, cr = inRim / 16.0f, cg = inRing / 16.0f, cb = 1.0f - cf - cr - cg;
				uint32_t &c = px[y * stride + x];
				const int blue = (int) ((c & 0xff) * cb + fill.b * cf + rim.b * cr + outline.b * cg + 0.5f);
				const int green = (int) (((c >> 8) & 0xff) * cb + fill.g * cf + rim.g * cr + outline.g * cg + 0.5f);
				const int red = (int) (((c >> 16) & 0xff) * cb + fill.r * cf + rim.r * cr + outline.r * cg + 0.5f);
				c = (c & 0xff000000) | (red << 16) | (green << 8) | blue;
			}
	}
}
