/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// SGB palette + attribute command handlers (subset for P6b):
//
//   PAL01/PAL23/PAL03/PAL12 — set two active palettes sharing color 0
//   ATTR_BLK                — apply palette to rectangular blocks
//   ATTR_LIN                — apply palette to whole rows/columns
//   ATTR_DIV                — split screen by a line into two halves
//   ATTR_CHR                — set palette per individual tile
//   PAL_SET                 — copy 4 palettes out of system-palette RAM
//   PAL_TRN                 — upload 512 system palettes from GB VRAM
//   ATTR_SET                — apply one of 45 stored attribute files
//   ATTR_TRN                — upload 45 attribute files from GB VRAM
//
// All commands expect `data[0]` to be the original command/length byte
// (preserved by sgb_packet.cpp). The actual parameters start at data[1].

#include "sgb_state.h"

#include <cstring>

namespace SGB {

namespace {

// Default grayscale — 4 evenly-spaced shades in BGR555.
constexpr uint16_t DEFAULT_PAL[4] = {
	0x7FFF,   // 31,31,31 — white
	0x56B5,   // 21,21,21 — light gray
	0x294A,   // 10,10,10 — dark gray
	0x0000    //  0, 0, 0 — black
};

inline uint16_t Rd16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

// Write a palette given a "shared color 0" + 3 unique colors layout.
inline void SetPaletteShared(SgbState &s, int idx, uint16_t c0,
                             const uint8_t *unique3)
{
	s.active[idx].colors[0] = c0;
	s.active[idx].colors[1] = Rd16(unique3 + 0);
	s.active[idx].colors[2] = Rd16(unique3 + 2);
	s.active[idx].colors[3] = Rd16(unique3 + 4);
}

// Apply a stored attribute file (90 bytes, 4 packed 2-bit entries per
// byte, MSB first) to the live attribute map.
void ApplyAttrFile(SgbState &s, uint8_t num)
{
	if (num >= 45) return;
	const uint8_t *file = &s.attr_files[num * 90];
	for (uint32_t i = 0; i < SGB_TILES; ++i)
	{
		const uint8_t byte  = file[i >> 2];
		const uint8_t shift = static_cast<uint8_t>((3 - (i & 3)) * 2);
		s.attr_map[i] = static_cast<uint8_t>((byte >> shift) & 0x03);
	}
	++s.attr_writes;
}

// ------------------------------------------------------------------
// PAL01 / PAL23 / PAL03 / PAL12
// ------------------------------------------------------------------
// Data layout (after cmd byte):
//   [1..2]   shared color 0
//   [3..8]   first palette, 3 unique colors (color 1, 2, 3)
//   [9..14]  second palette, 3 unique colors
//   [15]     unused

void HandlePal(SgbState &s, int first_idx, int second_idx, const uint8_t *d)
{
	const uint16_t c0 = Rd16(d + 1);
	SetPaletteShared(s, first_idx,  c0, d + 3);
	SetPaletteShared(s, second_idx, c0, d + 9);
	++s.palette_writes;
}

// ------------------------------------------------------------------
// ATTR_BLK — rectangular palette blocks.
// ------------------------------------------------------------------
// Data layout:
//   [1]      number of data sets (1..18)
//   [2..]    6 bytes per data set:
//              [0] control (bit 0 = change inside,
//                           bit 1 = change border,
//                           bit 2 = change outside)
//              [1] palette byte (bits 0-1 inside, 2-3 border, 4-5 outside)
//              [2] X1 (0..19)
//              [3] Y1 (0..17)
//              [4] X2 (0..19, >= X1)
//              [5] Y2 (0..17, >= Y1)

void HandleAttrBlk(SgbState &s, const uint8_t *d, uint32_t len)
{
	uint8_t count = d[1] & 0x1F;
	if (count > 18) count = 18;

	for (uint8_t i = 0; i < count; ++i)
	{
		const uint32_t off = 2 + static_cast<uint32_t>(i) * 6;
		if (off + 6 > len) break;
		const uint8_t *ds = &d[off];

		const uint8_t control     = static_cast<uint8_t>(ds[0] & 0x07);
		const uint8_t pal_byte    = ds[1];
		const uint8_t x1          = static_cast<uint8_t>(ds[2] & 0x1F);
		const uint8_t y1          = static_cast<uint8_t>(ds[3] & 0x1F);
		const uint8_t x2          = static_cast<uint8_t>(ds[4] & 0x1F);
		const uint8_t y2          = static_cast<uint8_t>(ds[5] & 0x1F);

		const uint8_t pal_inside  = static_cast<uint8_t>(pal_byte & 0x03);
		const uint8_t pal_border  = static_cast<uint8_t>((pal_byte >> 2) & 0x03);
		const uint8_t pal_outside = static_cast<uint8_t>((pal_byte >> 4) & 0x03);

		for (uint32_t y = 0; y < SGB_TILE_ROWS; ++y)
		{
			for (uint32_t x = 0; x < SGB_TILE_COLS; ++x)
			{
				const bool in_rect   = (x >= x1 && x <= x2 && y >= y1 && y <= y2);
				const bool on_border = in_rect && (x == x1 || x == x2 || y == y1 || y == y2);
				const bool inside    = in_rect && !on_border;
				const bool outside   = !in_rect;

				uint8_t pal = s.attr_map[y * SGB_TILE_COLS + x];
				bool write = false;

				if (inside  && (control & 0x01)) { pal = pal_inside;  write = true; }
				if (on_border && (control & 0x02)) { pal = pal_border;  write = true; }
				if (outside && (control & 0x04)) { pal = pal_outside; write = true; }

				if (write) s.attr_map[y * SGB_TILE_COLS + x] = pal;
			}
		}
	}
	++s.attr_writes;
}

// ------------------------------------------------------------------
// ATTR_LIN — whole-row or whole-column palettes.
// ------------------------------------------------------------------
// Data layout:
//   [1]      number of data sets (1..110)
//   [2..]    one byte per set:
//              bits 0-4  line number (row or column)
//              bits 5-6  palette (0..3)
//              bit  7    direction (0 = horizontal row, 1 = vertical column)

void HandleAttrLin(SgbState &s, const uint8_t *d, uint32_t len)
{
	uint8_t count = d[1] & 0x7F;
	if (count > 110) count = 110;

	for (uint8_t i = 0; i < count; ++i)
	{
		const uint32_t off = 2 + i;
		if (off >= len) break;
		const uint8_t b       = d[off];
		const uint8_t line    = static_cast<uint8_t>(b & 0x1F);
		const uint8_t pal     = static_cast<uint8_t>((b >> 5) & 0x03);
		const bool    vert    = (b & 0x80) != 0;

		if (vert)
		{
			if (line >= SGB_TILE_COLS) continue;
			for (uint32_t y = 0; y < SGB_TILE_ROWS; ++y)
				s.attr_map[y * SGB_TILE_COLS + line] = pal;
		}
		else
		{
			if (line >= SGB_TILE_ROWS) continue;
			for (uint32_t x = 0; x < SGB_TILE_COLS; ++x)
				s.attr_map[line * SGB_TILE_COLS + x] = pal;
		}
	}
	++s.attr_writes;
}

// ------------------------------------------------------------------
// ATTR_DIV — split the screen into two halves plus a divider line.
// ------------------------------------------------------------------
// Data layout:
//   [1]  control:
//          bits 0-1  palette for one side
//          bits 2-3  palette for the other side
//          bits 4-5  palette for the divider line
//          bit  6    direction (0 = horizontal split at row Y,
//                                1 = vertical   split at col X)
//   [2]  split line number

void HandleAttrDiv(SgbState &s, const uint8_t *d)
{
	const uint8_t info       = d[1];
	const uint8_t split      = d[2];
	const uint8_t pal_below  = static_cast<uint8_t>(info & 0x03);
	const uint8_t pal_above  = static_cast<uint8_t>((info >> 2) & 0x03);
	const uint8_t pal_line   = static_cast<uint8_t>((info >> 4) & 0x03);
	const bool    vertical   = (info & 0x40) != 0;

	for (uint32_t y = 0; y < SGB_TILE_ROWS; ++y)
	{
		for (uint32_t x = 0; x < SGB_TILE_COLS; ++x)
		{
			uint32_t key = vertical ? x : y;
			uint8_t  pal;
			if (key <  split)      pal = pal_above;
			else if (key == split) pal = pal_line;
			else                    pal = pal_below;
			s.attr_map[y * SGB_TILE_COLS + x] = pal;
		}
	}
	++s.attr_writes;
}

// ------------------------------------------------------------------
// ATTR_CHR — per-tile palette writes along a scan direction.
// ------------------------------------------------------------------
// Data layout:
//   [1]        start X (0..19)
//   [2]        start Y (0..17)
//   [3..4]     number of writes (little-endian, up to ~360)
//   [5]        direction: 0 = horizontal, 1 = vertical
//   [6..]      packed 2-bit palette indices, 4 per byte, MSB first

void HandleAttrChr(SgbState &s, const uint8_t *d, uint32_t len)
{
	uint32_t x       = d[1];
	uint32_t y       = d[2];
	const uint16_t n = static_cast<uint16_t>(d[3] | (static_cast<uint16_t>(d[4]) << 8));
	const bool vert  = (d[5] & 0x01) != 0;

	for (uint16_t i = 0; i < n; ++i)
	{
		const uint32_t byte_idx = 6u + (i >> 2);
		if (byte_idx >= len) break;
		const uint8_t shift = static_cast<uint8_t>((3 - (i & 3)) * 2);
		const uint8_t pal   = static_cast<uint8_t>((d[byte_idx] >> shift) & 0x03);

		if (x < SGB_TILE_COLS && y < SGB_TILE_ROWS)
			s.attr_map[y * SGB_TILE_COLS + x] = pal;

		if (vert)
		{
			if (++y >= SGB_TILE_ROWS) { y = 0; ++x; }
		}
		else
		{
			if (++x >= SGB_TILE_COLS) { x = 0; ++y; }
		}
	}
	++s.attr_writes;
}

// ------------------------------------------------------------------
// PAL_SET — apply 4 system palettes to active slots.
// ------------------------------------------------------------------
// Data layout:
//   [1..2]  system palette number for active PAL0 (0..511, LE)
//   [3..4]  PAL1
//   [5..6]  PAL2
//   [7..8]  PAL3
//   [9]     ATF control:
//             bit 7    apply stored attribute file
//             bit 6    cancel MASK_EN
//             bits 0-5 attribute-file number (0..44)

void HandlePalSet(SgbState &s, const uint8_t *d)
{
	if (!s.system_palettes_loaded) return;

	const uint16_t idx[4] = {
		Rd16(d + 1), Rd16(d + 3), Rd16(d + 5), Rd16(d + 7)
	};
	for (int i = 0; i < 4; ++i)
	{
		if (idx[i] >= 512) continue;
		const uint16_t *src = &s.system_palettes[idx[i] * 4];
		for (int c = 0; c < 4; ++c) s.active[i].colors[c] = src[c];
	}

	const uint8_t atf_ctrl = d[9];
	if (atf_ctrl & 0x80)
	{
		ApplyAttrFile(s, static_cast<uint8_t>(atf_ctrl & 0x3F));
	}
	// Bit 6 cancels MASK_EN; P6d will honor this once mask rendering lands.
	if (atf_ctrl & 0x40) s.mask_mode = 0;

	++s.palette_writes;
}

// ------------------------------------------------------------------
// PAL_TRN — upload 512 system palettes from 4KB of GB VRAM.
// ------------------------------------------------------------------

void HandlePalTrn(SgbState &s, const uint8_t *vram)
{
	if (!vram) return;
	// 512 × (4 colors × 2 bytes) = 4096 bytes.
	for (int p = 0; p < 512; ++p)
	{
		const uint8_t *src = vram + p * 8;
		for (int c = 0; c < 4; ++c)
			s.system_palettes[p * 4 + c] = Rd16(src + c * 2);
	}
	s.system_palettes_loaded = true;
}

// ------------------------------------------------------------------
// ATTR_TRN — upload 45 attribute files (90 bytes each) from VRAM.
// ------------------------------------------------------------------

void HandleAttrTrn(SgbState &s, const uint8_t *vram)
{
	if (!vram) return;
	std::memcpy(s.attr_files, vram, 45 * 90);
	s.attr_files_loaded = true;
}

// ------------------------------------------------------------------
// ATTR_SET — apply a stored attribute file, optionally cancel MASK_EN.
// ------------------------------------------------------------------
//   [1]  bit 7 cancel MASK_EN, bits 0-5 ATF number

void HandleAttrSet(SgbState &s, const uint8_t *d)
{
	const uint8_t b = d[1];
	ApplyAttrFile(s, static_cast<uint8_t>(b & 0x3F));
	if (b & 0x40) s.mask_mode = 0;
}

} // anonymous

// ===================================================================
// Public API
// ===================================================================

void SgbReset(SgbState &s)
{
	for (int i = 0; i < 4; ++i)
		for (int c = 0; c < 4; ++c)
			s.active[i].colors[c] = DEFAULT_PAL[c];

	std::memset(s.attr_map,         0, sizeof s.attr_map);
	std::memset(s.system_palettes,  0, sizeof s.system_palettes);
	std::memset(s.attr_files,       0, sizeof s.attr_files);
	s.system_palettes_loaded = false;
	s.attr_files_loaded      = false;
	s.mask_mode              = 0;
	s.palette_writes         = 0;
	s.attr_writes            = 0;
	s.last_cmd               = 0xFF;
}

void SgbHandleCommand(SgbState &s, uint8_t cmd, const uint8_t *data,
                      uint32_t len, const uint8_t *vram_4kb)
{
	if (len < 16) return;
	s.last_cmd = cmd;

	switch (cmd)
	{
		case 0x00: HandlePal(s, 0, 1, data); break;            // PAL01
		case 0x01: HandlePal(s, 2, 3, data); break;            // PAL23
		case 0x02: HandlePal(s, 0, 3, data); break;            // PAL03
		case 0x03: HandlePal(s, 1, 2, data); break;            // PAL12
		case 0x04: HandleAttrBlk(s, data, len); break;         // ATTR_BLK
		case 0x05: HandleAttrLin(s, data, len); break;         // ATTR_LIN
		case 0x06: HandleAttrDiv(s, data); break;              // ATTR_DIV
		case 0x07: HandleAttrChr(s, data, len); break;         // ATTR_CHR
		case 0x0A: HandlePalSet(s, data); break;               // PAL_SET
		case 0x0B: HandlePalTrn(s, vram_4kb); break;           // PAL_TRN
		case 0x15: HandleAttrTrn(s, vram_4kb); break;          // ATTR_TRN
		case 0x16: HandleAttrSet(s, data); break;              // ATTR_SET
		// P6c: CHR_TRN (0x13), PCT_TRN (0x14), MASK_EN (0x17)
		// P6d: SOUND (0x08), SOU_TRN (0x09), MLT_REQ (0x11), DATA_SND (0x0F),
		//      DATA_TRN (0x10), JUMP (0x12), OBJ_TRN (0x18), ATRC_EN (0x0C),
		//      TEST_EN (0x0D), ICON_EN (0x0E)
		default: break;
	}
}

uint8_t SgbGetTilePalette(const SgbState &s, uint32_t tile_x, uint32_t tile_y)
{
	if (tile_x >= SGB_TILE_COLS || tile_y >= SGB_TILE_ROWS) return 0;
	return s.attr_map[tile_y * SGB_TILE_COLS + tile_x];
}

uint16_t SgbResolveColor(const SgbState &s, uint32_t tile_x, uint32_t tile_y,
                         uint8_t shade)
{
	const uint8_t pal = SgbGetTilePalette(s, tile_x, tile_y);
	return s.active[pal].colors[shade & 3];
}

} // namespace SGB
