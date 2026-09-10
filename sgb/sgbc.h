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

struct Ppu;

// What the compositor needs from the emulator, gathered by the caller so this
// file does not depend on Emulator::Impl.
struct SgbcPane
{
	const uint16_t *color_fb;    // latched CGB frame, 160x144
	bool            color;       // trust color_fb (cart has real CGB palettes)
	bool            fb_valid;    // color_fb holds a real frame (no palettes yet)
	uint8_t         bgp;         // the cart's BGP right now
	uint8_t         fb_bgp;      // and the pair that drew color_fb - what the
	uint8_t         fb_lcdc;     // pane is showing, which the live pair is not
	uint8_t         fb_obp0;     // OBP0/OBP1 that drew color_fb, for DMG_BLANK
	uint8_t         fb_obp1;
	bool            fb_blank_any;// the DMG blank held over any line of that frame
	uint16_t        fallback[3]; // shades 1-3 when !color
	uint32_t        quirks;      // SGBC_QUIRK_* for this cart
	uint8_t         hram_bgp;    // the cart's BGP shadow at $FF9C (BGP_SHADOW)
};

// The boot-logo cover, Super Game Boy Color's own. sgb.cpp holds the panel on
// the picture the boot handed over and releases on the first frame that is
// neither that picture nor a blank one - but it reads "blank" off BGP, which a
// Color cart never touches, so its cover would never lift. Same rule, asked of
// the color frame. The caller owns one of these and drives it.
class SgbcBootCover
{
public:
	void Reset() { ref_valid_ = false; }
	// Should the cover stay up? Once per frame, past the boot ROM's handoff.
	bool Hold(const Ppu &ppu);

private:
	bool    ref_valid_ = false;
	uint8_t ref_[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT] = {};
};

// Paint the GB pane of a composed 256x224 SNES frame. True when DMG_BLANK
// painted it flat: colour 0 by the cart's own registers.
bool SgbcComposePane(uint16_t *dest, uint32_t pitch_pixels, const SgbcPane &in);

// A Color cart can swap its *_TRN screen mid-frame (GDMA) while the BIOS is
// still draining the payload off the capture ring, which tears it. Repeats the
// frame the read starts on; a cart that holds its screen feeds the ring the
// same pixels either way. The caller owns one of these and drives it.
class SgbcTrnHold
{
public:
	void Reset() { arm_ = 0; hold_ = 0; }
	void Arm();                                   // a CHR_TRN / PCT_TRN went out
	// The line a DMG would fetch from this VRAM - bank-0 map and tiles, no CGB
	// attributes - which is what a DMG-era BIOS reads off the ring. Once per
	// finished scanline; sprites stay as drawn.
	void Scanline(const Ppu &ppu);
	void OnVBlank();                              // latch the frame just rendered
	// The scanline to feed the capture ring: the held frame while one is held.
	const uint8_t *Line(uint32_t ly) const;
	const uint8_t *Frame() const { return view_; }   // this frame's DMG view

private:
	uint8_t arm_  = 0;   // VBlanks until the frame is latched
	uint8_t hold_ = 0;   // VBlanks the latched frame is served for
	uint8_t view_[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT] = {};
	uint8_t frame_[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT] = {};
};

// LCD off, or back on with nothing drawn yet: past a mask cancel the pane
// can only be holding masked-era content until the next VBlank. LCD_BLANK rows only.
class SgbcLcdBlank
{
public:
	void Reset() { frames_ = 0; armed_ = false; }
	void OnVBlank() { ++frames_; }
	bool Blank(bool lcd_on, uint32_t quirks);
private:
	uint32_t frames_ = 0;   // VBlanks so far
	uint32_t off_at_ = 0;   // frames_ when the LCD was last seen off
	bool     armed_  = false;
};

} // namespace SGB

#endif
