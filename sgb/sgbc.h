/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Game Boy Color: the pane compositor and the per-cart display quirks.
// Everything here runs ONLY under SGBC on a Color-capable cart, so nothing in
// it can reach a plain SGB session; the quirks narrow it further, to the carts
// whose table row asks for them.

#ifndef _SGB_SGBC_H_
#define _SGB_SGBC_H_

#include <cstdint>

#include "gb_ppu.h"   // GB_SCREEN_WIDTH / GB_SCREEN_HEIGHT

namespace SGB {

// What the compositor needs from the emulator, gathered by the caller so this
// file does not depend on Emulator::Impl.
struct SgbcPane
{
	const uint16_t *color_fb;    // latched CGB frame, 160x144
	bool            color;       // trust color_fb (cart has real CGB palettes)
	uint8_t         bgp;         // the cart's BGP right now
	uint8_t         lcdc;        // and its LCDC
	uint8_t         fb_bgp;      // and the pair that drew color_fb, for when
	uint8_t         fb_lcdc;     // the LCD is off and the pane is frozen on it
	uint16_t        fallback[3]; // shades 1-3 when !color
	uint32_t        quirks;      // SGBC_QUIRK_* for this cart
};

// Paint the GB pane of a composed 256x224 SNES frame.
void SgbcComposePane(uint16_t *dest, uint32_t pitch_pixels, const SgbcPane &in);

// A Color cart can swap its *_TRN screen mid-frame (GDMA) while the BIOS is
// still draining the payload off the capture ring, which tears it. Repeats the
// frame the read starts on; a cart that holds its screen feeds the ring the
// same pixels either way. The caller owns one of these and drives it.
class SgbcTrnHold
{
public:
	void Reset() { arm_ = 0; hold_ = 0; }
	void Arm();                                   // a CHR_TRN / PCT_TRN went out
	void OnVBlank(const uint8_t *raw_frame);      // that frame's raw indices
	// The scanline to feed the capture ring: the held frame while one is held.
	const uint8_t *Line(const uint8_t *live, uint32_t ly) const;

private:
	uint8_t arm_  = 0;   // VBlanks until the frame is latched
	uint8_t hold_ = 0;   // VBlanks the latched frame is served for
	uint8_t frame_[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT] = {};
};

} // namespace SGB

#endif
