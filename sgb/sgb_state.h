/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_SGB_STATE_H_
#define _SGB_SGB_STATE_H_

#include <cstdint>

namespace SGB {

// SGB persistent state — palettes, attribute map, and the bulk storage
// that PAL_TRN / ATTR_TRN populate from GB VRAM.
//
// The attribute map is 20 × 18 tiles (360 entries). Each entry holds a
// 2-bit palette index (0..3) selecting which of the 4 active palettes
// the 8×8 tile renders through. We store one byte per entry to keep
// access cheap — only the low 2 bits matter.

constexpr uint32_t SGB_TILE_COLS = 20;
constexpr uint32_t SGB_TILE_ROWS = 18;
constexpr uint32_t SGB_TILES     = SGB_TILE_COLS * SGB_TILE_ROWS;  // 360

struct SgbPalette
{
	uint16_t colors[4];  // BGR555 — identical to SNES CGRAM layout
};

struct SgbState
{
	// Four palettes currently applied to the GB screen output.
	SgbPalette active[4];

	// Per-tile palette index over the 20 × 18 grid.
	uint8_t    attr_map[SGB_TILES];

	// 512 system palettes uploaded by PAL_TRN. 4 colors × 512 = 2048 words.
	uint16_t   system_palettes[512 * 4];
	bool       system_palettes_loaded;

	// 45 attribute files uploaded by ATTR_TRN. 90 bytes each.
	uint8_t    attr_files[45 * 90];
	bool       attr_files_loaded;

	// MASK_EN current mode — lands in P6d. Default 0 = show normally.
	uint8_t    mask_mode;

	// Diagnostics.
	uint32_t   palette_writes;
	uint32_t   attr_writes;
	uint32_t   last_cmd;        // 0xFF = none
};

void     SgbReset(SgbState &s);

// Dispatch a complete SGB command. `vram_4kb` points at the first 4KB
// of GB VRAM (GB 0x8000..0x8FFF) — used by the *_TRN bulk-load
// commands. Pass nullptr if unavailable (those commands will no-op).
void     SgbHandleCommand(SgbState &s, uint8_t cmd, const uint8_t *data,
                          uint32_t len, const uint8_t *vram_4kb);

// Look up the palette index assigned to a screen tile.
uint8_t  SgbGetTilePalette(const SgbState &s, uint32_t tile_x, uint32_t tile_y);

// Resolve a framebuffer shade (0..3) into a final BGR555 color using
// the tile's assigned palette. Called per pixel in the P7 display path.
uint16_t SgbResolveColor(const SgbState &s, uint32_t tile_x, uint32_t tile_y, uint8_t shade);

} // namespace SGB

#endif
