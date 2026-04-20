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

// SGB border is 32x28 tiles of 8x8 = 256x224 pixels. The centered 20x18
// tile area (starting at tile (6, 5)) is covered by the GB screen.
constexpr uint32_t SGB_BORDER_COLS = 32;
constexpr uint32_t SGB_BORDER_ROWS = 28;
constexpr uint32_t SGB_BORDER_W    = SGB_BORDER_COLS * 8;   // 256
constexpr uint32_t SGB_BORDER_H    = SGB_BORDER_ROWS * 8;   // 224
constexpr uint32_t SGB_GB_TILE_X   = 6;   // GB screen starts at tile (6, 5)
constexpr uint32_t SGB_GB_TILE_Y   = 5;

// MASK_EN modes — applied to the GB screen area only, not the border.
enum : uint8_t
{
	SGB_MASK_CANCEL  = 0,  // show GB normally
	SGB_MASK_FREEZE  = 1,  // show last frame captured when MASK_EN was set
	SGB_MASK_BLACK   = 2,  // all black
	SGB_MASK_BLANK   = 3   // all color 0 of palette 0
};

struct SgbPalette
{
	uint16_t colors[4];  // BGR555 — identical to SNES CGRAM layout
};

struct SgbBorder
{
	// 256 tiles × 32 bytes (4bpp 8x8, planar SNES layout). Uploaded in
	// two halves of 128 tiles each via CHR_TRN.
	uint8_t  tiles[256 * 32];

	// 32x32 tilemap entries in SNES BG format:
	//   bits 0-9   tile number
	//   bits 10-12 palette (SGB border uses 4..7)
	//   bit  13    priority (ignored)
	//   bit  14    H flip
	//   bit  15    V flip
	uint16_t tile_map[32 * 32];

	// 4 border palettes × 16 colors each.
	uint16_t palettes[4][16];

	bool     tiles_loaded;
	bool     map_loaded;
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

	// MASK_EN mode currently applied to the GB screen area.
	uint8_t    mask_mode;

	// Frozen snapshot of the GB framebuffer used by SGB_MASK_FREEZE.
	uint8_t    frozen_frame[160 * 144];
	bool       frozen_frame_valid;

	// SGB border state — tiles + map + palettes.
	SgbBorder  border;

	// Diagnostics.
	uint32_t   palette_writes;
	uint32_t   attr_writes;
	uint32_t   last_cmd;        // 0xFF = none
};

void     SgbReset(SgbState &s);

// Dispatch a complete SGB command. `vram_4kb` points at the first 4KB
// of GB VRAM (GB 0x8000..0x8FFF) — used by the *_TRN bulk-load
// commands. `gb_fb_160x144` is the current GB framebuffer; MASK_EN
// freeze mode snapshots it. Either pointer may be null for callers
// that can't supply them (those commands will no-op).
void     SgbHandleCommand(SgbState &s, uint8_t cmd, const uint8_t *data,
                          uint32_t len,
                          const uint8_t *vram_4kb,
                          const uint8_t *gb_fb_160x144);

// Look up the palette index assigned to a screen tile.
uint8_t  SgbGetTilePalette(const SgbState &s, uint32_t tile_x, uint32_t tile_y);

// Resolve a framebuffer shade (0..3) into a final BGR555 color using
// the tile's assigned palette. Called per pixel in the P7 display path.
uint16_t SgbResolveColor(const SgbState &s, uint32_t tile_x, uint32_t tile_y, uint8_t shade);

// Render the SGB border into a 256 × 224 BGR555 buffer. The central
// 20 × 18 tile area (covered by the GB screen) is left untouched —
// P7 blits the live GB framebuffer over it. When no border has been
// uploaded, the outer frame is filled solid black.
void     SgbRenderBorder(const SgbState &s, uint16_t *out_256x224);

} // namespace SGB

#endif
