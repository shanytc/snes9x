/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Game Boy PPU — per-pixel implementation (one render call per dot).
//
// Timing (T-cycles, per Pan Docs):
//   line = 456 dots, split as
//     mode 2 (OAM scan)       : 80 dots
//     mode 3 (pixel transfer) : 172 + sprite_count*6 + (SCX & 7) dots
//                               (12-dot setup + 6 dots per OAM-scan-hit
//                               sprite + 1 dot per pixel the BG fetcher
//                               has to discard for SCX-fine alignment,
//                               then 160 pixels emit, total 172..239)
//     mode 0 (HBlank)         : 204 - sprite_count*6 - (SCX & 7) dots
//   144 visible lines + 10 VBlank lines = 154 total. Frame = 70224 dots.
//
// Mode 3 timing model:
//   The first 12 dots of mode 3 are setup (BG fetcher fills its 16-pixel
//   shift register with two tiles before pixel 0 emits). On lines with
//   sprites, the fetcher additionally stalls ~6 dots per sprite covering
//   the scanline while reading sprite tile data. When SCX is not
//   tile-aligned (SCX & 7 != 0), the fetcher discards (SCX & 7) pixels
//   from the first tile before emitting pixel 0 — each discarded pixel
//   costs 1 dot. Pixels start emitting at
//     mode_clock = 12 + sprite_count*6 + (SCX & 7)
//   and the LAST pixel emits 160 dots later. Mode 0 shrinks by the same
//   total to keep the scanline at 456 dots. We model the stall as a
//   single upfront block (computed at mode 2 → 3 transition from
//   sprite_count and the SCX latched at that boundary) rather than the
//   per-sprite-position pipeline — close enough that STAT-IRQ handler
//   writes land at the same effective pixel boundary as real HW.
//
// PpuStep iterates one GB t-cycle at a time. During mode 3 the renderer
// emits ONE pixel per dot (RenderPixel), re-sampling LCDC/SCX/SCY/OBP0/
// OBP1/WY at each pixel — this catches mid-LY register writes that some
// games use for parallax (Animaniacs cloud strip), palette effects, and
// partial-line scroll. WX is an exception: it is latched at mode 2 → 3
// and held for the line, so STAT-handler-driven mid-mode-3 writes apply
// to the NEXT line. This matches the 8-dot tile-boundary granularity
// real DMG uses for window engage (rather than per-pixel). Without WX
// latching, games that toggle WX from a STAT IRQ to engage/disengage
// the window for a dialog overlay (One Piece - Maboroshi no Grand Line)
// tear the boundary scanlines. BGP is also latched at mode 2 → 3 but a
// write landing during mode 3 re-latches immediately (applies from the
// current pixel on) — see Ppu::latched_bgp for the Initial D Gaiden /
// RoboCop trade-off this resolves.
//
// OBJ-over-BG priority: the per-pixel BG raw value (scanline_bg_raw) is
// tracked alongside the palette-mapped framebuffer so SampleSpritePixel
// can respect the "BG wins when BG color != 0" flag.
//
// STAT IRQ uses edge detection: the four sources (LYC match / mode 2 /
// mode 1 / mode 0) are ORed into a single "stat line"; we raise
// IRQ_LCDSTAT only on the 0 → 1 transition, matching real HW's single-
// wire behavior.

#include "gb_ppu.h"
#include "gb_memory.h"
#include "gb_joypad.h"
#include "gb_cpu.h"
#include "sgb.h"

#include <cstring>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>

extern int g_cam_countdown;
extern uint8_t g_cam_shade[128 * 112];
extern int g_cam_live;
extern int g_cam_brightness;

namespace SGB {

namespace {

// Mode 3 base lengths; per-scanline length shifts by p.mode3_sprite_stall.
constexpr int32_t MODE2_DOTS        = 80;
constexpr int32_t MODE3_DOTS        = 172;
constexpr int32_t MODE0_DOTS        = 204;
constexpr int32_t MODE3_SETUP_DOTS  = GB_MODE3_SETUP_DOTS;
constexpr int32_t SPRITE_STALL_DOTS = 6;
constexpr int32_t LINE_DOTS  = MODE2_DOTS + MODE3_DOTS + MODE0_DOTS;  // 456
constexpr int32_t VISIBLE_LINES = 144;
constexpr int32_t TOTAL_LINES   = 154;

// VBlank IF latches at the LY=144 dot minus this lead: cpu->t_cycles stamps
// instruction START but DMG samples IF on the LAST M-cycle (-8..-20 all match SameBoy).
static int64_t GB_VBLANK_IRQ_OFFSET = 7;   // env ACID_VB while tuning

inline uint8_t ApplyPalette(uint8_t palette, uint8_t color_idx)
{
	// BGP/OBP0/OBP1 are 2-bit mappings packed into 8 bits:
	//   bits 1:0 = shade for color 0, 3:2 for color 1, 5:4 for color 2, 7:6 for color 3.
	return static_cast<uint8_t>((palette >> (color_idx * 2)) & 0x03);
}

void RecomputeStatLine(Ppu &p, Memory &mem, bool defer_irq = false)
{
	// Rebuild the low 3 bits of STAT (mode + LYC coincidence). With the LCD
	// off the PPU comparator is frozen, so the coincidence flag latches the
	// value it held when the LCD was disabled rather than being recomputed
	// from the parked LY. Mr. Do! disables the LCD at line 145 (LYC=145
	// match set), loads VRAM, then re-waits for that same match while the
	// LCD is still off before re-enabling it; recomputing here would see
	// LY(0) != LYC(145), clear bit 2 and hang the wait forever.
	const bool lyc_match = (p.ly == p.lyc) && !p.lyc_relatch;
	uint8_t new_stat = static_cast<uint8_t>((p.stat & 0xF8) | static_cast<uint8_t>(p.mode));
	if (p.lcdc & 0x80)
	{
		if (lyc_match) new_stat |= 0x04;
	}
	else
	{
		new_stat |= static_cast<uint8_t>(p.stat & 0x04);
	}
	p.stat = new_stat;

	// Compute the combined IRQ line from the four source-enable flags.
	// When the LCD is off (LCDC.bit7 = 0), real DMG holds the STAT IRQ
	// line low regardless of the source-enable bits and the PPU state
	// (mode/LY/LYC are parked). PpuStep mirrors this by forcing mode 0 /
	// LY=0 with the LCD off, but writes to FF41/FF44/FF45 also call
	// RecomputeStatLine — without this LCDC guard those writes would see
	// "mode 0 active + bit 3 enabled" or "LY=LYC=0 + bit 6 enabled" and
	// raise a STAT IRQ that real hardware never fires while the LCD is
	// disabled. Zerd no Densetsu's bank-1 init exposed this: it writes
	// STAT.bit6 inside a DI region with the LCD off via $02CF; without
	// the guard the spurious raise leaks into the post-EI flow.
	bool line_high = false;
	if (p.lcdc & 0x80)
	{
		if ((p.stat & 0x40) && lyc_match)                       line_high = true;
		if ((p.stat & 0x20) && p.mode == PpuMode::OamScan && !p.lcdon_first)
		                                                         line_high = true;
		if ((p.stat & 0x10) && p.mode == PpuMode::VBlank)        line_high = true;
		if ((p.stat & 0x08) && p.mode == PpuMode::HBlank)        line_high = true;
	}
	else if ((p.stat & 0x40) && (p.stat & 0x04))
	{
		// LCD off: the comparator is frozen but its latched flag still
		// drives the LYC source, so re-enabling with a still-true match
		// produces no fresh edge (mooneye stat_lyc_onoff round 2).
		line_high = true;
	}

	// Rising edge on the combined line raises LCDSTAT IRQ.
	if (line_high && !p.stat_line_high)
	{
		if (defer_irq)
		{
			static int d2 = -1, d0 = -1, dv = -1, dl = -1;
			if (d2 < 0)
			{
				const char *e;
				d2 = (e = getenv("ACID_D2")) ? atoi(e) : 7;
				d0 = (e = getenv("ACID_D0")) ? atoi(e) : 1;
				dv = (e = getenv("ACID_DV")) ? atoi(e) : 9;
				dl = (e = getenv("ACID_DL")) ? atoi(e) : -1;   // -1 = mode-based
				static int dlc = -2;
				if (dlc < -1) { const char *f = getenv("ACID_DLC"); dlc = f ? atoi(f) : dl; }
				if (mem.cgb_hw) dl = dlc;
			}
			// An edge driven purely by the LYC comparator reaches IF one
			// dot before the mode-2 source (daid ppu_scanline_bgp's
			// halt-wake grid is anchored to it, unquantized).
			const bool lyc_driven = (p.stat & 0x40) && lyc_match &&
				!(((p.stat & 0x20) && p.mode == PpuMode::OamScan && !p.lcdon_first) ||
				  ((p.stat & 0x10) && p.mode == PpuMode::VBlank) ||
				  ((p.stat & 0x08) && p.mode == PpuMode::HBlank));
			int d = (p.mode == PpuMode::OamScan) ? d2
			      : (p.mode == PpuMode::HBlank)  ? d0 : dv;
			if (lyc_driven && dl >= 0)
				d = dl;
			if (d <= 0)
				mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
			else if (p.stat_irq_delay == 0)
				p.stat_irq_delay = d;
		}
		else
			mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
	}
	p.stat_line_high = line_high;
}

// Re-latch the LY==LYC comparator at a scanline boundary. First recompute
// with the coincidence suppressed (drops the STAT line if LYC was the only
// source holding it high), then the caller's end-of-step RecomputeStatLine
// re-asserts it so a still-valid LY==LYC match produces a fresh 0->1 edge.
// Models the real-hardware "LyForCompare briefly = -1" window that lets a
// chained-LYC raster effect keep firing even while the HBlank STAT IRQ holds
// the line high otherwise. See Ppu::lyc_relatch.
inline void RelatchLyc(Ppu &p, Memory &mem)
{
	p.lyc_relatch = true;
	RecomputeStatLine(p, mem, true);
	p.lyc_relatch = false;
}

// Sample one BG pixel at (x, ly) using the CURRENT register values.
// Inlined so the per-dot path doesn't pay function-call overhead per pixel.
inline uint8_t SampleBgPixel(const Ppu &p, int x)
{
	const uint16_t map_base  = (p.lcdc & 0x08) ? 0x1C00 : 0x1800;
	const bool     tiles_un  = (p.lcdc & 0x10) != 0;
	const uint8_t  bg_y      = static_cast<uint8_t>(p.ly + p.fetch_scy);
	const uint32_t tile_row  = bg_y >> 3;
	const uint32_t fine_y    = bg_y & 7;
	const uint8_t  bg_x      = static_cast<uint8_t>(x + p.scx);
	const uint32_t tile_col  = bg_x >> 3;
	const uint32_t fine_x    = bg_x & 7;

	const uint8_t tile_num = p.vram[map_base + (tile_row * 32) + tile_col];

	uint16_t tile_addr;
	if (tiles_un)
		tile_addr = static_cast<uint16_t>(tile_num * 16);
	else
		tile_addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(tile_num) * 16);
	tile_addr = static_cast<uint16_t>(tile_addr + fine_y * 2);

	const uint8_t lo  = p.vram[tile_addr];
	const uint8_t hi  = p.vram[tile_addr + 1];
	const uint8_t bit = static_cast<uint8_t>(7 - fine_x);
	return static_cast<uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

// Sample one window pixel. Uses latched_wx (sampled at mode 2 → 3)
// rather than live p.wx — caller already confirmed engage with the
// same latched value.
inline uint8_t SampleWindowPixel(const Ppu &p, int x)
{
	const uint16_t map_base = (p.lcdc & 0x40) ? 0x1C00 : 0x1800;
	const bool     tiles_un = (p.lcdc & 0x10) != 0;
	const uint32_t win_y    = static_cast<uint32_t>(p.om.window_line);
	const uint32_t tile_row = win_y >> 3;
	const uint32_t fine_y   = win_y & 7;
	const int      wx       = static_cast<int>(p.latched_wx) - 7;
	const int      win_col  = x - wx;
	const uint32_t tile_col = static_cast<uint32_t>(win_col) >> 3;
	const uint32_t fine_x   = static_cast<uint32_t>(win_col) & 7;

	const uint8_t tile_num = p.vram[map_base + (tile_row * 32) + tile_col];

	uint16_t tile_addr;
	if (tiles_un)
		tile_addr = static_cast<uint16_t>(tile_num * 16);
	else
		tile_addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(tile_num) * 16);
	tile_addr = static_cast<uint16_t>(tile_addr + fine_y * 2);

	const uint8_t lo  = p.vram[tile_addr];
	const uint8_t hi  = p.vram[tile_addr + 1];
	const uint8_t bit = static_cast<uint8_t>(7 - fine_x);
	return static_cast<uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

// Pull a per-LY sprite list at mode 2 → mode 3. Up to 10 sprites in OAM
// scan order (matches DMG hardware), pre-sorted by DMG draw priority
// (lowest X first, ties by OAM index). Render-time uses this list to
// answer "is there a sprite covering pixel x?" with a linear scan.
void EvalSprites(Ppu &p, Memory &mem)
{
	const bool large    = (p.lcdc & 0x04) != 0;
	const int  sprite_h = large ? 16 : 8;

	// Scan slots that land while an OAM DMA runs read nothing at all: the
	// PPU's Y/X bus keeps its stale bytes and every such slot evaluates
	// them again (SameBoy add_object_from_index skips the reads during
	// DMA; ashiepaws/strikethrough). The scan already happened dot-by-dot
	// when we latch here, so rebuild per-slot DMA activity from the
	// transfer schedule (1 byte per 4 dots).
	static int scb = -1, dph = -1;
	if (scb < 0) { const char *e = getenv("ACID_SCB"); scb = e ? atoi(e) : 4;
	               const char *f = getenv("ACID_DPH"); dph = f ? atoi(f) : 0; }
	const bool dma_scan = mem.dma_active && !p.cgb;
	const int  eval_dot = p.mode_clock + 80;

	// Hardware keeps the first 10 hits in OAM index order and drops the rest,
	// so an over-budget line loses its highest-index objects. no_sprite_limit
	// keeps them all (Emulator Hacks): mode 3 still bills 10, see below.
	const int limit = p.no_sprite_limit
		? static_cast<int>(sizeof p.sprites / sizeof p.sprites[0])
		: GB_OAM_SCAN_LIMIT;

	p.sprite_count = 0;
	for (int i = 0; i < 40 && p.sprite_count < limit; ++i)
	{
		uint8_t oy, ox;
		bool stale = false;
		if (dma_scan)
		{
			const int back = eval_dot - (scb + 2 * i);
			const int dest = mem.dma_index - ((back + dph) >> 2);
			stale = dest >= 0 && dest <= 0xA0;
			if (!stale && dest < 0)
			{
				// slot read before the transfer went live: original bytes
				p.scan_y_bus = mem.dma_oam_old[i * 4 + 0];
				p.scan_x_bus = mem.dma_oam_old[i * 4 + 1];
			}
		}
		if (!stale && !dma_scan)
		{
			p.scan_y_bus = p.oam[i * 4 + 0];
			p.scan_x_bus = p.oam[i * 4 + 1];
		}
		else if (!stale && dma_scan)
		{
			// slot read after the transfer finished: live bytes
			p.scan_y_bus = p.oam[i * 4 + 0];
			p.scan_x_bus = p.oam[i * 4 + 1];
		}
		oy = p.scan_y_bus;
		ox = p.scan_x_bus;
		const int top = static_cast<int>(oy) - 16;
		const int ly  = static_cast<int>(p.ly);
		if (ly < top || ly >= top + sprite_h) continue;
		Ppu::SpriteHit &h = p.sprites[p.sprite_count++];
		h.x       = static_cast<int16_t>(static_cast<int>(ox) - 8);
		h.oam_idx = static_cast<uint8_t>(i);
		h.y       = oy;
	}

	// The list stays in OAM-scan order: the FIFO's fetch order gives DMG
	// X-priority and OAM-index priority naturally (first fetch wins the
	// opaque FIFO slots).
}

// Sample one sprite-covering-pixel value using CURRENT registers. Returns
// {covered, sprite_color_2bit, sprite_palette_8bit, bg_priority} via out
// args. covered=false means no sprite covers this pixel or all covering
// sprites are color-0 (transparent).
struct SpritePixel { bool covered; uint8_t color; uint8_t palette; bool bg_over; };

SpritePixel SampleSpritePixel(const Ppu &p, int x)
{
	SpritePixel out{ false, 0, 0, false };
	if (!(p.lcdc & 0x02)) return out;

	const bool large    = (p.lcdc & 0x04) != 0;
	const int  sprite_h = large ? 16 : 8;

	// First sprite in priority order whose pixel at x is non-transparent wins.
	for (uint8_t i = 0; i < p.sprite_count; ++i)
	{
		const int sx = p.sprites[i].x;
		if (x < sx || x >= sx + 8) continue;

		const uint8_t oi    = p.sprites[i].oam_idx;
		const uint8_t oy    = p.oam[oi * 4 + 0];
		uint8_t       tile  = p.oam[oi * 4 + 2];
		const uint8_t flags = p.oam[oi * 4 + 3];

		int sprite_top = static_cast<int>(oy) - 16;
		int tile_row   = static_cast<int>(p.ly) - sprite_top;
		if (flags & 0x40) tile_row = sprite_h - 1 - tile_row;

		if (large) tile = static_cast<uint8_t>(tile & 0xFE);
		const uint8_t sub_tile = static_cast<uint8_t>(tile + (tile_row / 8));
		const int     fine_y   = tile_row & 7;

		const uint16_t tile_addr = static_cast<uint16_t>(sub_tile * 16 + fine_y * 2);
		const uint8_t  lo        = p.vram[tile_addr];
		const uint8_t  hi        = p.vram[tile_addr + 1];

		const int     px        = x - sx;
		const int     bit       = (flags & 0x20) ? px : (7 - px);
		const uint8_t color_idx = static_cast<uint8_t>(
			(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
		if (color_idx == 0) continue;   // transparent — try next sprite

		out.covered  = true;
		out.color    = color_idx;
		out.palette  = (flags & 0x10) ? p.obp1 : p.obp0;
		out.bg_over  = (flags & 0x80) != 0;
		return out;
	}
	return out;
}

inline uint16_t CgbColor(const uint8_t *pal, uint8_t palette, uint8_t color)
{
	const int idx = palette * 8 + color * 2;
	return static_cast<uint16_t>((pal[idx] | (pal[idx + 1] << 8)) & 0x7FFF);
}

struct BgPixelCgb { uint8_t color; uint8_t pal; bool priority; };

inline BgPixelCgb SampleBgPixelCgb(const Ppu &p, int x, bool window)
{
	uint32_t tile_row, fine_y, tile_col, fine_x;
	uint16_t map_base;
	if (window)
	{
		map_base = (p.lcdc & 0x40) ? 0x1C00 : 0x1800;
		const uint32_t win_y = static_cast<uint32_t>(p.om.window_line);
		tile_row = win_y >> 3;
		fine_y   = win_y & 7;
		const int win_col = x - (static_cast<int>(p.latched_wx) - 7);
		tile_col = static_cast<uint32_t>(win_col) >> 3;
		fine_x   = static_cast<uint32_t>(win_col) & 7;
	}
	else
	{
		map_base = (p.lcdc & 0x08) ? 0x1C00 : 0x1800;
		const uint8_t bg_y = static_cast<uint8_t>(p.ly + p.fetch_scy);
		tile_row = bg_y >> 3;
		fine_y   = bg_y & 7;
		const uint8_t bg_x = static_cast<uint8_t>(x + p.scx);
		tile_col = bg_x >> 3;
		fine_x   = bg_x & 7;
	}

	const uint16_t map_idx  = static_cast<uint16_t>(map_base + tile_row * 32 + tile_col);
	const uint8_t  tile_num = p.vram[map_idx];
	const uint8_t  attr     = p.vram[0x2000 + map_idx];

	const uint32_t databank = (attr & 0x08) ? 0x2000u : 0u;
	if (attr & 0x40) fine_y = 7 - fine_y;

	uint16_t tile_addr;
	if (p.lcdc & 0x10)
		tile_addr = static_cast<uint16_t>(tile_num * 16);
	else
		tile_addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(tile_num) * 16);
	tile_addr = static_cast<uint16_t>(tile_addr + fine_y * 2);

	const uint8_t lo  = p.vram[databank + tile_addr];
	const uint8_t hi  = p.vram[databank + tile_addr + 1];
	const uint8_t bit = (attr & 0x20) ? static_cast<uint8_t>(fine_x)
	                                  : static_cast<uint8_t>(7 - fine_x);
	const uint8_t color = static_cast<uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
	return BgPixelCgb{ color, static_cast<uint8_t>(attr & 0x07), (attr & 0x80) != 0 };
}

struct SpritePixelCgb { bool covered; uint8_t color; uint8_t pal; bool bg_over; };

inline SpritePixelCgb SampleSpritePixelCgb(const Ppu &p, int x)
{
	SpritePixelCgb out{ false, 0, 0, false };
	if (!(p.lcdc & 0x02)) return out;

	const bool large    = (p.lcdc & 0x04) != 0;
	const int  sprite_h = large ? 16 : 8;

	for (uint8_t i = 0; i < p.sprite_count; ++i)
	{
		const int sx = p.sprites[i].x;
		if (x < sx || x >= sx + 8) continue;

		const uint8_t oi    = p.sprites[i].oam_idx;
		const uint8_t oy    = p.oam[oi * 4 + 0];
		uint8_t       tile  = p.oam[oi * 4 + 2];
		const uint8_t flags = p.oam[oi * 4 + 3];

		int sprite_top = static_cast<int>(oy) - 16;
		int tile_row   = static_cast<int>(p.ly) - sprite_top;
		if (flags & 0x40) tile_row = sprite_h - 1 - tile_row;

		if (large) tile = static_cast<uint8_t>(tile & 0xFE);
		const uint8_t  sub_tile = static_cast<uint8_t>(tile + (tile_row / 8));
		const int      fine_y   = tile_row & 7;
		const uint32_t databank = (flags & 0x08) ? 0x2000u : 0u;

		const uint16_t tile_addr = static_cast<uint16_t>(sub_tile * 16 + fine_y * 2);
		const uint8_t  lo        = p.vram[databank + tile_addr];
		const uint8_t  hi        = p.vram[databank + tile_addr + 1];

		const int     px        = x - sx;
		const int     bit       = (flags & 0x20) ? px : (7 - px);
		const uint8_t color_idx = static_cast<uint8_t>(
			(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
		if (color_idx == 0) continue;

		out.covered = true;
		out.color   = color_idx;
		out.pal     = static_cast<uint8_t>(flags & 0x07);
		out.bg_over = (flags & 0x80) != 0;
		return out;
	}
	return out;
}

void RenderPixelCgb(Ppu &p, int x)
{
	uint8_t  *const line  = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];
	uint8_t  *const lay   = &p.layer[p.ly * GB_SCREEN_WIDTH];
	uint16_t *const cline = &p.color_fb[p.ly * GB_SCREEN_WIDTH];

	// Window engages once per scanline, latched at the WX trigger column
	// (real-hw x==WX comparator) rather than a running x>=WX test, so a
	// late mid-mode-3 LCDC.5 enable can't start the window partway across
	// and tear the line. See RenderPixel for the full rationale.
	const int wx        = static_cast<int>(p.latched_wx) - 7;
	const int trigger_x = wx < 0 ? 0 : wx;
	if (!p.window_active && x == trigger_x &&
		(p.lcdc & 0x20) != 0 &&
		p.wy_triggered)
	{
		p.window_active  = true;
		p.window_start_x = static_cast<int16_t>(x);
	}
	const bool win_active_here = p.window_active && (p.lcdc & 0x20) != 0;

	const BgPixelCgb bg = SampleBgPixelCgb(p, x, win_active_here);
	const bool bg_hidden = win_active_here ? !p.show_window : !p.show_bg;

	p.scanline_bg_raw[x] = bg.color;
	p.scanline_raw[x]    = bg.color;
	if (!p.present_hold)
	{
		line[x]  = bg.color;
		lay[x]   = win_active_here ? GB_PIXEL_WINDOW : GB_PIXEL_BG;
		cline[x] = bg_hidden ? CgbColor(p.bg_pal, 0, 0)
		                     : CgbColor(p.bg_pal, bg.pal, bg.color);
	}

	const SpritePixelCgb sp = SampleSpritePixelCgb(p, x);
	if (p.show_obj && sp.covered)
	{
		const bool bg_master = (p.lcdc & 0x01) != 0;
		const bool bg_wins   = !bg_hidden && bg_master && bg.color != 0 && (bg.priority || sp.bg_over);
		if (!bg_wins)
		{
			p.scanline_raw[x] = sp.color;
			if (!p.present_hold)
			{
				line[x]  = sp.color;
				lay[x]   = GB_PIXEL_OBJ;
				cline[x] = CgbColor(p.obj_pal, sp.pal, sp.color);
			}
		}
	}
}

// Render exactly one pixel at p.draw_x for the current LY. Re-samples
// every relevant register so mid-LY changes (SCX/SCY/BGP/LCDC/WX/etc.)
// take effect at the right pixel, matching real hardware's per-dot
// fetch behavior.
void RenderPixel(Ppu &p)
{
	const int x = static_cast<int>(p.draw_x);
	if (x < 0 || x >= GB_SCREEN_WIDTH) return;
	if (p.ly >= GB_SCREEN_HEIGHT)      return;

	if (p.cgb) { RenderPixelCgb(p, x); return; }

	uint8_t *const line = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];
	uint8_t *const lay  = &p.layer[p.ly * GB_SCREEN_WIDTH];

	// BG / window resolve.
	uint8_t bg_color = 0;   // raw 2-bit pre-palette
	uint8_t bg_layer = GB_PIXEL_BG;   // GB_PIXEL_BG (or blank) / GB_PIXEL_WINDOW
	if (p.lcdc & 0x01)
	{
		// The window engages ONCE per scanline, latched the moment X reaches
		// the WX trigger column — matching real-hw's x==WX comparator, not a
		// running x>=WX test. A later mid-mode-3 LCDC.5 (window enable) write
		// can't start the window: its trigger column already passed. Without
		// this, a STAT handler that re-enables the window late tears the line
		// from the write pixel on — Star Trek 25th Anniversary's credit "TV
		// monitor" re-enables the window at ~pixel 48 of line 103, streaking
		// the screen's bottom edge.
		const int wx        = static_cast<int>(p.latched_wx) - 7;
		const int trigger_x = wx < 0 ? 0 : wx;
		if (!p.window_active && x == trigger_x &&
			(p.lcdc & 0x20) != 0 &&
			p.wy_triggered)
		{
			p.window_active  = true;
			p.window_start_x = static_cast<int16_t>(x);
		}
		const bool win_active_here = p.window_active && (p.lcdc & 0x20) != 0;

		if (win_active_here)
		{
			bg_color = SampleWindowPixel(p, x);
			bg_layer = GB_PIXEL_WINDOW;
		}
		else
		{
			// DMG window-to-background pixel-shift glitch (Windesync-validate
			// test ROM): once the window has been active earlier in the frame,
			// every later line on which the window is DISABLED gets one extra
			// blank pixel inserted into the BG FIFO at the window-X "hit"
			// column (WX-7) — shifting the rest of the BG line right by one —
			// when (WX & 7) == 7 - (SCX & 7). DMG only. Star Trek 25th
			// Anniversary's credit "TV monitor" relies on this: it puts the
			// frame's top/bottom on the window and the sides on the BG, and the
			// glitch is what lines the BG sides up with the window (without it
			// the inner screen is 1px left of the frame).
			const int hit = static_cast<int>(p.latched_wx) - 7;
			if (p.om.window_line > 1 && (p.lcdc & 0x20) == 0 &&
				hit >= 0 && hit < GB_SCREEN_WIDTH &&
				(static_cast<int>(p.latched_wx) & 7) ==
					7 - (static_cast<int>(p.scx) & 7))
			{
				if (x == hit)     bg_color = 0;                       // inserted blank
				else if (x > hit) bg_color = SampleBgPixel(p, x - 1); // shifted +1
				else              bg_color = SampleBgPixel(p, x);
			}
			else
			{
				bg_color = SampleBgPixel(p, x);
			}
		}
	}

	const bool bg_hidden = (bg_layer == GB_PIXEL_WINDOW) ? !p.show_window : !p.show_bg;
	const uint8_t disp_bg = bg_hidden ? 0 : bg_color;

	p.scanline_bg_raw[x] = bg_color;
	p.scanline_raw[x]    = bg_color;
	if (!p.present_hold)
	{
		line[x] = ApplyPalette(p.latched_bgp, disp_bg);
		lay[x]  = bg_layer;
	}

	// Sprite resolve — overwrite if visible.
	const SpritePixel sp = SampleSpritePixel(p, x);
	if (p.show_obj && sp.covered && (!sp.bg_over || disp_bg == 0))
	{
		p.scanline_raw[x] = sp.color;
		if (!p.present_hold)
		{
			line[x] = ApplyPalette(sp.palette, sp.color);
			lay[x]  = GB_PIXEL_OBJ;
		}
	}
}

// Called once at the mode 3 → HBlank transition (after all 160 pixels
// have been emitted by RenderPixel). Snapshots the raw indices for the
// SGB BIOS-less border-capture path and hands the post-palette LCD line
// to the SGB ICD2 char-transfer ring. window_line advances only when the
// window actually drew at least one pixel this LY (matches Pan Docs:
// Window Internal Line Counter).
void FinalizeScanline(Ppu &p)
{
	if (p.ly >= GB_SCREEN_HEIGHT) return;

	uint8_t *const line = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];

	if (::g_cam_live > 0 && p.ly >= 16 && p.ly < 16 + 112)
	{
		const int srow = p.ly - 16;
		for (int xx = 0; xx < 128; ++xx)
		{
			const int sx = 8 + xx;
			if (sx < GB_SCREEN_WIDTH)
				line[sx] = ::g_cam_shade[srow * 128 + xx];
		}
	}

	std::memcpy(&p.raw_framebuffer[p.ly * GB_SCREEN_WIDTH],
	            p.scanline_raw, GB_SCREEN_WIDTH);
	S9xSGBCaptureScanline(line);
}

// ===================================================================
// Mode-3 pixel pipeline — pandocs fetcher/FIFO model. The background
// fetcher cycles tile# / data-lo / data-hi (2 dots each) and then
// retries a push every dot until the BG FIFO drains; the LCD pops one
// pixel per dot; sprite hits pause popping, wait for the fetcher to
// latch its data-high byte, then steal it for a 6-dot object fetch.
// Registers are read live at their consuming stage, which reproduces
// the mid-scanline write artifacts the mealybug-tearoom tests verify.
// ===================================================================

inline void BgFifoClear(PixelMachine &m)
{
	m.bgf_head  = 0;
	m.bgf_count = 0;
}

inline void ObjFifoClear(PixelMachine &m)
{
	std::memset(m.objf_color, 0, sizeof m.objf_color);
	std::memset(m.objf_flags, 0, sizeof m.objf_flags);
	std::memset(m.objf_owner, 0xFF, sizeof m.objf_owner);
	std::memset(m.objf_valid, 0, sizeof m.objf_valid);
	m.objf_head = m.objf_size = m.objf_uflow = 0;
}

// Un-pop one pixel (window-activation rollback). The OBJ ring still holds
// the popped slot's data, so stepping head back restores alignment; pops
// taken while the ring was empty cancel first (Coffee GB SpriteFifo).
inline void RewindPixel(PixelMachine &m)
{
	if (m.lcd_x <= 0) return;
	if (m.objf_uflow > 0)
		--m.objf_uflow;
	else if (m.objf_size < 8)
	{
		m.objf_head = static_cast<uint8_t>((m.objf_head - 1) & 7);
		++m.objf_size;
	}
	--m.lcd_x;
}

inline uint16_t FetchDataAddr(const Ppu &p, const PixelMachine &m, int hi)
{
	uint32_t fine_y;
	if (p.cgb)
		fine_y = m.fetch_y_latch & 7;
	else if (m.fetch_is_window)
		fine_y = static_cast<uint32_t>(m.window_line) & 7;
	else
		fine_y = static_cast<uint8_t>(p.ly + p.scy) & 7;

	uint32_t bank = 0;
	if (p.cgb)
	{
		if (m.fetch_attr & 0x08) bank = 0x2000;
		if (m.fetch_attr & 0x40) fine_y = 7 - fine_y;
	}
	uint16_t addr;
	if (p.lcdc & 0x10)
		addr = static_cast<uint16_t>(m.fetch_tile * 16);
	else
		addr = static_cast<uint16_t>(0x1000 + static_cast<int8_t>(m.fetch_tile) * 16);
	return static_cast<uint16_t>(bank + addr + fine_y * 2 + hi);
}

inline bool WinEnView(const Ppu &p, const PixelMachine &m)
{
	static int wd = -1;
	if (wd < 0) { const char *e = getenv("ACID_WD"); wd = e ? atoi(e) : 1; }
	const int d = m.emits ? wd : 0;   // the skeleton reads the live register
	const uint8_t v = d == 0 ? p.lcdc
	                : d == 1 ? p.lcdc_shadow
	                : d == 2 ? p.lcdc_d2
	                : d == 3 ? p.lcdc_d3 : p.lcdc_d4;
	return (v & 0x20) != 0;
}

// WX as this machine sees it (same latch as the window-enable view).
inline uint8_t WxView(const Ppu &p, const PixelMachine &m)
{
	static int wxd = -99;
	if (wxd < -90) { const char *e = getenv("ACID_WXD"); wxd = e ? atoi(e) : 0; }
	const int d = m.emits ? wxd : 0;
	return d == 0 ? p.wx
	     : d == 1 ? p.wx_d1
	     : d == 2 ? p.wx_d2
	     : d == 3 ? p.wx_d3 : p.wx_d4;
}

// Push attempt — retried every dot until the FIFO has drained. Returns
// without pushing while pixels remain.
void BgPushAttempt(Ppu &p, PixelMachine &m)
{
	if (m.bgf_count != 0)
		return;
	// Window-disable pixel insertion (SameBoy #278): with WY armed
	// but the window bit off, a WX match at the push slot injects a
	// single blank pixel instead of the tile row (m2_win_en_toggle).
	if (!p.cgb && p.wy_triggered && !WinEnView(p, m) && !m.win_insert_disable)
	{
		uint8_t lp = static_cast<uint8_t>(m.pos + 7);
		if (lp > 167) lp = 0;
		if (WxView(p, m) == lp)
		{
			m.bgf_color[m.bgf_head] = 0;
			m.bgf_attr[m.bgf_head]  = 0;
			m.bgf_layer[m.bgf_head] = 0;
			m.bgf_count = 1;
			return;
		}
	}
	const bool flip = p.cgb && (m.fetch_attr & 0x20);
	for (int i = 0; i < 8; ++i)
	{
		const int bit = flip ? i : (7 - i);
		const uint8_t c = static_cast<uint8_t>(
			(((m.fetch_hi >> bit) & 1) << 1) | ((m.fetch_lo >> bit) & 1));
		const int slot = (m.bgf_head + m.bgf_count) & 7;
		m.bgf_color[slot] = c;
		m.bgf_attr[slot]  = m.fetch_attr;
		m.bgf_layer[slot] = m.fetch_is_window ? 1 : 0;
		++m.bgf_count;
	}
	m.fetch_stage = 0;
	m.fetch_dot   = 0;
}

// One dot of the background/window fetcher. Each 2-dot stage latches its
// address (register sampling instant) on the first dot and performs the
// VRAM read on the second — SameBoy's T1/T2 split, which is what decides
// exactly which mid-scanline register write lands in which tile. The
// data-high read falls through to a same-dot push attempt (SameBoy H2).
void FetcherDot(Ppu &p, PixelMachine &m)
{
	if (m.fetch_pause > 0)
	{
		--m.fetch_pause;
		return;
	}
	switch (m.fetch_stage)
	{
	case 0:  // tile number
		if (m.fetch_dot == 0)
		{
			// CGB: the window dies the instant LCDC.5 goes low (SameBoy T1).
			// DMG runs the early-fetch abort rule in the render loop instead.
			if (p.cgb && !(p.lcdc & 0x20))
				m.fetch_is_window = false;
			uint16_t map_base;
			uint32_t row, col;
			if (m.fetch_is_window)
			{
				map_base = (p.lcdc & 0x40) ? 0x1C00 : 0x1800;
				// The hardware window-line counter is 8 bits — double
				// activations per line can wrap it within a frame.
				row = (static_cast<uint32_t>(m.window_line) & 0xFF) >> 3;
				col = m.fetch_tile_x & 31;
			}
			else
			{
				map_base = (p.lcdc & 0x08) ? 0x1C00 : 0x1800;
				row = static_cast<uint8_t>(p.ly + p.scy) >> 3;
				if (static_cast<uint8_t>(m.pos + 16) < 8)
					col = p.scx >> 3;
				else
				{
					// Hardware derives the column from live SCX plus the
					// PPU position, so mid-line SCX writes (even the fine
					// bits) re-steer the fetch (mealybug m3_scx_*).
					const uint32_t adj = (p.cgb && !m.during_obj) ? 1 : 0;
					col = ((static_cast<uint32_t>(p.scx) + m.pos + 8 - adj) >> 3) & 31;
				}
			}
			m.fetch_map_addr = static_cast<uint16_t>(map_base + row * 32 + col);
			m.fetch_y_latch  = m.fetch_is_window
				? static_cast<uint8_t>(m.window_line)
				: static_cast<uint8_t>(p.ly + p.scy);
			m.fetch_dot = 1;
			return;
		}
		m.fetch_dot = 0;
		m.fetch_tile = p.vram[m.fetch_map_addr];
		m.fetch_attr = p.cgb ? p.vram[0x2000 + m.fetch_map_addr] : 0;
		m.fetch_stage = 1;
		return;
	case 1:  // data low
		if (m.fetch_dot == 0)
		{
			m.fetch_data_addr = FetchDataAddr(p, m, 0);
			m.fetch_dot = 1;
			return;
		}
		m.fetch_dot = 0;
		m.fetch_lo = p.vram[m.fetch_data_addr];
		m.fetch_stage = 2;
		return;
	case 2:  // data high
		if (m.fetch_dot == 0)
		{
			m.fetch_data_addr = FetchDataAddr(p, m, 1);
			m.fetch_dot = 1;
			return;
		}
		m.fetch_dot = 0;
		m.fetch_hi = p.vram[m.fetch_data_addr];
		if (m.fetch_is_window)
			m.fetch_tile_x = static_cast<uint8_t>((m.fetch_tile_x + 1) & 31);
		m.fetch_stage = 3;
		BgPushAttempt(p, m);
		return;
	default: // parked at push
		BgPushAttempt(p, m);
		return;
	}
}

// Merge a fetched sprite row into the OBJ FIFO at slots 0-7 (slot 0 pops
// next). SameBoy fifo_overlay_object_row: the row is first padded with
// blanks, transparent pixels never write, an opaque pixel replaces a
// transparent slot, and on CGB also a slot owned by a higher OAM index —
// DMG first-opaque-wins and X/OAM priority fall out of the fetch order.
void ObjOverlayRow(Ppu &p, PixelMachine &m, uint8_t lo, uint8_t hi, uint8_t flags, uint8_t oi)
{
	m.objf_uflow = 0;
	while (m.objf_size < 8)
	{
		const int slot = (m.objf_head + m.objf_size) & 7;
		m.objf_color[slot] = 0;
		m.objf_flags[slot] = 0;
		m.objf_owner[slot] = 0xFF;
		m.objf_valid[slot] = 1;
		++m.objf_size;
	}
	for (int j = 0; j < 8; ++j)
	{
		const int bit  = (flags & 0x20) ? j : (7 - j);
		const int slot = (m.objf_head + j) & 7;
		const uint8_t c = static_cast<uint8_t>(
			(((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
		if (c == 0) continue;
		if (m.objf_color[slot] != 0)
		{
			if (!p.cgb) continue;
			if (m.objf_owner[slot] <= oi) continue;
		}
		m.objf_color[slot] = c;
		m.objf_flags[slot] = flags;
		m.objf_owner[slot] = oi;
	}
}

// VRAM address of the fetched object's current row — OBJ size and flip are
// sampled live at each data read (SameBoy get_object_line_address).
uint16_t ObjLineAddr(const Ppu &p, const PixelMachine &m, uint8_t raw_y)
{
	const bool h16 = (p.lcdc & 0x04) != 0;
	uint8_t ty = static_cast<uint8_t>((p.ly - raw_y) & (h16 ? 15 : 7));
	if (m.obj_flags_bus & 0x40) ty ^= h16 ? 15 : 7;
	uint16_t la = static_cast<uint16_t>(
		(h16 ? (m.obj_tile_bus & 0xFE) : m.obj_tile_bus) * 16 + ty * 2);
	if (p.cgb && (m.obj_flags_bus & 0x08)) la = static_cast<uint16_t>(la + 0x2000);
	return la;
}

// no_sprite_limit extras merge in one shot so mode-3 timing ignores them.
void ObjFetchInstant(Ppu &p, PixelMachine &m, int s)
{
	const Ppu::SpriteHit &h = p.sprites[s];
	const uint8_t saved_t = m.obj_tile_bus, saved_f = m.obj_flags_bus;
	m.obj_tile_bus  = p.oam[h.oam_idx * 4 + 2];
	m.obj_flags_bus = p.oam[h.oam_idx * 4 + 3];
	const uint16_t la = ObjLineAddr(p, m, h.y);
	ObjOverlayRow(p, m, p.vram[la], p.vram[la + 1], m.obj_flags_bus, h.oam_idx);
	m.obj_tile_bus = saved_t;
	m.obj_flags_bus = saved_f;
}

// SameBoy x_for_object_match: raw OAM X the position counter is passing.
inline uint8_t XForObjMatch(const PixelMachine &m)
{
	const uint8_t ret = static_cast<uint8_t>(m.pos + 8);
	return (ret > 0xF0) ? 0 : ret;
}

// Retire sprites the position counter has passed — this runs even with
// OBJ disabled on DMG (m3_lcdc_obj_en_change: re-enabling never fetches
// stale sprites).
void SpriteDiscardPassed(const Ppu &p, PixelMachine &m, uint8_t x_match)
{
	for (uint8_t i = 0; i < p.sprite_count; ++i)
	{
		if (m.sprite_used_mask & (1ull << i)) continue;
		const uint8_t raw = static_cast<uint8_t>(p.sprites[i].x + 8);
		if (raw < x_match) m.sprite_used_mask |= 1ull << i;
	}
}

// First unfetched sprite at exactly x_match, in OAM-scan order.
int SpriteMatchAt(const Ppu &p, const PixelMachine &m, uint8_t x_match)
{
	for (uint8_t i = 0; i < p.sprite_count; ++i)
	{
		if (m.sprite_used_mask & (1ull << i)) continue;
		if (static_cast<uint8_t>(p.sprites[i].x + 8) == x_match) return i;
	}
	return -1;
}

// Rendering (and scroll adjustment) stalls while an OAM-X=0 object is
// still pending (SameBoy render_pixel_if_possible head).
bool SpritePendingAtRaw0(const Ppu &p, const PixelMachine &m)
{
	for (uint8_t i = 0; i < p.sprite_count; ++i)
	{
		if (m.sprite_used_mask & (1ull << i)) continue;
		if (static_cast<uint8_t>(p.sprites[i].x + 8) == 0) return true;
	}
	return false;
}

// One dot of the object fetch. The wait state advances the BG fetcher
// until it holds its data-high with pixels queued; the fetch proper is 6
// dots (SameBoy sleeps 1+2+2+1 plus the two fetcher advances) with the
// OAM/VRAM reads landing on their hardware dots. The row merges at the
// start of the following dot (obj_overlay).
void ObjMachineDot(Ppu &p, PixelMachine &m, Memory &mem)
{
	const Ppu::SpriteHit &h = p.sprites[m.obj_fetch_slot];
	switch (m.obj_fetch_state)
	{
	case 200:
		// The object fetch waits for the BG fetcher to reach its data-high
		// read with pixels queued (SameBoy), then steals it.
		if (((m.fetch_stage == 2 && m.fetch_dot == 1) || m.fetch_stage == 3) &&
		    m.bgf_count > 0)
		{
			FetcherDot(p, m);
			m.obj_fetch_state = 5;
			return;
		}
		FetcherDot(p, m);
		return;
	case 5:
		FetcherDot(p, m);
		if (mem.dma_active && !p.cgb && mem.dma_index > 0 && mem.dma_index < 0xA0)
		{
			// DMA holds the OAM bus during the fetch's OAM read too.
			m.obj_tile_bus  = p.oam[mem.dma_index & ~1];
			m.obj_flags_bus = p.oam[(mem.dma_index & ~1) | 1];
		}
		else
		{
			m.obj_tile_bus  = p.oam[h.oam_idx * 4 + 2];
			m.obj_flags_bus = p.oam[h.oam_idx * 4 + 3];
		}
		p.scan_y_bus = m.obj_tile_bus;
		m.obj_fetch_state = 4;
		return;
	case 4: m.obj_fetch_state = 3; return;
	case 3:
		m.obj_lo = p.vram[ObjLineAddr(p, m, h.y)];
		m.obj_fetch_state = 2;
		return;
	case 2: m.obj_fetch_state = 1; return;
	default:
		m.obj_hi = p.vram[static_cast<uint16_t>(ObjLineAddr(p, m, h.y) + 1)];
		m.during_obj = false;
		m.obj_fetch_state = 0;
		m.obj_overlay = true;
		return;
	}
}

// Handle every unfetched sprite matching the current column. Returns true
// when a real fetch started and consumed this dot.
bool ObjHandleMatches(Ppu &p, PixelMachine &m, Memory &mem)
{
	if (!((p.lcdc & 0x02) || p.cgb)) return false;
	int s;
	while ((s = SpriteMatchAt(p, m, XForObjMatch(m))) >= 0)
	{
		m.sprite_used_mask |= 1ull << s;
		if (s >= GB_OAM_SCAN_LIMIT)
		{
			ObjFetchInstant(p, m, s);
			continue;
		}
		m.obj_fetch_slot = static_cast<uint8_t>(s);
		m.during_obj = true;
		m.obj_fetch_state = 200;
		ObjMachineDot(p, m, mem);
		return true;
	}
	return false;
}

// Emit the popped BG pixel (+ the popped OBJ FIFO pixel) to the framebuffer.
void EmitPixel(Ppu &p, PixelMachine &m, uint8_t bg_color, uint8_t bg_attr, bool is_window,
               uint8_t obj_c, uint8_t obj_flags)
{
	if (!m.emits) return;   // the timing skeleton produces no pixels
	const int x = m.lcd_x;
	if (x < 0 || x >= GB_SCREEN_WIDTH || p.ly >= GB_SCREEN_HEIGHT) return;

	uint8_t  *const line  = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];
	uint8_t  *const lay   = &p.layer[p.ly * GB_SCREEN_WIDTH];
	uint16_t *const cline = &p.color_fb[p.ly * GB_SCREEN_WIDTH];

	const bool bg_hidden = is_window ? !p.show_window : !p.show_bg;

	if (!p.cgb)
	{
		// BGP is sampled live at emission; LCDC.0/.1 one dot behind — except
		// the line's FIRST pixel, whose enable mux resolves one dot later
		// (Coffee GB DmgPixelFifo first-pixel split; m3_lcdc_*_change at x=0).
		const uint8_t mux = (x == 0) ? p.lcdc : p.lcdc_shadow;
		const uint8_t bgc = (mux & 0x01) ? bg_color : 0;
		const uint8_t disp_bg = bg_hidden ? 0 : bgc;

		p.scanline_bg_raw[x] = bgc;
		p.scanline_raw[x]    = bgc;
		if (!p.present_hold)
		{
			line[x] = ApplyPalette(p.bgp, disp_bg);
			lay[x]  = is_window ? GB_PIXEL_WINDOW : GB_PIXEL_BG;
		}
		if (p.show_obj && (mux & 0x02) && obj_c != 0 &&
		    (!(obj_flags & 0x80) || disp_bg == 0))
		{
			p.scanline_raw[x] = obj_c;
			if (!p.present_hold)
			{
				line[x] = ApplyPalette((obj_flags & 0x10) ? p.obp1 : p.obp0, obj_c);
				lay[x]  = GB_PIXEL_OBJ;
			}
		}
	}
	else
	{
		p.scanline_bg_raw[x] = bg_color;
		p.scanline_raw[x]    = bg_color;
		if (!p.present_hold)
		{
			line[x]  = bg_color;
			lay[x]   = is_window ? GB_PIXEL_WINDOW : GB_PIXEL_BG;
			cline[x] = bg_hidden ? CgbColor(p.bg_pal, 0, 0)
			                     : CgbColor(p.bg_pal, bg_attr & 0x07, bg_color);
		}
		if (p.show_obj && (p.lcdc & 0x02) && obj_c != 0)
		{
			const bool bg_master = (p.lcdc & 0x01) != 0;
			const bool bg_wins   = !bg_hidden && bg_master && bg_color != 0 &&
			                       ((bg_attr & 0x80) || (obj_flags & 0x80));
			if (!bg_wins)
			{
				p.scanline_raw[x] = obj_c;
				if (!p.present_hold)
				{
					line[x]  = obj_c;
					lay[x]   = GB_PIXEL_OBJ;
					cline[x] = CgbColor(p.obj_pal, obj_flags & 0x07, obj_c);
				}
			}
		}
	}
}

// LCDC.5 as the window comparator/disable machinery sees it — a few dots
// behind the CPU write (the LCD output-latch crossing; env ACID_WD).
// Window activation comparator (SameBoy render loop head). Runs once per
// loop iteration, before object handling. Returns true when the WX=0 +
// fine-scroll case burns the dot (the iteration resumes next dot).
bool WindowCheckDot(Ppu &p, PixelMachine &m)
{
	const uint8_t wxv = WxView(p, m);
	if (m.fetch_is_window || !p.wy_triggered || !WinEnView(p, m))
		return false;
	// DMG LCD/PPU desync catch-up: a WX match was masked by an LCDC.5-off
	// pulse; re-enabling within 3 dots activates as if at the match dot,
	// rolling the emitted pixels back (Coffee GB windowCatchUpPos).
	if (!p.cgb && m.win_catchup_pos >= 0 && !m.win_activated_line)
	{
		static int dmg = -1;
		if (dmg < 0) { const char *e = getenv("ACID_DMG"); dmg = e ? atoi(e) : 0; }
		const int gap = static_cast<int>(m.pos) - m.win_catchup_pos;
		if (gap >= 0 && gap <= dmg)
		{
			int target = m.win_catchup_pos;
			if (gap == dmg)
				++target;   // dmg_blob retains one more LCD pixel at max gap
			while (static_cast<int>(m.pos) > target)
			{
				RewindPixel(m);
				--m.pos;
			}
			m.win_catchup_pos = -1;
			++m.window_line;
			m.fetch_is_window = true;
			m.win_fresh       = true;
			m.fetch_tile_x    = 0;
			BgFifoClear(m);
			m.fetch_stage = 0;
			m.fetch_dot   = 0;
			p.window_active  = true;
			p.window_start_x = m.lcd_x;
			m.win_activated_line = true;
			FetcherDot(p, m);   // the fetch restarted back at the match dot
			return false;
		}
	}
	bool act = false;
	if (wxv == 0)
	{
		if (m.pos == 0xF9) act = true;                       // -7
		else if (m.pos == 0xF0 && (p.scx & 7)) act = true;   // -16 + fine scroll
		else if (m.pos >= 0xF1 && m.pos <= 0xF8) act = true; // -15..-8
	}
	else if (wxv < 166u + (p.cgb ? 1u : 0u))
	{
		if (wxv == static_cast<uint8_t>(m.pos + 7))
			act = true;
		else if (!p.cgb && wxv == static_cast<uint8_t>(m.pos + 6) &&
		         p.wx_write_cooldown == 0)
		{
			// DMG LCD/PPU desync: the comparator fires a dot early and
			// rolls the panel back one pixel (strikethrough).
			act = true;
			if (m.lcd_x > 0) --m.lcd_x;
		}
	}
	if (act)
	{
		++m.window_line;
		m.fetch_tile_x = 0;
		BgFifoClear(m);
		m.fetch_is_window = true;
		m.fetch_stage = 0;
		m.fetch_dot   = 0;
		m.win_fresh   = true;
		p.window_active  = true;
		p.window_start_x = m.lcd_x;
		if (wxv == 0 && (p.scx & 7) && !p.cgb)
		{
			m.iter_resume = true;   // the activation itself costs a dot
			return true;
		}
	}
	else if (!p.cgb && wxv == 166 && wxv == static_cast<uint8_t>(m.pos + 7))
	{
		// WX=166 matches but never engages — the line counter still ticks
		// (dmg-acid2 WIL test).
		++m.window_line;
	}
	return false;
}

// SameBoy render_pixel_if_possible: pop one pixel, run the position
// machine's scroll-discard states, emit when on-screen.
void RenderDot(Ppu &p, PixelMachine &m, Memory &mem)
{
	if (((p.lcdc & 0x02) || p.cgb) && SpritePendingAtRaw0(p, m))
		return;
	if (m.bgf_count == 0)
		return;

	uint8_t c, at;
	bool win;
	if (m.bgf_insert)
	{
		m.bgf_insert = false;
		c = 0; at = 0; win = m.fetch_is_window;
	}
	else
	{
		c   = m.bgf_color[m.bgf_head];
		at  = m.bgf_attr[m.bgf_head];
		win = m.bgf_layer[m.bgf_head] != 0;
		m.bgf_head = static_cast<uint8_t>((m.bgf_head + 1) & 7);
		--m.bgf_count;
	}
	// The OBJ FIFO pops in step with every BG pop — dropped pixels consume
	// sprite pixels too (left-edge clipping falls out of this).
	uint8_t obj_c = 0, obj_fl = 0;
	if (m.objf_size > 0)
	{
		obj_c  = m.objf_color[m.objf_head];
		obj_fl = m.objf_flags[m.objf_head];
		m.objf_head = static_cast<uint8_t>((m.objf_head + 1) & 7);
		--m.objf_size;
	}
	else
	{
		++m.objf_uflow;
	}

	if (static_cast<uint8_t>(m.pos + 16) < 8)
	{
		if ((m.pos & 7) == (p.scx & 7))
			m.pos = 0xF8;
		else if (m.win_fresh && (m.pos & 7) == 6 && (p.scx & 7) == 7)
			m.pos = 0xF8;
		else if (m.pos == 0xF7)
		{
			m.pos = 0xF0;
			return;
		}
	}
	m.win_fresh = false;

	if (m.pos >= 160)
	{
		++m.pos;
		return;
	}

	EmitPixel(p, m, c, at, win, obj_c, obj_fl);
	++m.pos;
	++m.lcd_x;
	p.draw_x = m.lcd_x;
	// The mode-0 STAT source pulses a few dots before the visible flip
	// (SameBoy fires it at the last pixel; ours lands early to match the
	// CPU-observed grid).
	static int m0pre = -1;
	if (m0pre < 0) { const char *e = getenv("ACID_M0PRE"); m0pre = e ? atoi(e) : 1; }
	if (!m.emits && m.lcd_x == GB_SCREEN_WIDTH - m0pre &&
	    (p.lcdc & 0x80) && (p.stat & 0x08) && !p.stat_line_high)
	{
		mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
		p.stat_line_high = true;
	}
}

} // anonymous

namespace { int g_pal_unit = 0; }

// True while the LCD is still receiving pixels for this line. The output
// machine trails the timing skeleton, so a write landing just after the
// skeleton's mode-3 ends is still a mid-line write as far as the pixels
// are concerned.
static bool PpuRendering(const Ppu &p)
{
	return p.mode == PpuMode::Transfer || !p.om.done;
}

void PpuSetPalUnit(int unit) { g_pal_unit = unit; }

void PpuReset(Ppu &p)
{
	std::memset(p.vram,        0, sizeof p.vram);
	std::memset(p.oam,         0, sizeof p.oam);
	std::memset(p.framebuffer,     0, sizeof p.framebuffer);
	std::memset(p.raw_framebuffer, 0, sizeof p.raw_framebuffer);
	std::memset(p.layer,           0, sizeof p.layer);
	std::memset(p.scanline_bg_raw, 0, sizeof p.scanline_bg_raw);
	std::memset(p.scanline_raw,    0, sizeof p.scanline_raw);
	p.lcdc = 0x91;   // LCD on, BG on, BG tile data at 0x8000, BG map at 0x9800.
	p.stat = 0x85;
	p.scy = p.scx = p.ly = p.lyc = 0;
	p.bgp = 0xFC; p.obp0 = 0xFF; p.obp1 = 0xFF;
	p.wy = p.wx = 0;
	p.mode = PpuMode::OamScan;
	p.mode_clock    = 0;
	p.tm = PixelMachine{};
	p.om = PixelMachine{};
	p.om.emits = true;
	p.wx_write_cooldown = 0;
	p.lcdon_first   = false;
	p.lcdon_pad     = 0;
	p.lcdon_line    = false;
	p.stop_display  = 0;
	p.mode3_hold    = -1;
	p.pal_glitch    = 0;
	p.ly_lag        = 0;
	p.scan_y_bus = p.scan_x_bus = 0;
	p.stat_line_high = false;
	p.vblank_irq_at = 0;
	p.stat_irq_delay = 0;
	p.present_hold  = false;
	p.frame_ready   = false;
	p.t_cycles      = 0;
	p.draw_x        = 0;
	p.sprite_count  = 0;
	p.window_active = false;
	p.window_start_x = 0;
	p.wy_triggered  = false;
	p.mode3_sprite_stall = 0;
	p.latched_wx    = 0;
	p.latched_bgp   = p.bgp;
	std::memset(p.color_fb, 0, sizeof p.color_fb);
	p.vbk = 0;
	p.bcps = p.ocps = 0;
	std::memset(p.bg_pal,  0xFF, sizeof p.bg_pal);
	std::memset(p.obj_pal, 0xFF, sizeof p.obj_pal);
}

// Mode-3 exit: strikethrough fill, WX=166 carry, stall math, HBlank.
// The output machine finished its line: fill any pixels the LCD never
// received (a WX desync leaves lcd_x short) and publish the scanline.
static void Mode3OutputExit(Ppu &p, PixelMachine &m)
{
	if (m.lcd_x < GB_SCREEN_WIDTH && p.ly < GB_SCREEN_HEIGHT)
	{
		uint8_t  *const line  = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];
		uint8_t  *const lay   = &p.layer[p.ly * GB_SCREEN_WIDTH];
		uint16_t *const cline = &p.color_fb[p.ly * GB_SCREEN_WIDTH];
		for (int x = m.lcd_x; x < GB_SCREEN_WIDTH; ++x)
		{
			p.scanline_raw[x]    = x ? p.scanline_raw[x - 1] : 0;
			p.scanline_bg_raw[x] = x ? p.scanline_bg_raw[x - 1] : 0;
			if (!p.present_hold)
			{
				line[x]  = x ? line[x - 1]
				             : (p.cgb ? 0 : ApplyPalette(p.bgp, 0));
				lay[x]   = x ? lay[x - 1] : GB_PIXEL_BG;
				cline[x] = x ? cline[x - 1] : CgbColor(p.bg_pal, 0, 0);
			}
		}
		m.lcd_x = GB_SCREEN_WIDTH;
	}
	FinalizeScanline(p);
}

// WX=166 at line end: the window pre-arms for the next line with
// tile_x=1 and the line counter ticks once more (SameBoy). Per machine.
static void Mode3WxCarry(Ppu &p, PixelMachine &m)
{
	if (!p.cgb && p.wy_triggered && (p.lcdc & 0x20) && p.wx == 166)
	{
		m.win_carry = true;
		++m.window_line;
	}
}

// The timing skeleton finished: mode 3 ends here, which is what STAT, LY,
// the interrupts and the CPU's memory locks follow.
static bool Mode3Exit(Ppu &p, PixelMachine &m, Memory &mem)
{
	Mode3WxCarry(p, m);
	// Mode-3 length is emergent; expose the overrun as the legacy
	// "sprite stall" so HBlank shrinks to keep the 456-dot line.
	p.mode3_sprite_stall = static_cast<int16_t>(p.mode_clock - MODE3_DOTS);
	p.mode_clock  = 0;
	p.mode        = PpuMode::HBlank;
	MemHdmaHBlank(mem);
	return true;
}

// One dot of one mode-3 machine. Returns true when this machine has
// finished the line (pos reached 160).
static bool Mode3Dot(Ppu &p, PixelMachine &m, Memory &mem)
{

	// --- SameBoy position_in_line render loop, one dot per call ---
	if (m.entry_wait > 0)
	{
		--m.entry_wait;
		return false;
	}
	if (m.machine_stall > 0)
	{
		--m.machine_stall;
		return false;
	}
	// ACID_XS=1: the mode-0 transition lands one dot after the last
	// pop (SameBoy sleeps once between the loop break and the STAT
	// flip); the extra dot renders nothing.
	{
		static int xs = -1;
		if (xs < 0) { const char *e = getenv("ACID_XS"); xs = e ? atoi(e) : 0; }
		if (xs > 0 && m.pos == 160 && m.entry_wait == 0)
		{
			if (p.mode3_hold < 0)
			{
				p.mode3_hold = static_cast<int16_t>(xs);
				return false;
			}
			if (p.mode3_hold > 0 && --p.mode3_hold > 0)
				return false;
			p.mode3_hold = -1;
			return true;
		}
	}

	// An in-flight object fetch owns the dot outright (pops paused).
	if (m.obj_fetch_state > 0)
	{
		ObjMachineDot(p, m, mem);
		return false;
	}

	bool to_render = false;
	if (m.obj_overlay)
	{
		// The fetched row lands now; a chained object at the same column
		// starts immediately, otherwise the iteration falls through to
		// the pop (the loop head does NOT re-run mid-iteration).
		m.obj_overlay = false;
		ObjOverlayRow(p, m, m.obj_lo, m.obj_hi, m.obj_flags_bus,
		              p.sprites[m.obj_fetch_slot].oam_idx);
		if (ObjHandleMatches(p, m, mem))
			return false;
		to_render = true;
	}

	if (!to_render)
	{
		const bool skip_head = m.iter_resume;
		m.iter_resume = false;
		// Commit or drop a pending window activation: the match dot's
		// register state must survive one dot or the fetch never starts
		// (Coffee GB windowPendingTicks). The pixel popped during the
		// pending dot rolls back so the activation lands at the match.
		if (m.win_pend)
		{
			m.win_pend = 0;
			if (p.wx == m.win_pend_wx && (p.cgb || WinEnView(p, m)))
			{
				if (m.pos != m.win_pend_pos)
				{
					RewindPixel(m);
					m.pos = m.win_pend_pos;
				}
				++m.window_line;
				m.fetch_tile_x = 0;
				BgFifoClear(m);
				m.fetch_is_window = true;
				m.fetch_stage = 0;
				m.fetch_dot   = 0;
				m.win_fresh   = true;
				p.window_active  = true;
				p.window_start_x = m.lcd_x;
				if (p.wx == 0 && (p.scx & 7) && !p.cgb)
					m.machine_stall = 1;
				if (!m.win_activated_line)
					FetcherDot(p, m);   // the fetch restarted back at the match dot
				m.win_activated_line = true;
			}
		}
		// DMG: clearing LCDC.5 kills the window fetch only while it is
		// still early (through the map read); later stages complete and
		// push their window tile (Coffee GB; m3_lcdc_win_en_change_multiple).
		if (!p.cgb && m.fetch_is_window && !WinEnView(p, m) &&
		    m.obj_fetch_state == 0 && m.fetch_stage <= 1)
		{
			if (m.fetch_stage != 0 || m.fetch_dot != 0)
			{
				m.fetch_stage = 0;
				m.fetch_dot   = 0;
			}
			m.fetch_is_window = false;
			m.win_fresh       = false;
		}
		// A WX match landing while the pulse has the window off arms the
		// desync catch-up (only before the line's first activation).
		if (!p.cgb && !m.fetch_is_window && !WinEnView(p, m) &&
		    !m.win_activated_line && p.wy_triggered &&
		    p.wx >= 7 && p.wx < 166 && p.wx_write_cooldown == 0 &&
		    p.wx == static_cast<uint8_t>(m.pos + 7))
			m.win_catchup_pos = static_cast<int16_t>(m.pos);
		if (!skip_head && WindowCheckDot(p, m))
			return false;
		// WX re-match while the window is active with a fresh tile fetch
		// queued: one blank pixel squeezes into the stream (SameBoy
		// insert_bg_pixel; mealybug m3_wx_4/5/6_change).
		if (p.wx == static_cast<uint8_t>(m.pos + 7) && (!p.cgb || p.wx == 0) &&
		    m.fetch_is_window && !m.win_fresh &&
		    m.fetch_stage == 0 && m.fetch_dot == 0 && m.bgf_count == 8)
			m.bgf_insert = true;

		SpriteDiscardPassed(p, m, XForObjMatch(m));
		if (ObjHandleMatches(p, m, mem))
			return false;
	}

	RenderDot(p, m, mem);
	FetcherDot(p, m);

	if (m.pos == 160)
	{
		{
			static int xs2 = -1;
			if (xs2 < 0) { const char *e = getenv("ACID_XS"); xs2 = e ? atoi(e) : 0; }
			if (xs2 > 0)
				return false;   // exit handled at the top of the next dot
		}
		return true;
	}

	return false;
}

// One GB t-cycle's worth of PPU work. Drives mode 2 sprite eval (latched
// at the mode 2→3 boundary), mode 3 per-pixel render (one pixel per dot,
// re-sampling registers fresh — fixes mid-LY scroll/palette/LCDC tricks),
// and mode 0/1 timing.
inline void ExecPpuDot(Ppu &p, Memory &mem)
{
	if (p.pal_glitch > 0 && --p.pal_glitch == 0)
	{
		uint8_t *r = p.pal_glitch_reg == 0 ? &p.bgp
		           : p.pal_glitch_reg == 1 ? &p.obp0 : &p.obp1;
		*r = p.pal_glitch_next;
	}
	p.mode_clock += 1;
	bool transitioned = false;

	// Deferred VBlank IRQ — latch IF.VBLANK once the CPU catches up to the
	// LY=144 dot (cycle-precise), see Ppu::vblank_irq_at for rationale.
	if (p.vblank_irq_at != 0 && mem.cpu && mem.cpu->t_cycles >= p.vblank_irq_at)
	{
		mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_VBLANK);
		p.vblank_irq_at = 0;
	}

	if (p.stat_irq_delay > 0)
	{
		--p.stat_irq_delay;
		if (p.stat_irq_delay == 0)
		{
			mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
		}
	}

	switch (p.mode)
	{
	case PpuMode::OamScan:
		// The WY comparator runs continuously while the window is enabled,
		// so a brief mode-2 enable pulse still arms it (m2_win_en_toggle).
		if ((p.lcdc & 0x20) && p.ly == p.wy)
			p.wy_triggered = true;
		// STAT flips to mode 3 at line dot 84 (SameBoy grid; entered at -4).
		// Keeping the subtraction at 80 hands mode 3 a 4-dot head start on
		// mode_clock, which the stall math turns into a 4-dot-shorter HBlank.
		{
			static int lk = 99, xv = -1;
			if (lk == 99) { const char *e = getenv("ACID_LCDON"); lk = e ? atoi(e) : 1;
			                const char *f = getenv("ACID_XV"); xv = f ? atoi(f) : 7; }
			if (p.mode_clock < MODE2_DOTS + (p.lcdon_first ? lk : xv))
				break;
			if (p.lcdon_first)
			{
				static int lp = 99;
				if (lp == 99) { const char *e = getenv("ACID_LPAD"); lp = e ? atoi(e) : -4; }
				p.lcdon_pad = static_cast<int16_t>(lp);
			}
		}
		{
			const bool was_lcdon = p.lcdon_first;
			p.lcdon_line  = was_lcdon;
			p.lcdon_first = false;
			p.mode_clock -= MODE2_DOTS;
			p.mode        = PpuMode::Transfer;
			// Latch the per-LY sprite list at the mode 2→3 boundary (the
			// hardware OAM scan's outcome) in scan order — the FIFO's
			// fetch order then produces X/OAM priority naturally.
			EvalSprites(p, mem);

			// Prime both dot machines. Each starts with 8 junk pixels in
			// its BG FIFO (the position machine's prefix drops them). The
			// skeleton runs from the STAT flip; the output machine trails
			// it so its register and VRAM reads land on the hardware dots.
			static int entry = -1, entryc = -1, entryl = -1;
			if (entry < 0) { const char *e = getenv("ACID_ENTRY"); entry = e ? atoi(e) : 4;
			                 const char *f = getenv("ACID_ENTRYC"); entryc = f ? atoi(f) : 4;
			                 const char *g = getenv("ACID_ENTRYL"); entryl = g ? atoi(g) : entry; }
			const int odelay = was_lcdon ? entryl : p.cgb ? entryc : entry;
			for (int mi = 0; mi < 2; ++mi)
			{
				PixelMachine &pm = mi ? p.om : p.tm;
				const bool carry = pm.win_carry;
				const int32_t wline = pm.window_line;
				pm = PixelMachine{};
				pm.emits       = (mi != 0);
				pm.entry_delay = static_cast<uint8_t>(mi ? odelay : 0);
				pm.entry_wait  = pm.entry_delay;
				pm.window_line = wline;
				pm.bgf_count   = 8;
				pm.fetch_is_window = carry;
				pm.fetch_tile_x    = carry ? 1 : 0;
				pm.win_activated_line = carry;
				std::memset(pm.objf_owner, 0xFF, sizeof pm.objf_owner);
			}
			p.draw_x        = 0;
			p.window_active = p.om.fetch_is_window;
			p.mode3_sprite_stall  = 0;

			// Legacy latches — kept for savestate layout and the SGB
			// capture consumers; the fetcher itself reads live registers.
			p.latched_wx  = p.wx;
			p.latched_bgp = p.bgp;
			p.fetch_scy   = p.scy;
			// Arm the window WY-trigger once LY == WY while it's enabled.
			if ((p.lcdc & 0x20) && p.ly == p.wy)
				p.wy_triggered = true;
			transitioned = true;
		}
		break;

	case PpuMode::Transfer:
	{
// Both machines advance on every mode-3 dot. The skeleton decides when
// mode 3 ends; the output machine trails it by entry_delay dots and keeps
// running into HBlank until it has produced all 160 pixels.
		if (!p.tm.done && Mode3Dot(p, p.tm, mem))
		{
			p.tm.done = true;
			transitioned = Mode3Exit(p, p.tm, mem);
		}
		if (!p.om.done && Mode3Dot(p, p.om, mem))
		{
			p.om.done = true;
			Mode3WxCarry(p, p.om);
			Mode3OutputExit(p, p.om);
		}
		p.draw_x        = p.om.lcd_x;
		p.window_active = p.om.fetch_is_window || p.om.win_carry;
		break;
	}

	case PpuMode::HBlank:
	{
		// The output machine trails the skeleton, so it finishes its line a
		// few dots into HBlank.
		if (!p.om.done && Mode3Dot(p, p.om, mem))
		{
			p.om.done = true;
			Mode3WxCarry(p, p.om);
			Mode3OutputExit(p, p.om);
			p.draw_x = p.om.lcd_x;
		}

		// Mode 0 absorbs the sprite stall so the scanline still totals 456
		// dots — including the 4-dot line-start window (below) during which
		// STAT still reads mode 0 while LY has already advanced.
		const int32_t mode0_length = MODE0_DOTS - 4 - p.mode3_sprite_stall +
		                             p.lcdon_pad;
		if (p.mode_clock >= mode0_length)
		{
			p.mode_clock -= mode0_length;
			p.mode_clock -= 4;
			p.lcdon_pad = 0;
			p.lcdon_line  = false;
			p.ly_prev     = p.ly;
			p.ly_change_t = p.t_cycles;
			++p.ly;
			S9xSGBOnPpuHBlank();
			if (p.ly == VISIBLE_LINES)
			{
				if (::g_cam_live > 0)
				{
					--::g_cam_live;
					if (mem.joypad)
					{
						const Joypad &jp = *mem.joypad;
						const uint8_t dn = jp.sgb_active ? jp.sgb_pads[0] : jp.dpad;
						if ((dn & 0x04) == 0 && ::g_cam_brightness <  96) ::g_cam_brightness += 3;
						if ((dn & 0x08) == 0 && ::g_cam_brightness > -96) ::g_cam_brightness -= 3;
					}
				}
				p.mode          = PpuMode::VBlank;
				// The OAM STAT source also pulses at vblank start, on the
				// same cycle as the vblank IRQ (mooneye vblank_stat_intr).
				if ((p.stat & 0x20) && !p.stat_line_high && p.stat_irq_delay == 0)
				{
					static int vp = -1;
					if (vp < 0) { const char *e = getenv("ACID_VP"); vp = e ? atoi(e) : 7; }
					p.stat_irq_delay = static_cast<uint8_t>(vp);
				}
				p.frame_ready   = true;
				p.present_hold  = false;
				p.tm.window_line = -1;
				p.om.window_line = -1;
				p.window_active = false;
				p.wy_triggered  = false;
				p.vblank_irq_at = p.t_cycles + GB_VBLANK_IRQ_OFFSET;
				S9xSGBOnPpuVBlank();
			}
			else
			{
				p.mode = PpuMode::OamScan;
			}
			RelatchLyc(p, mem);
			transitioned = true;
		}
		break;
	}

	case PpuMode::VBlank:
		// LY=153 hardware quirk (Pan Docs §STAT.lyc-glitch):
		// On real DMG, LY only reads as 153 for the first 4 dots of
		// scanline 153, then visibly becomes 0 for the remaining 452
		// dots while the PPU stays in mode 1 (VBlank). That early LY=0
		// is the moment LYC=0 STAT IRQ rises on real hardware — not at
		// scanline 0 as a naive scanline-end counter would imply.
		// Models the LY transition; the actual end-of-scanline-153 →
		// scanline-0 mode transition happens 452 dots later.
		if (p.ly == 153 && p.mode_clock == 4)
		{
			p.ly = 0;
			RelatchLyc(p, mem);
			transitioned = true;
		}
		if (p.mode_clock >= LINE_DOTS)
		{
			p.mode_clock -= LINE_DOTS;
			if (p.ly == 0)
			{
				// Quirk already set LY=0; the scanline-153 → scanline-0
				// boundary just transitions mode to OamScan with LY
				// unchanged.
				p.mode = PpuMode::OamScan;
			}
			else
			{
				++p.ly;
				if (p.ly >= TOTAL_LINES)
				{
					// Safety fallback (shouldn't fire if the quirk did).
					p.ly   = 0;
					p.mode = PpuMode::OamScan;
				}
				RelatchLyc(p, mem);
			}
			transitioned = true;
		}
		break;
	}

	if (transitioned)
		RecomputeStatLine(p, mem, true);

	// WX-write suppression pulse (SameBoy wx_just_changed): a write between
	// dots must still read as "just changed" during the NEXT dot's window
	// comparator, so the decrement happens at end-of-dot, not entry.
	if (p.wx_write_cooldown > 0) --p.wx_write_cooldown;
	p.lcdc_d4 = p.lcdc_d3;
	p.lcdc_d3 = p.lcdc_d2;
	p.lcdc_d2 = p.lcdc_shadow;
	p.lcdc_shadow = p.lcdc;
	p.wx_d4 = p.wx_d3;
	p.wx_d3 = p.wx_d2;
	p.wx_d2 = p.wx_d1;
	p.wx_d1 = p.wx;
}

void PpuOnCpuStop(Ppu &p, bool cgb_hw)
{
	if (!cgb_hw)
	{
		// DMG: the oscillator halts and the panel drains to white.
		std::memset(p.framebuffer, 0, sizeof p.framebuffer);
		std::memset(p.layer, GB_PIXEL_BG, sizeof p.layer);
		p.stop_display = 1;
	}
	else if (p.mode == PpuMode::Transfer && (p.lcdc & 0x80))
	{
		// CGB stopped during mode 3: VRAM stays accessible, the panel
		// keeps showing the current picture.
		p.stop_display = 3;
	}
	else
	{
		// CGB: PPU keeps scanning but reads all-black.
		std::memset(p.framebuffer, 3, sizeof p.framebuffer);
		std::memset(p.color_fb, 0, sizeof p.color_fb);
		std::memset(p.layer, GB_PIXEL_BG, sizeof p.layer);
		p.stop_display = 2;
	}
}

void PpuOnCpuStopEnd(Ppu &p)
{
	p.stop_display = 0;
}

void PpuStep(Ppu &p, Memory &mem, int32_t tcycles)
{
	if (tcycles <= 0) return;
	p.t_cycles += tcycles;

	// STOP display override — keep the clock advancing (mode machinery
	// stays live for the host frame loop) but freeze pixel output.
	if (p.stop_display != 0)
	{
		while (tcycles-- > 0)
		{
			const int16_t save_x = p.draw_x;
			ExecPpuDot(p, mem);
			// Undo any pixel the dot emitted — the panel shows the
			// stop pattern, not fresh renders.
			if (p.draw_x != save_x && p.stop_display != 3 &&
			    p.ly < GB_SCREEN_HEIGHT)
			{
				const int x = p.draw_x - 1;
				if (x >= 0 && x < GB_SCREEN_WIDTH)
				{
					p.framebuffer[p.ly * GB_SCREEN_WIDTH + x] =
						(p.stop_display == 1) ? 0 : 3;
					p.color_fb[p.ly * GB_SCREEN_WIDTH + x] =
						(p.stop_display == 1) ? 0x7FFF : 0x0000;
				}
			}
			else if (p.draw_x != save_x && p.stop_display == 3)
			{
				// hold: ExecPpuDot already wrote a fresh pixel; that
				// matches "keeps displaying the same data" since VRAM
				// is untouched while the CPU is stopped.
			}
		}
		return;
	}

	if (::g_cam_countdown > 0) { ::g_cam_countdown -= tcycles; if (::g_cam_countdown < 0) ::g_cam_countdown = 0; }

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
		p.tm.window_line = -1;
		p.om.window_line = -1;
		p.stat_line_high = (p.stat & 0x40) && (p.stat & 0x04);
		p.vblank_irq_at = 0;
		p.stat_irq_delay = 0;
		p.draw_x         = 0;
		p.window_active  = false;
		p.wy_triggered   = false;
		p.mode3_sprite_stall = 0;
		p.latched_wx     = p.wx;
		p.latched_bgp    = p.bgp;
		// Clear the mode bits (parked mode 0) but latch the LYC coincidence
		// flag — the comparator is frozen while the LCD is off, so bit 2
		// holds its disable-time value (see RecomputeStatLine / Mr. Do!).
		p.stat = static_cast<uint8_t>(p.stat & 0xFC);
		(void)mem;
		return;
	}

	while (tcycles-- > 0)
		ExecPpuDot(p, mem);
}

uint8_t PpuReadReg(const Ppu &p, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF40: return p.lcdc;
		case 0xFF41:
		{
			uint8_t v = static_cast<uint8_t>(p.stat | 0x80);   // bit 7 always 1
			// Line-start glitch: for the first 4 dots of each visible line
			// the mode bits still read 0 even though the OAM scan (and its
			// STAT interrupt) have begun (SameBoy / mooneye lcdon tests).
			{
				static int msk = -1, v3 = -1, v0 = -1;
				if (msk < 0) { const char *e = getenv("ACID_MSK"); msk = e ? atoi(e) : -3;
				               const char *f = getenv("ACID_V3"); v3 = f ? atoi(f) : 0;
				               const char *g = getenv("ACID_V0"); v0 = g ? atoi(g) : 0; }
				if (!p.cgb && p.mode == PpuMode::OamScan &&
				    (p.mode_clock <= -msk || p.lcdon_first))
					v = static_cast<uint8_t>(v & ~0x03);
				// The CPU-visible mode runs a few dots ahead of the render
				// machine (mooneye lcdon_timing pins the absolute grid).
				else if (!p.cgb && !p.lcdon_line && p.mode == PpuMode::OamScan &&
				         p.mode_clock >= MODE2_DOTS + 7 - v3)
					v = static_cast<uint8_t>((v & ~0x03) | 0x03);
				else if (!p.cgb && !p.lcdon_line && p.mode == PpuMode::Transfer &&
				         v0 > 0 && p.tm.lcd_x >= GB_SCREEN_WIDTH - v0)
					v = static_cast<uint8_t>(v & ~0x03);
				else if (!p.cgb && p.mode == PpuMode::HBlank &&
				         (p.lcdc & 0x80) && p.stop_display == 0 && !p.om.done)
				{
					// STAT keeps reading mode 3 while the LCD is still
					// taking this line's last pixels.
					v = static_cast<uint8_t>((v & ~0x03) | 0x03);
				}
			}
			// Around a line flip the coincidence flag walks old-LY compare,
			// then a matches-nothing gap, then the new LY (Mesen LyForCompare;
			// mooneye lcdon_timing).
			static int cmp1 = -1, cmp2 = -1;
			if (cmp1 < 0) { const char *e = getenv("ACID_CMP1"); cmp1 = e ? atoi(e) : 4;
			                const char *f = getenv("ACID_CMP2"); cmp2 = f ? atoi(f) : 4; }
			if (p.ly == p.ly_prev + 1)
			{
				const int64_t dt = p.t_cycles - p.ly_change_t;
				if (dt < cmp1)
				{
					v = static_cast<uint8_t>(v & ~0x04);
					if (p.ly_prev == p.lyc)
						v = static_cast<uint8_t>(v | 0x04);
				}
				else if (dt < cmp1 + cmp2)
					v = static_cast<uint8_t>(v & ~0x04);
			}
			return v;
		}
		case 0xFF42: return p.scy;
		case 0xFF43: return p.scx;
		case 0xFF44:
		{
			// FF44 reads lag the internal counter by a few dots — the
			// LY flip lands mid-line-start pipeline (hblank_ly_scx).
			static int lyk = -1;
			if (lyk < 0) { const char *e = getenv("ACID_LYK"); lyk = e ? atoi(e) : 4; }
			if (!p.cgb && p.t_cycles - p.ly_change_t < lyk && p.ly == p.ly_prev + 1)
				return p.ly_prev;
			return p.ly;
		}
		case 0xFF45: return p.lyc;
		case 0xFF47: return p.bgp;
		case 0xFF48: return p.obp0;
		case 0xFF49: return p.obp1;
		case 0xFF4A: return p.wy;
		case 0xFF4B: return p.wx;
	}
	return 0xFF;
}

void PpuWriteReg(Ppu &p, Memory &mem, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF40:
		{
			const bool was_on = (p.lcdc & 0x80) != 0;
			// DMG: dropping OBJ enable mid-object-fetch aborts the fetch
			// (SameBoy object_fetch_aborted; m3_lcdc_obj_en_change).
			if (!p.cgb && (p.lcdc & 0x02) && !(value & 0x02))
			{
				for (PixelMachine *pm : { &p.tm, &p.om })
					if (pm->during_obj) { pm->obj_fetch_state = 0; pm->during_obj = false; }
			}
			// Window bit dropped while the window row is still fetching:
			// the insertion glitch stays off for the rest of the line.
			if (!p.cgb && (p.lcdc & 0x20) && !(value & 0x20) &&
			    PpuRendering(p))
				for (PixelMachine *pm : { &p.tm, &p.om })
					if (pm->win_fresh) pm->win_insert_disable = true;
			p.lcdc = value;
			const bool is_on  = (p.lcdc & 0x80) != 0;
			if (was_on && !is_on && p.cgb && p.mode != PpuMode::HBlank)
				MemHdmaLcdOff(mem);
			// LCD turn-on resets to line 0 / mode 2.
			if (!was_on && is_on)
			{
				p.ly          = 0;
				p.mode        = PpuMode::OamScan;
				p.mode_clock  = 0;
				p.tm.window_line = -1;
				p.om.window_line = -1;
				p.lcdon_first = true;
				p.present_hold = p.hold_present_on_enable;
			}
			if (is_on) RecomputeStatLine(p, mem);
			break;
		}
		case 0xFF41:
		{
			// DMG STAT write quirk (Pan Docs §STAT.spurious-stat-interrupts):
			// On DMG, writing to STAT momentarily glitches the enable bits
			// to all-1 for one cycle. If a source condition (mode 0/1/2 or
			// LYC=LY) is currently active during that glitch AND the STAT
			// line was previously low, a spurious LCDSTAT IRQ is raised.
			//
			// Zerd no Densetsu's bank-1 init relies on this. At $62DB
			// (SET 6 / LDH ($41),A) the PPU is mid-VBlank (mode 1 active).
			// The quirk fires IF.STAT immediately — with IME=0 from the
			// surrounding DI region it sits pending; after the $62E7 EI
			// and the CALL into $021E, the IRQ dispatches with bank=1
			// still mapped, so the trampoline at $C2CC reads the real
			// handler in bank 1. Without the quirk, IF.STAT only sets on
			// the next LYC=0 edge ~3000 cycles later — by then the wait
			// loop has entered the bank-7 sound CALL ($4023) inside its
			// DI region, the trampoline reads bank 7 garbage ($7B), and
			// the CPU falls into RST 38 forever.
			//
			// Edge gate: only fire the quirk when the write ACTUALLY
			// newly-enables at least one source bit (3..6 going 0→1).
			// Idempotent rewrites (e.g. STAT=$40 → STAT=$40) don't
			// produce a hardware-visible edge — without this gate
			// Initial D Gaiden's racing-scene STAT handler over-fires
			// itself catastrophically: the handler does EI early then
			// writes STAT idempotently as part of its work, each write
			// quirk-firing a new STAT IRQ that nests on top of the
			// running handler, stack-bombing WRAM down hundreds of bytes
			// per frame and walking the per-scanline SCY/SCX/BGP table
			// way past one entry per line. The road perspective falls
			// apart into stripes.
			// DMG/SGB-only: CGB fixed the STAT-write spurious-IRQ bug. Gating on
			// !p.cgb stops NASCAR 2000's per-line STAT=$08 rewrite from double-
			// firing the handler (which scrambled the digitized title).
			const uint8_t old_enables = static_cast<uint8_t>(p.stat & 0x78);
			const uint8_t new_enables = static_cast<uint8_t>(value & 0x78);
			const bool    newly_set   = (~old_enables & new_enables) != 0;
			if (!p.cgb && newly_set && !p.stat_line_high && (p.lcdc & 0x80))
			{
				const bool any_source_active =
					p.mode == PpuMode::HBlank ||
					p.mode == PpuMode::VBlank ||
					p.mode == PpuMode::OamScan ||
					p.ly == p.lyc;
				if (any_source_active)
					mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_LCDSTAT);
			}
			p.stat = static_cast<uint8_t>((p.stat & 0x07) | (value & 0x78));
			RecomputeStatLine(p, mem);
			break;
		}
		case 0xFF42: p.scy = value; break;
		case 0xFF43: p.scx = value; break;
		case 0xFF44: break;   // LY is read-only on hardware
		case 0xFF45: p.lyc = value; RecomputeStatLine(p, mem); break;
		case 0xFF47:
		{
			// DMG palette-write glitch: the dot the store lands on renders
			// with (old | new); the clean value takes hold next dot.
			if (!p.cgb && PpuRendering(p) && g_pal_unit == 0)
			{
				{ static int pgl = -1;
				  if (pgl < 0) { const char *e = getenv("ACID_PGL"); pgl = e ? atoi(e) : 2; }
				  p.pal_glitch = static_cast<uint8_t>(pgl); }
				p.pal_glitch_reg= 0;
				p.pal_glitch_next = value;
				p.bgp = static_cast<uint8_t>(p.bgp | value);
			}
			else
				p.bgp = value;
			if (PpuRendering(p))
				p.latched_bgp = value;
			// DMG raster write-time reconstruction. In the per-dot interleave
			// the CPU trails the PPU by up to kMaxOpcodeTCycles, so by the
			// time this store reaches us the PPU has already emitted the
			// pixels the write was aimed at, using the stale palette.
			// Prehistorik Man's title streams BGP from a WRAM LD (HL),D/E
			// chain timed so its first write lands in the 2-dot window
			// before pixel 0 — landing a lag-window late instead exposed the
			// previous line's blackout palette as a black block on the left
			// edge. The raw 2-bit indices for the line are still in
			// scanline_bg_raw, so re-map the lag window's pixels with the
			// new palette. OBJ pixels keep their OBP colors. cpu->t_cycles
			// still holds the instruction-start time when the store lands
			// (cycles are added after Dispatch); +4 puts the effective dot
			// on the store's actual memory microcycle.
			if (!p.cgb && mem.cpu && !p.present_hold &&
			    p.ly < GB_SCREEN_HEIGHT && (p.lcdc & 0x80))
			{
				int64_t lag = p.t_cycles - (mem.cpu->t_cycles + 4);
				if (lag > 0 && lag <= 2 * kMaxOpcodeTCycles)
				{
					int x_end = -1;   // one past the last stale pixel
					if (p.mode == PpuMode::Transfer)
						x_end = static_cast<int>(p.draw_x);
					else if (p.mode == PpuMode::HBlank && lag > p.mode_clock)
					{
						// Store's dot falls back inside mode 3 — the line's
						// tail pixels were emitted with the stale palette.
						x_end = GB_SCREEN_WIDTH;
						lag  -= p.mode_clock;
					}
					if (x_end > 0)
					{
						int x0 = x_end - static_cast<int>(lag);
						if (x0 < 0) x0 = 0;
						uint8_t *const line = &p.framebuffer[p.ly * GB_SCREEN_WIDTH];
						const uint8_t *const lay = &p.layer[p.ly * GB_SCREEN_WIDTH];
						for (int x = x0; x < x_end; ++x)
						{
							if (lay[x] == GB_PIXEL_OBJ)
								continue;
							const bool hidden = (lay[x] == GB_PIXEL_WINDOW)
							                        ? !p.show_window : !p.show_bg;
							line[x] = ApplyPalette(value,
							                       hidden ? 0 : p.scanline_bg_raw[x]);
						}
						// The HBlank back-spill lands AFTER FinalizeScanline
						// already pushed this line into the SGB ICD2 capture
						// ring — the SGB BIOS displays the ring, not the
						// framebuffer, so without a re-push the correction is
						// invisible in BIOS mode (black band on the RIGHT
						// edge, mirror of the left-edge bug). Re-capture the
						// corrected line: sgb_row/bank haven't advanced yet
						// (that happens at HBlank end), so this overwrites
						// the same ring slot, and the BIOS can't have drained
						// this band yet — trailing raster writes arrive
						// within ~35 dots of HBlank entry. Idempotent memcpy;
						// no-op in BIOS-less mode and in the harness.
						if (x_end == GB_SCREEN_WIDTH)
							S9xSGBCaptureScanline(line);
					}
				}
			}
			break;
		}
		case 0xFF48:
			if (!p.cgb && PpuRendering(p) && g_pal_unit == 0)
			{
				{ static int pgl = -1;
				  if (pgl < 0) { const char *e = getenv("ACID_PGL"); pgl = e ? atoi(e) : 2; }
				  p.pal_glitch = static_cast<uint8_t>(pgl); }
				p.pal_glitch_reg= 1;
				p.pal_glitch_next = value;
				p.obp0 = static_cast<uint8_t>(p.obp0 | value);
			}
			else
				p.obp0 = value;
			break;
		case 0xFF49:
			if (!p.cgb && PpuRendering(p) && g_pal_unit == 0)
			{
				{ static int pgl = -1;
				  if (pgl < 0) { const char *e = getenv("ACID_PGL"); pgl = e ? atoi(e) : 2; }
				  p.pal_glitch = static_cast<uint8_t>(pgl); }
				p.pal_glitch_reg= 2;
				p.pal_glitch_next = value;
				p.obp1 = static_cast<uint8_t>(p.obp1 | value);
			}
			else
				p.obp1 = value;
			break;
		case 0xFF4A: p.wy = value;   break;
		case 0xFF4B:
			p.wx = value; p.wx_write_cooldown = 1; break;
	}
}

} // namespace SGB
