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

namespace SGB {

// What the compositor needs from the emulator, gathered by the caller so this
// file does not depend on Emulator::Impl.
struct SgbcPane
{
	const uint16_t *color_fb;    // latched CGB frame, 160x144
	bool            color;       // trust color_fb (cart has real CGB palettes)
	uint8_t         bgp;         // the cart's BGP right now
	uint16_t        fallback[3]; // shades 1-3 when !color
	uint32_t        quirks;      // SGBC_QUIRK_* for this cart
};

// Paint the GB pane of a composed 256x224 SNES frame.
void SgbcComposePane(uint16_t *dest, uint32_t pitch_pixels, const SgbcPane &in);

} // namespace SGB

#endif
