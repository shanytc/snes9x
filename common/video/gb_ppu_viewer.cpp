/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_ppu_viewer.hpp"

#include "sgb/sgb.h"

#include <algorithm>

namespace gbviewer
{

namespace
{

constexpr Pixel kGridColor = 0x80808080u;

Pixel rgb555(uint16_t c)
{
    const uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) |
           ((b << 3) | (b >> 2));
}

void gray_palette(uint8_t reg, Pixel pal[4])
{
    for (int i = 0; i < 4; i++)
    {
        const uint32_t v = (uint32_t)(3 - ((reg >> (i * 2)) & 3)) * 85;
        pal[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
}

void cgb_palettes(const uint8_t *palram, Pixel out[8][4])
{
    for (int p = 0; p < 8; p++)
        for (int c = 0; c < 4; c++)
            out[p][c] = rgb555((uint16_t)(palram[p * 8 + c * 2] | (palram[p * 8 + c * 2 + 1] << 8)));
}

void build_palette(int mode, int index, Pixel pal[4])
{
    if (mode == kPalAuto)
        mode = S9xSGBIsCgb() ? kPalCgbBG : kPalBGP;

    if (mode == kPalCgbBG || mode == kPalCgbOBJ)
    {
        const uint8_t *pr = mode == kPalCgbBG ? S9xSGBGetCgbBgPal() : S9xSGBGetCgbObjPal();
        if (pr)
        {
            const int p = index & 7;
            for (int i = 0; i < 4; i++)
                pal[i] = rgb555((uint16_t)(pr[p * 8 + i * 2] | (pr[p * 8 + i * 2 + 1] << 8)));
            return;
        }
    }
    else if (mode == kPalSGB)
    {
        const uint16_t *ap = S9xSGBGetActivePalettes();
        if (ap)
        {
            const int p = index & 3;
            for (int i = 0; i < 4; i++)
                pal[i] = rgb555(ap[p * 4 + i]);
            return;
        }
    }
    else
    {
        SgbPpuRegs r;
        S9xSGBGetPpuRegs(&r);
        gray_palette(mode == kPalOBP0 ? r.obp0 : mode == kPalOBP1 ? r.obp1 : r.bgp, pal);
        return;
    }
    gray_palette(0xE4, pal);
}

/* All four indices drawn. Tiles and tilemaps. */
void blit_opaque(const uint8_t tile[64], const Pixel pal[4], Image &out, int dx, int dy,
                 bool hflip, bool vflip)
{
    for (int y = 0; y < 8; y++)
    {
        const int sy = vflip ? 7 - y : y;
        Pixel *row = out.row(dy + y) + dx;
        for (int x = 0; x < 8; x++)
            row[x] = pal[tile[sy * 8 + (hflip ? 7 - x : x)] & 3];
    }
}

/* Index 0 skipped, clipped to the image. Sprites. */
void blit_clipped(const uint8_t tile[64], const Pixel pal[4], Image &out, int dx, int dy,
                  bool hflip, bool vflip)
{
    for (int y = 0; y < 8; y++)
    {
        const int py = dy + y;
        if (py < 0 || py >= out.height)
            continue;
        const int sy = vflip ? 7 - y : y;
        for (int x = 0; x < 8; x++)
        {
            const int px = dx + x;
            if (px < 0 || px >= out.width)
                continue;
            const uint8_t c = tile[sy * 8 + (hflip ? 7 - x : x)] & 3;
            if (c)
                out.row(py)[px] = pal[c];
        }
    }
}

void draw_grid(Image &out)
{
    for (int y = 0; y < out.height; y++)
        for (int x = 0; x < out.width; x++)
            if (!(x & 7) || !(y & 7))
                out.row(y)[x] = kGridColor;
}

void outline(Image &out, int x, int y, int w, int h, Pixel color)
{
    auto plot = [&](int px, int py) {
        if (px >= 0 && px < out.width && py >= 0 && py < out.height)
            out.row(py)[px] = color;
    };
    for (int i = 0; i < w; i++)
    {
        plot(x + i, y);
        plot(x + i, y + h - 1);
    }
    for (int i = 0; i < h; i++)
    {
        plot(x, y + i);
        plot(x + w - 1, y + i);
    }
}

/* The 160x144 screen at a scroll offset, wrapping around the 256x256 map. */
void outline_wrapped(Image &out, int scx, int scy, Pixel color)
{
    for (int i = 0; i < 160; i++)
    {
        const int x = (scx + i) & 255;
        out.row(scy & 255)[x] = color;
        out.row((scy + 143) & 255)[x] = color;
    }
    for (int i = 0; i < 144; i++)
    {
        const int y = (scy + i) & 255;
        out.row(y)[scx & 255] = color;
        out.row(y)[(scx + 159) & 255] = color;
    }
}

uint16_t map_base(int map, uint8_t lcdc)
{
    const uint8_t bit = map == kMapBG ? 0x08 : 0x40;
    return (lcdc & bit) ? 0x1C00 : 0x1800;
}

} // namespace

const char *palette_mode_name(int mode)
{
    switch (mode)
    {
    case kPalBGP:    return "DMG BGP";
    case kPalOBP0:   return "DMG OBP0";
    case kPalOBP1:   return "DMG OBP1";
    case kPalCgbBG:  return "CGB BG";
    case kPalCgbOBJ: return "CGB OBJ";
    case kPalSGB:    return "SGB";
    default:         return "Auto";
    }
}

/* ---- Tile Viewer --------------------------------------------------------- */

void render_tiles(const TileViewerState &state, Image &out)
{
    const int tiles_x = std::clamp(state.width_tiles, 8, 32);
    const int rows = (kTileCount + tiles_x - 1) / tiles_x;
    out.resize(tiles_x * 8, rows * 8);

    Pixel pal[4];
    build_palette(state.palette_mode, state.palette_index, pal);
    out.fill(pal[0]);

    if (const uint8_t *vram = S9xSGBGetVRAM())
    {
        const uint32_t bank = (state.bank & 1) * 0x2000;
        for (int t = 0; t < kTileCount; t++)
        {
            uint8_t tile[64];
            ppuviewer::decode_tile_bytes(&vram[bank + t * 16], 2, tile);
            blit_opaque(tile, pal, out, (t % tiles_x) * 8, (t / tiles_x) * 8, false, false);
        }
    }

    if (state.show_grid)
        draw_grid(out);
}

int tile_at_pixel(const TileViewerState &state, int x, int y)
{
    const int tiles_x = std::clamp(state.width_tiles, 8, 32);
    if (x < 0 || y < 0 || x >= tiles_x * 8)
        return -1;
    const int tile = (y / 8) * tiles_x + x / 8;
    return tile < kTileCount ? tile : -1;
}

uint16_t tile_address(int tile)
{
    return (uint16_t)(0x8000 + tile * 16);
}

/* ---- Tilemap Viewer ------------------------------------------------------ */

const char *map_name(int map)
{
    return map == kMapWindow ? "Window" : "BG";
}

const char *tile_data_name(int tile_data)
{
    switch (tile_data)
    {
    case kTileData8000: return "0x8000";
    case kTileData8800: return "0x8800";
    default:            return "Auto (LCDC)";
    }
}

const char *map_background_name(int background)
{
    if (background == kMapBackgroundPalette)
        return "Palette";
    return ppuviewer::viewer_bg_name(background - 1);
}

void render_tilemap(const TilemapViewerState &state, Image &out, bool for_export)
{
    out.resize(256, 256);

    const uint8_t *vram = S9xSGBGetVRAM();
    SgbPpuRegs r;
    S9xSGBGetPpuRegs(&r);
    const bool cgb = S9xSGBIsCgb();

    const uint16_t base = map_base(state.map, r.lcdc);
    const bool unsigned_tiles = state.tile_data == kTileData8000   ? true
                                : state.tile_data == kTileData8800 ? false
                                                                   : (r.lcdc & 0x10) != 0;

    Pixel pal[4];
    Pixel cgb_pal[8][4];
    if (cgb)
    {
        if (const uint8_t *bp = S9xSGBGetCgbBgPal())
            cgb_palettes(bp, cgb_pal);
        else
            for (auto &p : cgb_pal)
                gray_palette(0xE4, p);
    }
    else
        build_palette(kPalBGP, 0, pal);

    // Key colour 0 of every palette so index-0 pixels show the chosen background.
    if (state.background != kMapBackgroundPalette)
    {
        const Pixel key = ppuviewer::viewer_bg_color(state.background - 1, for_export);
        pal[0] = key;
        for (auto &p : cgb_pal)
            p[0] = key;
    }

    if (!vram)
        out.fill(0xFF000000u);
    else
        for (int ty = 0; ty < 32; ty++)
            for (int tx = 0; tx < 32; tx++)
            {
                const uint16_t index = (uint16_t)(base + ty * 32 + tx);
                const uint8_t number = vram[index];
                const uint8_t attr = cgb ? vram[0x2000 + index] : 0;
                const uint16_t address = unsigned_tiles ? (uint16_t)(number * 16)
                                                        : (uint16_t)(0x1000 + (int8_t)number * 16);
                const uint32_t bank = (cgb && (attr & 0x08)) ? 0x2000u : 0u;
                uint8_t tile[64];
                ppuviewer::decode_tile_bytes(&vram[bank + address], 2, tile);
                blit_opaque(tile, cgb ? cgb_pal[attr & 7] : pal, out, tx * 8, ty * 8,
                            cgb && (attr & 0x20), cgb && (attr & 0x40));
            }

    if (state.show_grid)
        draw_grid(out);

    // The window always starts at its own map's origin.
    if (state.show_viewport)
        outline_wrapped(out, state.map == kMapBG ? r.scx : 0, state.map == kMapBG ? r.scy : 0,
                        0xFFFF3030u);
}

TilemapCell tilemap_cell(const TilemapViewerState &state, int x, int y)
{
    TilemapCell cell;
    cell.x = std::clamp(x, 0, 255) / 8;
    cell.y = std::clamp(y, 0, 255) / 8;

    SgbPpuRegs r;
    S9xSGBGetPpuRegs(&r);
    const uint16_t index = (uint16_t)(map_base(state.map, r.lcdc) + cell.y * 32 + cell.x);
    cell.map_address = (uint16_t)(0x8000 + index);
    cell.cgb = S9xSGBIsCgb();
    if (const uint8_t *vram = S9xSGBGetVRAM())
    {
        cell.tile = vram[index];
        cell.attr = cell.cgb ? vram[0x2000 + index] : 0;
    }
    return cell;
}

/* ---- Sprite Viewer ------------------------------------------------------- */

void render_sprites(const SpriteViewerState &state, Image &out, bool for_export)
{
    out.resize(256, 256);
    out.fill(ppuviewer::viewer_bg_color(state.background, for_export));

    const uint8_t *vram = S9xSGBGetVRAM();
    const uint8_t *oam = S9xSGBGetOAM();
    SgbPpuRegs r;
    S9xSGBGetPpuRegs(&r);
    const bool cgb = S9xSGBIsCgb();
    const int height = (r.lcdc & 0x04) ? 16 : 8;

    Pixel cgb_pal[8][4];
    Pixel obp0[4], obp1[4];
    if (cgb)
    {
        if (const uint8_t *op = S9xSGBGetCgbObjPal())
            cgb_palettes(op, cgb_pal);
        else
            for (auto &p : cgb_pal)
                gray_palette(0xE4, p);
    }
    else
    {
        gray_palette(r.obp0, obp0);
        gray_palette(r.obp1, obp1);
    }

    // The visible 160x144 screen: OAM's origin is 8 left and 16 above it.
    if (state.show_viewport)
        outline(out, 8, 16, 160, 144, 0xFF404060u);

    if (!vram || !oam)
        return;

    // Back to front, so lower OAM indices end up on top as on hardware.
    for (int i = kSpriteCount - 1; i >= 0; i--)
    {
        const int y = oam[i * 4 + 0];
        const int x = oam[i * 4 + 1];
        const int number = oam[i * 4 + 2];
        const int flags = oam[i * 4 + 3];
        const bool hflip = (flags & 0x20) != 0;
        const bool vflip = (flags & 0x40) != 0;
        const uint32_t bank = (cgb && (flags & 0x08)) ? 0x2000u : 0u;
        const Pixel *pal = cgb ? cgb_pal[flags & 7] : ((flags & 0x10) ? obp1 : obp0);
        const int first = height == 16 ? (number & 0xFE) : number;
        const int halves = height / 8;
        for (int row = 0; row < halves; row++)
        {
            uint8_t tile[64];
            const int sub = first + (vflip ? halves - 1 - row : row);
            ppuviewer::decode_tile_bytes(&vram[bank + sub * 16], 2, tile);
            blit_clipped(tile, pal, out, x, y + row * 8, hflip, vflip);
        }
    }

    const int s = std::clamp(state.selected, 0, kSpriteCount - 1);
    outline(out, oam[s * 4 + 1], oam[s * 4 + 0], 8, height, 0xFFFFFF00u);
}

SpriteInfo sprite_info(int index)
{
    SpriteInfo info;
    const uint8_t *oam = S9xSGBGetOAM();
    if (!oam)
        return info;
    const int s = std::clamp(index, 0, kSpriteCount - 1);
    info.y = oam[s * 4 + 0];
    info.x = oam[s * 4 + 1];
    info.tile = oam[s * 4 + 2];
    info.flags = oam[s * 4 + 3];
    info.cgb = S9xSGBIsCgb();
    info.valid = true;
    return info;
}

} // namespace gbviewer
