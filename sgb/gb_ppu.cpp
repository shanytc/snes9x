/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Game Boy PPU — scanline-level implementation.
//
// Timing (T-cycles, per Pan Docs):
//   line = 456 dots, split as
//     mode 2 (OAM scan)       : 80 dots
//     mode 3 (pixel transfer) : 172 dots (fixed — real HW is 172..289)
//     mode 0 (HBlank)         : 204 dots
//   144 visible lines + 10 VBlank lines = 154 total. Frame = 70224 dots.
//
// The renderer runs at mode-3 entry and deposits the entire scanline
// atomically. This matches what most scanline emulators do — it passes
// dmg-acid2 and the vast majority of commercial games, but won't
// reproduce mid-scanline register-write effects used by a handful of
// demos. Pixel-FIFO accuracy is P10 territory if ever needed.
//
// OBJ-over-BG priority: the BG's raw pre-palette color (scanline_bg_raw)
// is tracked alongside the palette-mapped framebuffer so the sprite
// renderer can respect the "BG wins when BG color != 0" flag after the
// fact.
//
// STAT IRQ uses edge detection: the four sources (LYC match / mode 2 /
// mode 1 / mode 0) are ORed into a single "stat line"; we raise
// IRQ_LCDSTAT only on the 0 → 1 transition, matching real HW's single-
// wire behavior.

#include "gb_ppu.h"
#include "gb_memory.h"
#include "gb_cpu.h"
#include "sgb.h"

#include <cstring>

namespace SGB {

namespace {

// Mode durations in T-cycles for the fixed-timing model.
constexpr int32_t MODE2_DOTS = 80;
constexpr int32_t MODE3_DOTS = 172;
constexpr int32_t MODE0_DOTS = 204;
constexpr int32_t LINE_DOTS  = MODE2_DOTS + MODE3_DOTS + MODE0_DOTS;  // 456
constexpr int32_t VISIBLE_LINES = 144;
constexpr int32_t TOTAL_LINES   = 154;

inline uint8_t ApplyPalette(uint8_t palette, uint8_t color_idx)
{
	// BGP/OBP0/OBP1 are 2-bit mappings packed into 8 bits:
	//   bits 1:0 = shade for color 0, 3:2 for color 1, 5:4 for color 2, 7:6 for color 3.
	return static_cast<uint8_t>((palette >> (color_idx * 2)) & 0x03);
}

void RecomputeStatLine(Ppu &p, Memory &mem)
{
	// Rebuild the low 3 bits of STAT (mode + LYC coincidence).
	uint8_t new_stat = static_cast<uint8_t>((p.stat & 0xF8) | static_cast<uint8_t>(p.mode));
	if (p.ly == p.lyc) new_stat |= 0x04;
	p.stat = new_stat;

	// Compute the combined IRQ line from the four source-enable flags.
	bool line_high = false;
	if ((p.stat & 0x40) && (p.ly == p.lyc))                 line_high = true;
	if ((p.stat & 0x20) && p.mode == PpuMode::OamScan)       line_high = true;
	if ((p.stat & 0x10) && p.mode == PpuMode::VBlank)        line_high = true;
	if ((p.stat & 0x08) && p.mode == PpuMode::HBlank)        line_high = true;

	// Rising edge on the combined line raises LCDSTAT IRQ.
	if (line_high && !p.stat_line_high)
		mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
	p.stat_line_high = line_high;
}

void RenderBG(Ppu &p, uint8_t *line_out)
{
	const uint16_t map_base  = (p.lcdc & 0x08) ? 0x1C00 : 0x1800;  // VRAM-relative
	const bool     tiles_un  = (p.lcdc & 0x10) != 0;
	const uint8_t  bg_y      = static_cast<uint8_t>(p.ly + p.scy);
	const uint32_t tile_row  = bg_y >> 3;
	const uint32_t fine_y    = bg_y & 7;

	for (int x = 0; x < GB_SCREEN_WIDTH; ++x)
	{
		const uint8_t  bg_x     = static_cast<uint8_t>(x + p.scx);
		const uint32_t tile_col = bg_x >> 3;
		const uint32_t fine_x   = bg_x & 7;

		const uint8_t tile_num = p.vram[map_base + (tile_row * 32) + tile_col];

		uint16_t tile_addr;
		if (tiles_un)
		{
			// Unsigned mode — 0x8000 base.
			tile_addr = static_cast<uint16_t>(tile_num * 16);
		}
		else
		{
			// Signed mode — 0x9000 base (= VRAM offset 0x1000).
			tile_addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(tile_num) * 16);
		}
		tile_addr = static_cast<uint16_t>(tile_addr + fine_y * 2);

		const uint8_t lo = p.vram[tile_addr];
		const uint8_t hi = p.vram[tile_addr + 1];
		const uint8_t bit = static_cast<uint8_t>(7 - fine_x);
		const uint8_t color_idx = static_cast<uint8_t>(
			(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));

		p.scanline_bg_raw[x] = color_idx;
		p.scanline_raw[x]    = color_idx;   // raw for SGB $7800 capture
		line_out[x]          = ApplyPalette(p.bgp, color_idx);
	}
}

void RenderWindow(Ppu &p, uint8_t *line_out)
{
	// WX is offset by 7; WX=7 puts window at screen column 0.
	// Values 0..6 and 166+ are unreliable on real HW; we render what we can.
	const int wx = static_cast<int>(p.wx) - 7;
	if (wx >= GB_SCREEN_WIDTH) return;

	const uint16_t map_base = (p.lcdc & 0x40) ? 0x1C00 : 0x1800;
	const bool     tiles_un = (p.lcdc & 0x10) != 0;
	const uint32_t win_y    = static_cast<uint32_t>(p.window_line);
	const uint32_t tile_row = win_y >> 3;
	const uint32_t fine_y   = win_y & 7;

	const int start_x = (wx < 0) ? 0 : wx;

	for (int x = start_x; x < GB_SCREEN_WIDTH; ++x)
	{
		const int      win_col  = x - wx;
		const uint32_t tile_col = static_cast<uint32_t>(win_col) >> 3;
		const uint32_t fine_x   = static_cast<uint32_t>(win_col) & 7;

		const uint8_t tile_num = p.vram[map_base + (tile_row * 32) + tile_col];

		uint16_t tile_addr;
		if (tiles_un) tile_addr = static_cast<uint16_t>(tile_num * 16);
		else          tile_addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(tile_num) * 16);
		tile_addr = static_cast<uint16_t>(tile_addr + fine_y * 2);

		const uint8_t lo  = p.vram[tile_addr];
		const uint8_t hi  = p.vram[tile_addr + 1];
		const uint8_t bit = static_cast<uint8_t>(7 - fine_x);
		const uint8_t color_idx = static_cast<uint8_t>(
			(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));

		p.scanline_bg_raw[x] = color_idx;
		p.scanline_raw[x]    = color_idx;   // raw for SGB $7800 capture
		line_out[x]          = ApplyPalette(p.bgp, color_idx);
	}
}

void RenderSprites(Ppu &p, uint8_t *line_out)
{
	const bool    large     = (p.lcdc & 0x04) != 0;
	const int     sprite_h  = large ? 16 : 8;

	// OAM scan — up to 10 sprites overlapping this scanline.
	struct Hit { int x; uint8_t oam_idx; };
	Hit hits[10];
	int hit_count = 0;

	for (int i = 0; i < 40 && hit_count < 10; ++i)
	{
		const uint8_t oy = p.oam[i * 4 + 0];
		const uint8_t ox = p.oam[i * 4 + 1];
		const int top    = static_cast<int>(oy) - 16;
		const int ly     = static_cast<int>(p.ly);
		if (ly < top || ly >= top + sprite_h) continue;
		// OX of 0 or >= 168 still counts toward the 10-per-line cap but
		// draws off-screen. We skip the pixel loop later.
		hits[hit_count++] = { static_cast<int>(ox), static_cast<uint8_t>(i) };
	}

	// DMG priority: lower X wins; tie breaks on lower OAM index. We draw
	// in REVERSE priority order so higher-priority sprites overwrite.
	// Simple insertion sort to keep this stable.
	for (int i = 1; i < hit_count; ++i)
	{
		Hit cur = hits[i];
		int j = i;
		while (j > 0)
		{
			const Hit &prev = hits[j - 1];
			const bool prev_higher_pri = (prev.x < cur.x) ||
			                             (prev.x == cur.x && prev.oam_idx < cur.oam_idx);
			if (prev_higher_pri) break;
			hits[j] = hits[j - 1];
			--j;
		}
		hits[j] = cur;
	}

	// Render from lowest priority → highest, overwriting as we go.
	for (int idx = hit_count - 1; idx >= 0; --idx)
	{
		const uint8_t oi    = hits[idx].oam_idx;
		const uint8_t oy    = p.oam[oi * 4 + 0];
		const uint8_t ox    = p.oam[oi * 4 + 1];
		uint8_t       tile  = p.oam[oi * 4 + 2];
		const uint8_t flags = p.oam[oi * 4 + 3];

		int sprite_top = static_cast<int>(oy) - 16;
		int tile_row   = static_cast<int>(p.ly) - sprite_top;
		if (flags & 0x40) tile_row = sprite_h - 1 - tile_row;   // Y flip

		if (large) tile = static_cast<uint8_t>(tile & 0xFE);    // 8x16 mode: bit 0 of tile ignored
		const uint8_t sub_tile = static_cast<uint8_t>(tile + (tile_row / 8));
		const int     fine_y   = tile_row & 7;

		const uint16_t tile_addr = static_cast<uint16_t>(sub_tile * 16 + fine_y * 2);
		const uint8_t  lo        = p.vram[tile_addr];
		const uint8_t  hi        = p.vram[tile_addr + 1];

		const uint8_t palette   = (flags & 0x10) ? p.obp1 : p.obp0;
		const bool    bg_over   = (flags & 0x80) != 0;
		const int     screen_x  = static_cast<int>(ox) - 8;

		for (int px = 0; px < 8; ++px)
		{
			const int x = screen_x + px;
			if (x < 0 || x >= GB_SCREEN_WIDTH) continue;

			const int     bit       = (flags & 0x20) ? px : (7 - px);   // X flip
			const uint8_t color_idx = static_cast<uint8_t>(
				(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
			if (color_idx == 0) continue;  // sprite color 0 = transparent

			if (bg_over && p.scanline_bg_raw[x] != 0) continue;

			p.scanline_raw[x] = color_idx;   // raw sprite pixel for SGB capture
			line_out[x]       = ApplyPalette(palette, color_idx);
		}
	}
}

void RenderScanline(Ppu &p)
{
	const uint32_t y = p.ly;
	if (y >= GB_SCREEN_HEIGHT) return;

	uint8_t *const line = &p.framebuffer[y * GB_SCREEN_WIDTH];

	if (p.lcdc & 0x01)
	{
		RenderBG(p, line);

		if ((p.lcdc & 0x20) && y >= p.wy)
		{
			RenderWindow(p, line);
			++p.window_line;
		}
	}
	else
	{
		// BG/window master off — fill with color 0 (post-palette).
		std::memset(line,                 ApplyPalette(p.bgp, 0), GB_SCREEN_WIDTH);
		std::memset(p.scanline_bg_raw, 0, GB_SCREEN_WIDTH);
		std::memset(p.scanline_raw,    0, GB_SCREEN_WIDTH);
	}

	if (p.lcdc & 0x02) RenderSprites(p, line);

	// Snapshot pre-palette raw indices for the SGB BIOS-less border
	// capture path. End-of-frame, the capture decodes a 128x128 area
	// from raw_framebuffer back into 4 KB of CHR_TRN/PCT_TRN bytes.
	std::memcpy(&p.raw_framebuffer[y * GB_SCREEN_WIDTH],
	            p.scanline_raw, GB_SCREEN_WIDTH);

	// SGB BIOS-mode hook: capture post-BGP/OBP shade values (what the
	// real GB's LCD would show). SameBoy's ICD pixel callback delivers
	// the same — post-palette value 0-3 — and bsnes stores those
	// directly as the "color" it shifts into $7800 bit-planes.
	S9xSGBCaptureScanline(line);
}

} // anonymous

void PpuReset(Ppu &p)
{
	std::memset(p.vram,        0, sizeof p.vram);
	std::memset(p.oam,         0, sizeof p.oam);
	std::memset(p.framebuffer,     0, sizeof p.framebuffer);
	std::memset(p.raw_framebuffer, 0, sizeof p.raw_framebuffer);
	std::memset(p.scanline_bg_raw, 0, sizeof p.scanline_bg_raw);
	std::memset(p.scanline_raw,    0, sizeof p.scanline_raw);
	p.lcdc = 0x91;   // LCD on, BG on, BG tile data at 0x8000, BG map at 0x9800.
	p.stat = 0x85;
	p.scy = p.scx = p.ly = p.lyc = 0;
	p.bgp = 0xFC; p.obp0 = 0xFF; p.obp1 = 0xFF;
	p.wy = p.wx = 0;
	p.mode = PpuMode::OamScan;
	p.mode_clock    = 0;
	p.window_line   = 0;
	p.stat_line_high = false;
	p.frame_ready   = false;
	p.t_cycles      = 0;
}

void PpuStep(Ppu &p, Memory &mem, int32_t tcycles)
{
	if (tcycles > 0) p.t_cycles += tcycles;

	// LCD master disable (LCDC bit 7). Real HW parks the PPU in mode 0
	// with LY=0 until the bit toggles back on. We keep the framebuffer
	// contents so the display keeps showing the last valid frame.
	//
	// Crucially: STAT IRQs do NOT fire while the LCD is off. The STAT
	// line is forced low, which means we must NOT call RecomputeStatLine
	// here — that function's "rising edge" detector would see a live
	// HBlank (mode=0) every call and raise IRQ_LCDSTAT on every CPU
	// instruction. Pokemon Yellow's Pikachu-voice routine disables the
	// LCD with STAT bit 3 (HBlank IRQ) still enabled, and we were
	// storm-firing its ISR forever at PC=15C1 (one instruction before
	// RETI, right after POP AF).
	if (!(p.lcdc & 0x80))
	{
		p.mode           = PpuMode::HBlank;
		p.ly             = 0;
		p.mode_clock     = 0;
		p.window_line    = 0;
		p.stat_line_high = false;
		// Zero the STAT mode + coincidence bits (low 3); keep the IRQ
		// enable bits (4..6) because the game's register writes still
		// work. Intentionally NOT raising IRQ_LCDSTAT.
		p.stat = static_cast<uint8_t>(p.stat & 0xF8);
		(void)mem;
		return;
	}

	p.mode_clock += tcycles;

	// Cascade through every mode transition the accumulated mode_clock
	// covers, not just the first. A single PpuStep call can be handed a
	// large enough delta to cross multiple mode boundaries (OAM-scan →
	// transfer → HBlank → next-line OAM-scan); a one-shot switch would
	// only fire the first transition and leave the rest as phantom
	// mode_clock leftover, dropping scanline transitions on the floor.
	for (;;)
	{
		bool transitioned = false;
		switch (p.mode)
		{
		case PpuMode::OamScan:
			if (p.mode_clock >= MODE2_DOTS)
			{
				p.mode_clock -= MODE2_DOTS;
				p.mode        = PpuMode::Transfer;
				// Render at mode-3 entry (simplification — see top-of-file note).
				RenderScanline(p);
				transitioned = true;
			}
			break;

		case PpuMode::Transfer:
			if (p.mode_clock >= MODE3_DOTS)
			{
				p.mode_clock -= MODE3_DOTS;
				p.mode        = PpuMode::HBlank;
				transitioned  = true;
			}
			break;

		case PpuMode::HBlank:
			if (p.mode_clock >= MODE0_DOTS)
			{
				p.mode_clock -= MODE0_DOTS;
				++p.ly;
				// bsnes ICD::ppuHreset equivalent — advance $6000 row/bank
				// from GB PPU. Frozen while GB halted (callback only fires
				// when GB actually ticks).
				S9xSGBOnPpuHBlank();
				if (p.ly == VISIBLE_LINES)
				{
					p.mode          = PpuMode::VBlank;
					p.frame_ready   = true;
					p.window_line   = 0;
					mem.if_         = static_cast<uint8_t>(mem.if_ | IRQ_VBLANK);
					S9xSGBOnPpuVBlank();  // bsnes ICD::ppuVreset
				}
				else
				{
					p.mode = PpuMode::OamScan;
				}
				transitioned = true;
			}
			break;

		case PpuMode::VBlank:
			if (p.mode_clock >= LINE_DOTS)
			{
				p.mode_clock -= LINE_DOTS;
				++p.ly;
				if (p.ly >= TOTAL_LINES)
				{
					p.ly   = 0;
					p.mode = PpuMode::OamScan;
				}
				transitioned = true;
			}
			break;
		}
		if (!transitioned) break;
		// STAT IRQ uses edge detection — must run after every transition
		// inside the cascade, NOT just once at the end. With one large
		// PpuStep delta the cascade can fire 5+ scanline transitions in
		// a single call; if RecomputeStatLine runs only at the end, the
		// GB game's HBlank/mode-2/LYC-match STAT handlers miss the inter-
		// mediate edges → game state drifts → packet timing to SGB BIOS
		// shifts → BIOS writes $6001 17 or 19 times instead of 18.
		RecomputeStatLine(p, mem);
	}
}

uint8_t PpuReadReg(const Ppu &p, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF40: return p.lcdc;
		case 0xFF41: return static_cast<uint8_t>(p.stat | 0x80);  // bit 7 always 1
		case 0xFF42: return p.scy;
		case 0xFF43: return p.scx;
		case 0xFF44: return p.ly;
		case 0xFF45: return p.lyc;
		case 0xFF47: return p.bgp;
		case 0xFF48: return p.obp0;
		case 0xFF49: return p.obp1;
		case 0xFF4A: return p.wy;
		case 0xFF4B: return p.wx;
	}
	return 0xFF;
}

void PpuWriteReg(Ppu &p, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF40:
		{
			const bool was_on = (p.lcdc & 0x80) != 0;
			p.lcdc = value;
			const bool is_on  = (p.lcdc & 0x80) != 0;
			// LCD turn-on resets to line 0 / mode 2.
			if (!was_on && is_on)
			{
				p.ly          = 0;
				p.mode        = PpuMode::OamScan;
				p.mode_clock  = 0;
				p.window_line = 0;
			}
			break;
		}
		case 0xFF41:
			// Low 3 bits read-only; high 4 are IRQ enables.
			p.stat = static_cast<uint8_t>((p.stat & 0x07) | (value & 0x78));
			break;
		case 0xFF42: p.scy = value; break;
		case 0xFF43: p.scx = value; break;
		case 0xFF44: p.ly  = 0;     break;  // Pan Docs: any write resets LY
		case 0xFF45: p.lyc = value; break;
		case 0xFF47: p.bgp  = value; break;
		case 0xFF48: p.obp0 = value; break;
		case 0xFF49: p.obp1 = value; break;
		case 0xFF4A: p.wy = value;   break;
		case 0xFF4B: p.wx = value;   break;
	}
}

} // namespace SGB
