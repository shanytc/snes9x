/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "sgbc.h"
#include "sgb.h"
#include "gb_ppu.h"
#include "sgbc_patches.h"
#include "../snes9x.h"
#include "../ppu.h"
#include "../gfx.h"

#include <cstring>

namespace SGB {

// The keys the patched BIOS paints GB shades 1-3 of the pane in. Kept beside
// the compositor that consumes them; sgb.cpp has its own copy for the BIOS
// state block it writes.
static const uint16_t kSgbcKey[3] = { 0x7C1F, 0x7D1F, 0x7E1F };

// The BIOS puts the GB picture at y=39, not the y=40 the tile grid suggests.
static const uint32_t ORIGIN_X = 48, ORIGIN_Y = 39;

static inline uint16_t BgrToHostBright(uint16_t bgr, const uint8 *xb)
{
	uint8 r = xb[bgr & 0x1F];
	uint8 g = xb[(bgr >> 5) & 0x1F];
	uint8 b = xb[(bgr >> 10) & 0x1F];
	S9xApplyColorAdjustments(r, g, b, 0x1F);
	return static_cast<uint16_t>(BUILD_PIXEL(r, g, b));
}

// Does this CGB frame look like *_TRN payload rather than a picture? Payload
// is a 2bpp tile grid: at most a handful of colours, and a colour change every
// couple of pixels across the whole row. Text and art change colour far less
// often per row, and art uses far more colours.
static bool LooksLikePayload(const uint16_t *fb)
{
	uint16_t cols[9]; int ncols = 0;
	uint32_t transitions = 0;
	for (int y = 0; y < GB_SCREEN_HEIGHT; y += 2)
	{
		const uint16_t *row = fb + y * GB_SCREEN_WIDTH;
		uint16_t prev = row[0];
		for (int x = 0; x < GB_SCREEN_WIDTH; ++x)
		{
			if (row[x] != prev) ++transitions;
			prev = row[x];
			if (ncols < 9)
			{
				int k = 0;
				while (k < ncols && cols[k] != row[x]) ++k;
				if (k == ncols) cols[ncols++] = row[x];
			}
		}
	}
	// 72 rows sampled: payload measured 37-94 changes per row on three carts,
	// a dense text screen about 18, art far more colours than 8.
	return ncols <= 8 && transitions >= 72u * 24u;
}

void SgbcComposePane(uint16_t *dest, uint32_t pitch_pixels, const SgbcPane &in)
{

	// Only while the cart is showing no picture of its own - BGP mapping every
	// index to shade 0, or the BG off - because the payload is what that blank
	// was hiding. Judge that on the registers that DREW the frame on the pane,
	// not on the live ones: the pane is a frame behind, and with the LCD off it
	// is frozen on an older frame still (a cart can blank, transfer, then draw a
	// real picture and turn the LCD off over it - Katou Hifumi's board).
	// Content is evidence only once the cart has CGB palettes. Before that the
	// pane is drawn from the ICD2 feed, which carries Color BG indices where a
	// DMG would have drawn nothing, so the blank signal alone decides - and a
	// real picture can look like a tile grid too (a shougi board matched for the
	// whole game), which is what the content test is there to reject.
	const bool hold = (in.quirks & SGBC_QUIRK_HOLD_PAYLOAD) && in.fb_valid &&
	                  (in.fb_bgp == 0 || !(in.fb_lcdc & 0x01)) &&
	                  (!in.color || LooksLikePayload(in.color_fb));
	const int width = (IPPU.RenderedScreenWidth > 0) ? IPPU.RenderedScreenWidth : SNES_WIDTH;
	if (!dest || width > SNES_WIDTH ||
	    (int) PPU.ScreenHeight < (int) (ORIGIN_Y + GB_SCREEN_HEIGHT)) return;

	// A cart that blanks the DMG way - BGP mapping every index to shade 0 -
	// means it for the pane too, but the Color renderer ignores BGP, so the
	// *_TRN payload the blank was hiding stays up. Only carts whose table row
	// asks for this are affected.
	const bool bgp_blank = (in.quirks & SGBC_QUIRK_BGP_BLANK) != 0;

	// The keys and the backdrop exactly as the PPU drew each line - its
	// brightness and backdrop at render time, not now: the BIOS force-blanks
	// at VBlank, so PPU.Brightness already reads 0 here.
	int      last_b    = -1;
	uint16_t last_back = 0xFFFF;
	const uint8 *xb = nullptr;
	uint16_t key[3] = {}, mono[3] = {}, back = 0;

	for (uint32_t y = 0; y < GB_SCREEN_HEIGHT; ++y)
	{
		uint8  b;
		uint16 bd;
		S9xGetLineRenderState((int) (ORIGIN_Y + y), b, bd);
		if ((int) b != last_b || bd != last_back)
		{
			last_b    = b;
			last_back = bd;
			xb = mul_brightness[b];
			for (int k = 0; k < 3; ++k)
			{
				key[k]  = BgrToHostBright(kSgbcKey[k], xb);
				mono[k] = BgrToHostBright(in.fallback[k], xb);
			}
			back = BgrToHostBright(bd, xb);
		}

		const uint16_t *src = in.color_fb + y * GB_SCREEN_WIDTH;
		uint16_t *dst = dest + (ORIGIN_Y + y) * pitch_pixels + ORIGIN_X;
		for (uint32_t x = 0; x < GB_SCREEN_WIDTH; ++x)
		{
			const uint16_t px = dst[x];
			if (px == key[0] || px == key[1] || px == key[2])
			{
				// The key stands for a GB colour INDEX: in CGB mode the buffer
				// feeding the BIOS ring carries indices, not shades.
				const int idx = (px == key[0]) ? 1 : (px == key[1]) ? 2 : 3;
				const int sh  = (in.bgp >> (idx * 2)) & 3;
				// Only while the cart has no CGB palettes of its own. Past that
				// point BGP means nothing to a Color game - Dragon Dance leaves
				// it at $00 for the whole boot while drawing in colour, and
				// blanking on it there erases the picture.
				if (hold)                  dst[x] = back;
				else if (in.color)         dst[x] = BgrToHostBright(src[x], xb);
				else if (bgp_blank && !sh) dst[x] = back;
				else                       dst[x] = sh ? mono[sh - 1] : back;
			}
			else if (px == back && in.color && !hold)
				dst[x] = BgrToHostBright(src[x], xb);
		}
	}
}

// A frame later than the command: the cart may still be painting now.
void SgbcTrnHold::Arm() { arm_ = 2; }

void SgbcTrnHold::OnVBlank(const uint8_t *raw_frame)
{
	if (arm_)
	{
		// One frame is all the tail of the read needs, and short enough not to
		// reach the next transfer of a back-to-back run.
		if (--arm_ == 0 && raw_frame)
		{
			std::memcpy(frame_, raw_frame, sizeof frame_);
			hold_ = 1;
		}
	}
	else if (hold_)
		--hold_;
}

const uint8_t *SgbcTrnHold::Line(const uint8_t *live, uint32_t ly) const
{
	return (hold_ && ly < GB_SCREEN_HEIGHT) ? &frame_[ly * GB_SCREEN_WIDTH] : live;
}

} // namespace SGB
