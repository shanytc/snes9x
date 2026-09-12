/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Game Boy Color: the in-memory patches. Two targets, two shapes:
//   - the SNES-side SGB2 BIOS takes one built-in IPS blob (PatchSgb2BiosForSgbc)
//     that keys its pane;
//   - the Game Boy cart takes a per-game row of byte edits (CartPatch,
//     ApplyCartPatch) that make a dual cart run its SGB init on the Color
//     branch too.

#ifndef _SGB_SGBC_PATCHES_H_
#define _SGB_SGBC_PATCHES_H_

#include <cstdint>
#include <cstddef>
#include <vector>

namespace SGB {

// Format decoder only: apply a plain IPS ("PATCH", 5-byte records, RLE
// records, "EOF") to any image in place; a record past the end grows it. False
// on a malformed patch, with the image untouched. PatchSgb2BiosForSgbc is its
// one caller.
bool ApplyIps(std::vector<uint8_t> &rom, const uint8_t *ips, size_t len);

// The SNES-side SGB2 BIOS: apply the built-in IPS that keys the SGBC pane, only
// to the dump it was built against (SHA-256 of the pristine 512 KB image). True
// when `bios` was patched.
bool PatchSgb2BiosForSgbc(std::vector<uint8_t> &bios);

// Per-cart display quirks, applied by the SGBC compositor (sgbc.cpp) and by
// nothing else. A cart whose row leaves `quirks` at 0 is untouched.
//   BGP_BLANK: the cart blanks its screen the DMG way, mapping every colour
//   index to shade 0 through BGP. CGB rendering ignores BGP, so without this
//   the *_TRN payload the blank was hiding stays on the pane.
static const uint32_t SGBC_QUIRK_BGP_BLANK = 1u << 0;
//   HOLD_PAYLOAD: the cart leaves its *_TRN payload on the Color screen between
//   the transfer and its first real draw, where a real SGB2 shows the DMG
//   path's blank. Blank the pane while the cart shows no picture of its own
//   (BGP all shade 0, or LCD/BG off) and the CGB frame looks like payload (a
//   tile grid: few colours, very dense horizontal colour changes).
static const uint32_t SGBC_QUIRK_HOLD_PAYLOAD = 1u << 1;
//   DMG_BLANK: the cart's SGB path blanks its screen the DMG way - BGP, OBP0
//   and OBP1 all zero, a flat shade-0 frame there. CGB rendering ignores all
//   three and keeps the sprites up; paint the pane in colour 0 instead.
static const uint32_t SGBC_QUIRK_DMG_BLANK = 1u << 2;
//   LCD_BLANK: the cart cancels its mask while its LCD is still off, so the
//   frame the pane holds is masked-era *_TRN payload. Keep the cover, filled
//   with colour 0, until the first frame drawn after the LCD comes back on.
static const uint32_t SGBC_QUIRK_LCD_BLANK = 1u << 3;
//   BGP_SHADOW: the cart hides a redraw by blanking through BGP, but on the
//   Color path it stops copying its BGP shadow to the register, so the
//   register says nothing. Read the shadow it keeps at $FF9C instead: while
//   every index there maps to one shade, paint the pane that shade.
static const uint32_t SGBC_QUIRK_BGP_SHADOW = 1u << 4;
//   BGP_OBJ_BLANK: the cart blanks by writing BGP=$00 with sprites off, and
//   its Color path then leaves BGP at $00 over real screens, so the register
//   alone says nothing. Cover the pane only while OBJ is off with it.
static const uint32_t SGBC_QUIRK_BGP_OBJ_BLANK = 1u << 5;
//   STATIC_UNMASK: the cart cancels its mask onto a screen it then leaves
//   alone, so the wait for the pane to change never ends and the cover sits
//   there. Its Color pane is its own frame already; hand it back at the cancel.
static const uint32_t SGBC_QUIRK_STATIC_UNMASK = 1u << 6;
//   DENSE_PAYLOAD: the cart repaints its palettes mid-transfer, so the payload
//   test's colour cap rejects it; judge this row's payload on density alone.
static const uint32_t SGBC_QUIRK_DENSE_PAYLOAD = 1u << 7;

// One byte run in a Game Boy cart image, up to three bytes (a jump, a store):
// `old` is verified before `neu` is written.
struct CartEdit
{
	uint32_t addr;
	uint8_t  len;
	uint8_t  old[3];
	uint8_t  neu[3];
};

// A per-game row of cart edits. A dual cart picks its branch from A at $0100
// and runs no SGB code on the Color branch, so the BIOS never gets its border
// or sound packets. These edits make a known game run its SGB init as well:
// one row per cart, its edits inline (eight at most so far; unused slots stay
// zero). Rows are found by header global-sum + title, never a whole-ROM hash.
struct CartPatch
{
	uint16_t    global_sum;   // header $014E-$014F, big-endian as stored
	const char *title;        // header $0134.., up to the first NUL
	const char *name;         // shown on the load banner
	uint8_t     edit_count;
	CartEdit    edits[8];
	uint32_t    quirks = 0;   // SGBC_QUIRK_* bits; 0 for almost every row
};

const CartPatch *FindSgbcCartPatch(const uint8_t *rom, size_t size);

// Super Game Boy compatibility: a cart whose own SGB path stops under a BIOS
// (Joryuu Janshi parks on the SGB2 boot value). Same row shape; applied in
// memory under any SGB BIOS, Super Game Boy Color included.
const CartPatch *FindSgbCartPatch(const uint8_t *rom, size_t size);

// Write a row's edits into the Game Boy cart image in memory. True when every
// edit's old bytes matched and all were written; false with nothing written
// otherwise. Never resizes the image.
bool ApplyCartPatch(const CartPatch &p, uint8_t *rom, size_t size);

} // namespace SGB

#endif
