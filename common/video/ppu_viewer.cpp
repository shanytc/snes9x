/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "ppu_viewer.hpp"

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "sa1.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <png.h>

#ifdef SYSTEM_ZIP
#  include <minizip/zip.h>
#else
#  include "unzip/zip.h"
#endif

namespace ppuviewer
{

namespace
{

/* A grid line light enough to read over both a dark backdrop and a bright
 * one; the same value win32's viewers use. */
constexpr Pixel kGridColor = 0x80808080u;

uint8_t vram_byte(uint32_t address)
{
    return Memory.VRAM[address & 0xFFFF];
}

/* SNES planar tiles: an 8x8 2bpp tile is 16 bytes, one byte pair per row,
 * with pixel x taking bit (7 - x) out of each. 4bpp adds planes 2/3 sixteen
 * bytes later, 8bpp planes 4/5 and 6/7 sixteen bytes after that again. */
void decode_2bpp_row(const uint8_t *pair, uint8_t out[8])
{
    const uint8_t b0 = pair[0];
    const uint8_t b1 = pair[1];
    for (int x = 0; x < 8; x++)
    {
        const int bit = 7 - x;
        out[x] = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
    }
}

/* Walks a CPU-style memory map (Memory.Map or SA1.Map) for one byte with no
 * side effects. Ported from the private S9xDebugGetByte in debug.cpp. */
uint8_t read_via_map(uint8_t *const *map, uint32_t address)
{
    address &= 0xFFFFFF;
    uint8_t *p = map[address >> MEMMAP_SHIFT];
    if (p >= (uint8_t *)CMemory::MAP_LAST)
        return p[address & 0xFFFF];

    switch ((pint)p)
    {
    case CMemory::MAP_LOROM_SRAM:
    case CMemory::MAP_SA1RAM:
        if (!Memory.SRAMMask)
            return 0;
        return Memory.SRAM[((((address & 0xff0000) >> 1) | (address & 0x7fff)) & Memory.SRAMMask)];
    case CMemory::MAP_HIROM_SRAM:
    case CMemory::MAP_RONLY_SRAM:
        if (!Memory.SRAMMask)
            return 0;
        return Memory.SRAM[((((address & 0x7fff) - 0x6000 + ((address & 0xf0000) >> 3))) & Memory.SRAMMask)];
    case CMemory::MAP_BWRAM:
        return Memory.BWRAM[(address & 0x7fff) - 0x6000];
    default:
        return 0;
    }
}

/* Composites an 8x8 decoded tile into a canvas. Colour 0 is transparent and
 * leaves the destination alone, so the caller's pre-fill (the SNES backdrop,
 * usually) shows through, which is what bsnes-plus does. */
void blit_tile(const uint8_t tile[64], const Pixel palette[256], int palette_offset,
               Pixel *dst, int stride, int x, int y, bool hflip, bool vflip)
{
    for (int ty = 0; ty < 8; ty++)
    {
        const int sy = vflip ? (7 - ty) : ty;
        Pixel *dst_row = dst + (size_t)(y + ty) * stride + x;
        for (int tx = 0; tx < 8; tx++)
        {
            const int sx = hflip ? (7 - tx) : tx;
            const int index = tile[sy * 8 + sx];
            if (index == 0)
                continue;
            dst_row[tx] = palette[(palette_offset + index) & 0xFF];
        }
    }
}

/* One Mode 7 tile. Its character data lives in the odd VRAM bytes, one byte
 * per pixel, 128 bytes to a tile. */
void blit_mode7_tile(uint32_t tile_index, bool extbg, const Pixel palette[256],
                     Pixel *dst, int stride)
{
    const uint32_t base = tile_index * 128 + 1;
    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            uint8_t pixel = vram_byte(base + y * 16 + x * 2);
            if (extbg)
                pixel &= 0x7F;
            if (pixel != 0)
                dst[(size_t)y * stride + x] = palette[pixel];
        }
    }
}

void draw_grid(Image &image, int cell_size)
{
    if (cell_size <= 0)
        return;
    for (int y = 0; y < image.height; y += cell_size)
    {
        Pixel *row = image.row(y);
        for (int x = 0; x < image.width; x++)
            row[x] = kGridColor;
    }
    for (int y = 0; y < image.height; y++)
    {
        Pixel *row = image.row(y);
        for (int x = 0; x < image.width; x += cell_size)
            row[x] = kGridColor;
    }
}

/* ---- Tile Viewer geometry ------------------------------------------------
 * The 8x16 and 16x16 layouts put the halves (quarters) of one object side by
 * side, so a "band" of consecutive tiles becomes two canvas rows. */

int tiles_per_band(int layout, int tiles_x)
{
    switch (layout)
    {
    case kLayout8x16:
        return tiles_x * 2;
    case kLayout16x16:
        return (tiles_x / 2) * 4;
    default:
        return tiles_x;
    }
}

int band_rows(int layout)
{
    return layout == kLayoutNormal ? 1 : 2;
}

void cell_for_tile(int layout, int tiles_x, int tile, int *cell_x, int *cell_y)
{
    switch (layout)
    {
    case kLayout8x16:
    {
        const int per_band = tiles_x * 2;
        const int band = tile / per_band;
        const int rest = tile % per_band;
        *cell_x = rest >> 1;
        *cell_y = band * 2 + (rest & 1);
        return;
    }
    case kLayout16x16:
    {
        const int per_band = (tiles_x / 2) * 4;
        const int band = tile / per_band;
        const int rest = tile % per_band;
        *cell_x = (rest >> 2) * 2 + (rest & 1);
        *cell_y = band * 2 + ((rest & 3) >> 1);
        return;
    }
    default:
        *cell_x = tile % tiles_x;
        *cell_y = tile / tiles_x;
        return;
    }
}

int tile_at_cell(int layout, int tiles_x, int cell_x, int cell_y)
{
    switch (layout)
    {
    case kLayout8x16:
        return (cell_y >> 1) * (tiles_x * 2) + cell_x * 2 + (cell_y & 1);
    case kLayout16x16:
    {
        const int quad_columns = tiles_x / 2;
        if (cell_x >= quad_columns * 2)
            return -1;
        return (cell_y >> 1) * (quad_columns * 4) + (cell_x >> 1) * 4 + (cell_y & 1) * 2 + (cell_x & 1);
    }
    default:
        return cell_y * tiles_x + cell_x;
    }
}

int clamp_width_tiles(int width_tiles)
{
    if (width_tiles < 8)
        return 8;
    if (width_tiles > 64)
        return 64;
    return width_tiles;
}

uint32_t source_upper_bound(int source)
{
    const uint32_t size = source_size(source);
    return size ? size : 0x10000u;
}

int max_tiles(int source, int bit_depth)
{
    if (is_mode7(bit_depth))
        return 256;
    const uint32_t size = source_upper_bound(source);
    const uint32_t per_tile = (uint32_t)tile_bytes(bit_depth);
    if (size < per_tile)
        return 0;
    uint32_t count = size / per_tile;
    if (count > 65536)
        count = 65536;
    return (int)count;
}

/* The palette the tiles get indexed through: CGRAM as it stands, or a
 * grayscale ramp covering the bit depth's colour count. */
void build_tile_palette(const TileViewerState &state, Pixel palette[256])
{
    if (state.use_cgram)
    {
        snapshot_palette(palette);
        return;
    }
    int colors = tile_colors(state.bit_depth);
    if (colors < 2)
        colors = 2;
    for (int i = 0; i < 256; i++)
    {
        const uint32_t value = (uint32_t)((i % colors) * 255 / (colors - 1));
        palette[i] = 0xFF000000u | (value << 16) | (value << 8) | value;
    }
}

void draw_tile_into_canvas(const TileViewerState &state, int tile, const Pixel palette[256],
                           Image &image)
{
    const int tiles_x = clamp_width_tiles(state.width_tiles);
    int cell_x, cell_y;
    cell_for_tile(state.layout, tiles_x, tile, &cell_x, &cell_y);
    const int x = cell_x * 8;
    const int y = cell_y * 8;
    if (x + 8 > image.width || y + 8 > image.height)
        return;

    Pixel *dst = image.row(y) + x;

    if (is_mode7(state.bit_depth))
    {
        blit_mode7_tile(tile & 0xFF, state.bit_depth == kMode7ExtBG, palette, dst, image.width);
        return;
    }

    const int bpp = state.bit_depth == kBpp2 ? 2 : state.bit_depth == kBpp4 ? 4 : 8;
    const int bytes = tile_bytes(state.bit_depth);
    const uint32_t limit = source_upper_bound(state.source);
    const uint32_t base = state.address + (uint32_t)tile * (uint32_t)bytes;

    uint8_t raw[64];
    for (int i = 0; i < bytes; i++)
    {
        const uint32_t address = base + i;
        raw[i] = address < limit ? read_source_byte(state.source, address) : 0;
    }

    uint8_t decoded[64];
    decode_tile_bytes(raw, bpp, decoded);
    blit_tile(decoded, palette, state.palette_offset & 0xFF, image.pixels.data(), image.width,
              x, y, false, false);
}

/* ---- Tilemap Viewer ------------------------------------------------------ */

/* bsnes-plus bitDepthForLayer(): 0 means the layer does not exist in that
 * mode. Mode 7 is handled on its own. */
int bpp_for_mode(int mode, int bg)
{
    static const int table[8][4] = {
        { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 },
        { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 0, 0, 0, 0 },
    };
    return table[mode & 7][bg & 3];
}

/* Where 8x8 character `c` lives. 16x16 cells resolve their four sub-tiles
 * separately in draw_cell(). */
uint32_t character_address(const TilemapInfo &info, unsigned c)
{
    switch (info.bpp)
    {
    case 8:
        return (info.tile_address + c * 64) & 0xFFC0;
    case 4:
        return (info.tile_address + c * 32) & 0xFFE0;
    case 2:
        return (info.tile_address + c * 16) & 0xFFF0;
    default:
        return 0;
    }
}

/* One 8x8 tile at the map's bit depth. Colour 0 leaves the destination
 * untouched so the backdrop shows through. */
void draw_map_tile(const TilemapInfo &info, uint32_t char_address, unsigned palette_offset,
                   bool hflip, bool vflip, const Pixel palette[256], Pixel *dst, int stride)
{
    for (int y = 0; y < 8; y++)
    {
        const int sy = vflip ? (7 - y) : y;
        const uint32_t sliver = char_address + sy * 2;

        const uint8_t p0 = vram_byte(sliver);
        const uint8_t p1 = vram_byte(sliver + 1);
        uint8_t p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0;
        if (info.bpp >= 4)
        {
            p2 = vram_byte(sliver + 16);
            p3 = vram_byte(sliver + 17);
        }
        if (info.bpp >= 8)
        {
            p4 = vram_byte(sliver + 32);
            p5 = vram_byte(sliver + 33);
            p6 = vram_byte(sliver + 48);
            p7 = vram_byte(sliver + 49);
        }

        Pixel *dst_row = dst + (size_t)y * stride;
        for (int x = 0; x < 8; x++)
        {
            const uint8_t mask = 0x80 >> x;
            uint8_t pixel = 0;
            if (p0 & mask) pixel |= 0x01;
            if (p1 & mask) pixel |= 0x02;
            if (info.bpp >= 4)
            {
                if (p2 & mask) pixel |= 0x04;
                if (p3 & mask) pixel |= 0x08;
            }
            if (info.bpp >= 8)
            {
                if (p4 & mask) pixel |= 0x10;
                if (p5 & mask) pixel |= 0x20;
                if (p6 & mask) pixel |= 0x40;
                if (p7 & mask) pixel |= 0x80;
            }
            if (pixel != 0)
                dst_row[hflip ? (7 - x) : x] = palette[(palette_offset + pixel) & 0xFF];
        }
    }
}

/* One map entry, covering an 8x8 or a 16x16 cell. */
void draw_cell(const TilemapInfo &info, uint32_t entry_address, int x, int y,
               const Pixel palette[256], Image &image)
{
    const uint16_t entry = (uint16_t)(vram_byte(entry_address) | (vram_byte(entry_address + 1) << 8));

    const unsigned character = entry & 0x03FF;
    const unsigned palette_index = (entry >> 10) & 7;
    const bool hflip = (entry & 0x4000) != 0;
    const bool vflip = (entry & 0x8000) != 0;

    /* In 2bpp mode 0 each layer gets its own 32-colour bank; 4bpp indexes
     * 16-colour blocks; 8bpp goes straight into CGRAM. */
    unsigned palette_offset = 0;
    if (info.bpp == 2)
        palette_offset = palette_index * 4 + (info.mode == 0 ? info.bg * 32 : 0);
    else if (info.bpp == 4)
        palette_offset = palette_index * 16;

    Pixel *dst = image.row(y) + x;

    if (info.tile_size == 8)
    {
        draw_map_tile(info, character_address(info, character), palette_offset, hflip, vflip,
                      palette, dst, image.width);
        return;
    }

    /* 16x16: four sub-tiles, with the flips swapping which goes where. */
    unsigned c1 = character;
    unsigned c2 = (character & 0x3F0) | ((character + 1) & 0x00F);
    if (hflip)
        std::swap(c1, c2);
    unsigned c3 = c1 + 0x10;
    unsigned c4 = c2 + 0x10;
    if (vflip)
    {
        std::swap(c1, c3);
        std::swap(c2, c4);
    }
    draw_map_tile(info, character_address(info, c1), palette_offset, hflip, vflip, palette,
                  dst, image.width);
    draw_map_tile(info, character_address(info, c2), palette_offset, hflip, vflip, palette,
                  dst + 8, image.width);
    draw_map_tile(info, character_address(info, c3), palette_offset, hflip, vflip, palette,
                  dst + (size_t)8 * image.width, image.width);
    draw_map_tile(info, character_address(info, c4), palette_offset, hflip, vflip, palette,
                  dst + (size_t)8 * image.width + 8, image.width);
}

/* One 32x32-cell screen. Wider or taller layouts pack their screens
 * sequentially in VRAM, 0x800 bytes apart. */
void draw_screen(const TilemapInfo &info, uint32_t map_address, int x0, int y0,
                 const Pixel palette[256], Image &image)
{
    for (int ty = 0; ty < 32; ty++)
    {
        for (int tx = 0; tx < 32; tx++)
        {
            const uint32_t entry = (map_address + (ty * 32 + tx) * 2) & 0xFFFF;
            draw_cell(info, entry, x0 + tx * info.tile_size, y0 + ty * info.tile_size,
                      palette, image);
        }
    }
}

void draw_mode7_map(const Pixel palette[256], Image &image)
{
    for (int ty = 0; ty < 128; ty++)
    {
        for (int tx = 0; tx < 128; tx++)
        {
            /* Mode 7 map entries are the even VRAM bytes. */
            const uint8_t character = vram_byte((ty * 128 + tx) * 2);
            blit_mode7_tile(character, false, palette, image.row(ty * 8) + tx * 8, image.width);
        }
    }
}

/* ---- Sprite Viewer ------------------------------------------------------- */

/* Where object tile `n` lives, matching tileimpl.h: the second name table is
 * reached by the OBJNameSelect gap, not by the tile stride. */
uint32_t sprite_tile_address(uint16_t tile)
{
    uint32_t address = PPU.OBJNameBase + (tile & 0x3FF) * 32;
    if (tile & 0x100)
        address += PPU.OBJNameSelect;
    return address & 0xFFFF;
}

/* Object tiles sit in a 16-wide grid that wraps inside the current name
 * table: the table bit is kept, the row and column wrap in their own fields. */
uint16_t sprite_sub_tile(uint16_t name, int dx, int dy)
{
    const uint16_t table = (uint16_t)(name & 0x100);
    const uint16_t column = (uint16_t)((name + dx) & 0x00F);
    const uint16_t row = (uint16_t)((name + dy * 16) & 0x0F0);
    return (uint16_t)(table | row | column);
}

/* Draws one object into a canvas at (x, y), clipped to it. */
void draw_sprite(const Sprite &sprite, int size_select, int x, int y,
                 const Pixel palette[256], Pixel *dst, int stride, int height)
{
    int width_px, height_px;
    sprite_size(size_select, sprite.large, &width_px, &height_px);
    const int palette_offset = 128 + (sprite.palette & 7) * 16;

    const int tiles_across = width_px / 8;
    const int tiles_down = height_px / 8;

    for (int ty = 0; ty < tiles_down; ty++)
    {
        for (int tx = 0; tx < tiles_across; tx++)
        {
            const int dx = sprite.hflip ? (tiles_across - 1 - tx) : tx;
            const int dy = sprite.vflip ? (tiles_down - 1 - ty) : ty;

            uint8_t tile[64];
            decode_tile(sprite_tile_address(sprite_sub_tile(sprite.name, dx, dy)), 4, tile);

            const int px = x + tx * 8;
            const int py = y + ty * 8;
            if (px + 8 <= 0 || py + 8 <= 0 || px >= stride || py >= height)
                continue;

            for (int yy = 0; yy < 8; yy++)
            {
                const int sy = sprite.vflip ? (7 - yy) : yy;
                const int dest_y = py + yy;
                if (dest_y < 0 || dest_y >= height)
                    continue;
                Pixel *row = dst + (size_t)dest_y * stride;
                for (int xx = 0; xx < 8; xx++)
                {
                    const int sx = sprite.hflip ? (7 - xx) : xx;
                    const uint8_t index = tile[sy * 8 + sx];
                    if (index == 0)
                        continue;
                    const int dest_x = px + xx;
                    if (dest_x < 0 || dest_x >= stride)
                        continue;
                    row[dest_x] = palette[(palette_offset + index) & 0xFF];
                }
            }
        }
    }
}

/* ---- PNG ----------------------------------------------------------------- */

void png_write_to_vector(png_structp png, png_bytep data, png_size_t length)
{
    auto *out = (std::vector<uint8_t> *)png_get_io_ptr(png);
    out->insert(out->end(), data, data + length);
}

void png_flush_nothing(png_structp) {}

bool write_png_common(png_structp png, png_infop info, const Image &image)
{
    if (setjmp(png_jmpbuf(png)))
        return false;

    png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_bgr(png); // the buffers are BGRA in memory; libpng swaps to RGBA
    png_write_info(png, info);

    std::vector<png_bytep> rows((size_t)image.height);
    for (int y = 0; y < image.height; y++)
        rows[y] = (png_bytep)(image.pixels.data() + (size_t)y * image.width);
    png_write_image(png, rows.data());
    png_write_end(png, info);
    return true;
}

} // namespace

/* ---- Image --------------------------------------------------------------- */

void Image::resize(int w, int h)
{
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    width = w;
    height = h;
    pixels.assign((size_t)w * h, 0u);
}

void Image::fill(Pixel color)
{
    std::fill(pixels.begin(), pixels.end(), color);
}

/* ---- Tile sources -------------------------------------------------------- */

const char *source_name(int source)
{
    switch (source)
    {
    case kSourceVRAM:    return "VRAM";
    case kSourceCPUBus:  return "S-CPU Bus";
    case kSourceCartROM: return "Cartridge ROM";
    case kSourceCartRAM: return "Cartridge RAM";
    case kSourceSA1Bus:  return "SA1 Bus";
    case kSourceSFXBus:  return "SFX Bus";
    default:             return "";
    }
}

uint8_t read_source_byte(int source, uint32_t address)
{
    switch (source)
    {
    case kSourceVRAM:
        return Memory.VRAM[address & 0xFFFF];
    case kSourceCPUBus:
        return read_via_map(Memory.Map, address);
    case kSourceCartROM:
        if (Memory.CalculatedSize == 0)
            return 0;
        return Memory.ROM[address % Memory.CalculatedSize];
    case kSourceCartRAM:
        if (!Memory.SRAMMask)
            return 0;
        return Memory.SRAM[address & Memory.SRAMMask];
    case kSourceSA1Bus:
        if (!Settings.SA1)
            return 0;
        return read_via_map(SA1.Map, address);
    case kSourceSFXBus:
        /* The SuperFX shares the CPU map; use it as a best-effort source. */
        if (!Settings.SuperFX)
            return 0;
        return read_via_map(Memory.Map, address);
    default:
        return 0;
    }
}

uint32_t source_size(int source)
{
    switch (source)
    {
    case kSourceVRAM:    return 0x10000;
    case kSourceCPUBus:  return 0x1000000;
    case kSourceCartROM: return Memory.CalculatedSize;
    case kSourceCartRAM: return Memory.SRAMMask ? (Memory.SRAMMask + 1u) : 0;
    case kSourceSA1Bus:  return Settings.SA1 ? 0x1000000u : 0;
    case kSourceSFXBus:  return Settings.SuperFX ? 0x1000000u : 0;
    default:             return 0;
    }
}

bool source_available(int source)
{
    switch (source)
    {
    case kSourceVRAM:    return true;
    case kSourceCPUBus:  return Memory.CalculatedSize > 0;
    case kSourceCartROM: return Memory.CalculatedSize > 0;
    case kSourceCartRAM: return Memory.SRAMMask > 0;
    case kSourceSA1Bus:  return Settings.SA1 != 0;
    case kSourceSFXBus:  return Settings.SuperFX != 0;
    default:             return false;
    }
}

/* ---- Decoding ------------------------------------------------------------ */

void decode_tile_bytes(const uint8_t *tile_bytes, int bpp, uint8_t out[64])
{
    for (int y = 0; y < 8; y++)
    {
        uint8_t row01[8] = {};
        decode_2bpp_row(&tile_bytes[y * 2], row01);

        if (bpp == 2)
        {
            for (int x = 0; x < 8; x++)
                out[y * 8 + x] = row01[x];
            continue;
        }

        uint8_t row23[8] = {};
        decode_2bpp_row(&tile_bytes[16 + y * 2], row23);

        if (bpp == 4)
        {
            for (int x = 0; x < 8; x++)
                out[y * 8 + x] = row01[x] | (row23[x] << 2);
            continue;
        }

        uint8_t row45[8] = {};
        uint8_t row67[8] = {};
        decode_2bpp_row(&tile_bytes[32 + y * 2], row45);
        decode_2bpp_row(&tile_bytes[48 + y * 2], row67);
        for (int x = 0; x < 8; x++)
            out[y * 8 + x] = row01[x] | (row23[x] << 2) | (row45[x] << 4) | (row67[x] << 6);
    }
}

void decode_tile(uint32_t vram_byte_address, int bpp, uint8_t out[64])
{
    const int bytes = bpp == 2 ? 16 : bpp == 4 ? 32 : 64;
    uint8_t buffer[64];
    for (int i = 0; i < bytes; i++)
        buffer[i] = vram_byte(vram_byte_address + i);
    decode_tile_bytes(buffer, bpp, out);
}

void snapshot_palette(Pixel out[256])
{
    for (int i = 0; i < 256; i++)
    {
        const uint16_t bgr = PPU.CGDATA[i];
        uint32_t r = bgr & 0x001F;
        uint32_t g = (bgr & 0x03E0) >> 5;
        uint32_t b = (bgr & 0x7C00) >> 10;
        /* 5 bits to 8, replicating the top three into the low ones. */
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* ---- Tile Viewer --------------------------------------------------------- */

const char *bit_depth_name(int depth)
{
    switch (depth)
    {
    case kBpp2:       return "2bpp";
    case kBpp4:       return "4bpp";
    case kBpp8:       return "8bpp";
    case kMode7:      return "Mode 7";
    case kMode7ExtBG: return "Mode 7 EXTBG";
    default:          return "";
    }
}

const char *tile_layout_name(int layout)
{
    switch (layout)
    {
    case kLayoutNormal: return "Normal";
    case kLayout8x16:   return "8x16 (same line)";
    case kLayout16x16:  return "16x16 (same line)";
    default:            return "";
    }
}

int tile_bytes(int bit_depth)
{
    switch (bit_depth)
    {
    case kBpp2:       return 16;
    case kBpp4:       return 32;
    case kBpp8:       return 64;
    case kMode7:
    case kMode7ExtBG: return 128;
    default:          return 32;
    }
}

int tile_colors(int bit_depth)
{
    switch (bit_depth)
    {
    case kBpp2: return 4;
    case kBpp4: return 16;
    default:    return 256;
    }
}

bool is_mode7(int bit_depth)
{
    return bit_depth == kMode7 || bit_depth == kMode7ExtBG;
}

uint32_t align_address(int bit_depth, uint32_t address)
{
    switch (bit_depth)
    {
    case kBpp8: return address & ~0x3Fu;
    case kBpp4: return address & ~0x1Fu;
    case kBpp2: return address & ~0x0Fu;
    default:    return 0;
    }
}

uint32_t address_mask(int source)
{
    return source == kSourceVRAM ? 0xFFFFu : 0xFFFFFFu;
}

void render_tiles(const TileViewerState &state, Image &out)
{
    Pixel palette[256];
    build_tile_palette(state, palette);

    int total_tiles = max_tiles(state.source, state.bit_depth);
    if (!is_mode7(state.bit_depth))
    {
        int first = (int)(state.address / (uint32_t)tile_bytes(state.bit_depth));
        if (first >= total_tiles)
            first = 0;
        total_tiles -= first;
        if (total_tiles < 0)
            total_tiles = 0;
    }

    const int tiles_x = clamp_width_tiles(state.width_tiles);
    const int rows_per_band = band_rows(state.layout);
    int per_band = tiles_per_band(state.layout, tiles_x);
    if (per_band < 1)
        per_band = 1;
    const int bands = (total_tiles + per_band - 1) / per_band;

    int height = bands * rows_per_band * 8;
    if (height > kTileCanvasMaxHeight)
        height = kTileCanvasMaxHeight;
    if (height < 8)
        height = 8;

    out.resize(tiles_x * 8, height);
    /* Colour 0 is transparent, so pre-fill with the backdrop it stands for. */
    out.fill(palette[0]);

    int drawable = ((height / 8) / rows_per_band) * per_band;
    if (drawable > total_tiles)
        drawable = total_tiles;
    for (int tile = 0; tile < drawable; tile++)
        draw_tile_into_canvas(state, tile, palette, out);

    if (state.show_grid)
        draw_grid(out, 8);
}

int tile_at_pixel(const TileViewerState &state, int x, int y)
{
    if (x < 0 || y < 0)
        return -1;
    return tile_at_cell(state.layout, clamp_width_tiles(state.width_tiles), x / 8, y / 8);
}

uint32_t tile_address(const TileViewerState &state, int tile_index)
{
    if (is_mode7(state.bit_depth))
        return (uint32_t)(tile_index & 0xFF) * 128 + 1;
    return (state.address + (uint32_t)tile_index * (uint32_t)tile_bytes(state.bit_depth)) &
           address_mask(state.source);
}

void step_address(TileViewerState &state, bool forward)
{
    if (is_mode7(state.bit_depth))
        return;

    const uint32_t step = (uint32_t)tile_bytes(state.bit_depth) *
                          (uint32_t)clamp_width_tiles(state.width_tiles);
    const uint32_t limit = source_upper_bound(state.source);

    if (forward)
    {
        uint32_t address = state.address + step;
        if (limit && address + step > limit)
            address = limit > step ? (limit - step) : 0;
        state.address = address;
    }
    else
    {
        state.address = state.address >= step ? (state.address - step) : 0;
    }
    state.address = align_address(state.bit_depth, state.address);
}

void base_addresses(uint32_t out[kBaseAddressCount])
{
    for (int i = 0; i < 4; i++)
        out[i] = (uint32_t)(PPU.BG[i].NameBase << 1) & 0xFFFF;
    out[4] = (uint32_t)PPU.OBJNameBase & 0xFFFF;
    out[5] = (uint32_t)(PPU.OBJNameBase + 0x2000 + PPU.OBJNameSelect) & 0xFFFF;
}

int base_address_bit_depth(int which)
{
    if (which >= 4)
        return kBpp4; // objects are always 4bpp

    const int mode = PPU.BGMode & 7;
    if (mode == 7)
        return kBpp4; // BG1-4 do not map onto a Mode 7 screen

    switch (bpp_for_mode(mode, which))
    {
    case 2:  return kBpp2;
    case 8:  return kBpp8;
    default: return kBpp4;
    }
}

/* ---- Tilemap Viewer ------------------------------------------------------ */

int tilemap_bit_depth_index(int bpp)
{
    switch (bpp)
    {
    case 2:  return kTilemapBpp2;
    case 8:  return kTilemapBpp8;
    default: return kTilemapBpp4;
    }
}

TilemapInfo resolve_tilemap(const TilemapViewerState &state)
{
    TilemapInfo info;
    info.bg = state.bg & 3;
    info.mode = state.custom_screen_mode ? (state.custom_mode & 7) : (PPU.BGMode & 7);

    auto make_mode7 = [&info]() {
        info.mode7 = true;
        info.bpp = 8;
        info.width_tiles = 128;
        info.height_tiles = 128;
        info.tile_size = 8;
        info.map_address = 0;
        info.tile_address = 0;
        info.valid = true;
    };

    if (info.mode == 7)
    {
        make_mode7();
        return info;
    }

    if (state.override_tilemap)
    {
        if ((state.override_bit_depth & 3) == kTilemapMode7)
        {
            make_mode7();
            return info;
        }
        static const int bpp_map[4] = { 2, 4, 8, 0 };
        info.bpp = bpp_map[state.override_bit_depth & 3];
        info.width_tiles = (state.override_map_size & 1) ? 64 : 32;
        info.height_tiles = (state.override_map_size & 2) ? 64 : 32;
        info.tile_size = state.override_tile_size ? 16 : 8;
        info.map_address = state.override_map_address & 0xFFFF;
        info.tile_address = state.override_tile_address & 0xFFFF;
    }
    else
    {
        info.bpp = bpp_for_mode(info.mode, info.bg);
        info.tile_size = PPU.BG[info.bg].BGSize ? 16 : 8;
        info.width_tiles = (PPU.BG[info.bg].SCSize & 1) ? 64 : 32;
        info.height_tiles = (PPU.BG[info.bg].SCSize & 2) ? 64 : 32;
        info.map_address = (uint32_t)(PPU.BG[info.bg].SCBase << 1) & 0xFFFF;
        info.tile_address = (uint32_t)(PPU.BG[info.bg].NameBase << 1) & 0xFFFF;
    }

    info.valid = info.bpp != 0;
    return info;
}

const char *viewer_bg_name(int color)
{
    switch (color)
    {
    case kViewerBgMagenta: return "Magenta";
    case kViewerBgCyan:    return "Cyan";
    case kViewerBgWhite:   return "White";
    case kViewerBgBlack:   return "Black";
    default:               return "Transparent";
    }
}

Pixel viewer_bg_color(int color, bool for_export)
{
    switch (color)
    {
    case kViewerBgMagenta: return 0xFFFF00FFu;
    case kViewerBgCyan:    return 0xFF00FFFFu;
    case kViewerBgWhite:   return 0xFFFFFFFFu;
    case kViewerBgBlack:   return 0xFF000000u;
    default:               return for_export ? 0x00000000u : 0xFF303030u;
    }
}

const char *tilemap_background_name(int background)
{
    if (background == kTilemapBackdrop)
        return "Backdrop (Color 0)";
    return viewer_bg_name(background - 1);
}

Pixel tilemap_background_color(int background, const Pixel palette[256], bool for_export)
{
    if (background == kTilemapBackdrop)
        return palette[0];
    return viewer_bg_color(background - 1, for_export);
}

void render_tilemap(const TilemapViewerState &state, Image &out, TilemapInfo *out_info,
                    bool for_export)
{
    const TilemapInfo info = resolve_tilemap(state);
    if (out_info)
        *out_info = info;

    Pixel palette[256];
    snapshot_palette(palette);

    /* Colour 0 of every tile is left untouched, so this shows through the map
     * wherever it is transparent, not just past its edges. */
    const Pixel background = tilemap_background_color(state.background, palette, for_export);

    if (!info.valid)
    {
        out.resize(8, 8);
        out.fill(background);
        return;
    }

    if (info.mode7)
    {
        out.resize(1024, 1024);
        out.fill(background);
        draw_mode7_map(palette, out);
        if (state.show_grid)
            draw_grid(out, 8);
        return;
    }

    const int width = info.width_tiles * info.tile_size;
    const int height = info.height_tiles * info.tile_size;
    out.resize(width, height);
    out.fill(background);

    uint32_t address = info.map_address;
    const int screen_px = 32 * info.tile_size;
    for (int y = 0; y < height; y += screen_px)
    {
        for (int x = 0; x < width; x += screen_px)
        {
            draw_screen(info, address, x, y, palette, out);
            address = (address + 0x800) & 0xFFFF;
        }
    }

    if (state.show_grid)
        draw_grid(out, info.tile_size);
}

/* ---- Sprite Viewer ------------------------------------------------------- */

void snapshot_oam(OAMSnapshot &out)
{
    out.size_select = PPU.OBJSizeSelect & 7;
    out.first_sprite = PPU.FirstSprite & 127;

    const uint8_t *oam = PPU.OAMData;
    for (int i = 0; i < 128; i++)
    {
        const uint8_t d0 = oam[i * 4 + 0];
        const uint8_t d1 = oam[i * 4 + 1];
        const uint8_t d2 = oam[i * 4 + 2];
        const uint8_t d3 = oam[i * 4 + 3];
        const uint8_t high = oam[512 + (i >> 2)];
        const int shift = (i & 3) * 2;

        int x = d0 | (((high >> shift) & 1) << 8);
        if (x & 0x100)
            x |= ~0x1FF; // the 9-bit X is signed

        Sprite &sprite = out.sprites[i];
        sprite.x = (int16_t)x;
        sprite.y = d1;
        sprite.name = (uint16_t)(d2 | ((d3 & 1) << 8));
        sprite.palette = (d3 >> 1) & 7;
        sprite.priority = (d3 >> 4) & 3;
        sprite.hflip = (d3 & 0x40) != 0;
        sprite.vflip = (d3 & 0x80) != 0;
        sprite.large = ((high >> (shift + 1)) & 1) != 0;
    }
}

void sprite_size(int size_select, bool large, int *width, int *height)
{
    static const int sizes[8][4] = {
        {  8,  8, 16, 16 },
        {  8,  8, 32, 32 },
        {  8,  8, 64, 64 },
        { 16, 16, 32, 32 },
        { 16, 16, 64, 64 },
        { 32, 32, 64, 64 },
        { 16, 32, 32, 64 },
        { 16, 32, 32, 32 },
    };
    const int *entry = sizes[size_select & 7];
    *width = large ? entry[2] : entry[0];
    *height = large ? entry[3] : entry[1];
}

/* This list interleaves the eight sprite palettes, so map the entries that
 * are key colours onto the shared list and report the rest as palettes. */
static int sprite_background_key(int background)
{
    if (background == kBackgroundTransparent)
        return kViewerBgTransparent;
    if (background >= kBackgroundMagenta && background <= kBackgroundBlack)
        return kViewerBgMagenta + (background - kBackgroundMagenta);
    return -1;
}

const char *sprite_background_name(int background)
{
    const int key = sprite_background_key(background);
    return key < 0 ? nullptr : viewer_bg_name(key); // null for the palettes
}

Pixel sprite_background_color(int background, const Pixel palette[256])
{
    const int key = sprite_background_key(background);
    if (key >= 0)
        return viewer_bg_color(key, false);
    if (background >= kBackgroundPalette0 && background <= kBackgroundPalette7)
        return palette[128 + (background - kBackgroundPalette0) * 16];
    return viewer_bg_color(kViewerBgTransparent, false);
}

void render_sprite(const OAMSnapshot &oam, int index, Image &out, int background,
                   bool transparent)
{
    if (index < 0 || index >= 128)
    {
        out.resize(0, 0);
        return;
    }

    Pixel palette[256];
    snapshot_palette(palette);

    int width, height;
    sprite_size(oam.size_select, oam.sprites[index].large, &width, &height);
    out.resize(width, height);
    out.fill(transparent ? 0u : sprite_background_color(background, palette));

    draw_sprite(oam.sprites[index], oam.size_select, 0, 0, palette, out.pixels.data(),
                out.width, out.height);
}

void render_sprite_screen(const OAMSnapshot &oam, const bool visible[128], int background,
                          bool outline, bool transparent, Image &out)
{
    Pixel palette[256];
    snapshot_palette(palette);

    out.resize(kSpriteScreenWidth, kSpriteScreenHeight);
    out.fill(transparent && background == kBackgroundTransparent
                 ? 0u
                 : sprite_background_color(background, palette));

    /* Back to front: the priority rotation starts at FirstSprite and runs
     * forward, earlier meaning higher priority, so draw it in reverse. */
    for (int i = 127; i >= 0; i--)
    {
        const int id = (oam.first_sprite + i) & 127;
        if (visible && !visible[id])
            continue;

        const Sprite &sprite = oam.sprites[id];
        int width, height;
        sprite_size(oam.size_select, sprite.large, &width, &height);

        const int x = sprite.x + kSpriteScreenVisibleX;
        draw_sprite(sprite, oam.size_select, x, sprite.y, palette, out.pixels.data(),
                    out.width, out.height);
        /* Objects near the bottom wrap back through the top of the screen. */
        if (sprite.y + height > kSpriteScreenHeight)
            draw_sprite(sprite, oam.size_select, x, sprite.y - kSpriteScreenHeight, palette,
                        out.pixels.data(), out.width, out.height);
    }

    if (outline)
    {
        const Pixel color = 0xFFFFFF00u; // yellow
        const int x0 = kSpriteScreenVisibleX;
        const int x1 = x0 + kSpriteScreenVisibleWidth - 1;
        const int y1 = kSpriteScreenVisibleHeight - 1;
        for (int x = x0; x <= x1; x++)
        {
            out.row(0)[x] = color;
            out.row(y1)[x] = color;
        }
        for (int y = 0; y <= y1; y++)
        {
            out.row(y)[x0] = color;
            out.row(y)[x1] = color;
        }
    }
}

/* ---- Text ---------------------------------------------------------------- */

bool parse_hex(const std::string &text, uint32_t *out)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        i++;
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X'))
        i += 2;
    else if (i < text.size() && text[i] == '$')
        i++;
    if (i >= text.size() || text.size() - i > 8)
        return false;

    uint32_t value = 0;
    for (; i < text.size(); i++)
    {
        const char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F')
            digit = 10 + c - 'A';
        else
            return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

/* ---- Export -------------------------------------------------------------- */

bool write_png(const std::string &path, const Image &image)
{
    if (image.empty())
        return false;

    FILE *file = fopen(path.c_str(), "wb");
    if (!file)
        return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
    {
        fclose(file);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, nullptr);
        fclose(file);
        return false;
    }

    bool ok = false;
    if (!setjmp(png_jmpbuf(png)))
    {
        png_init_io(png, file);
        ok = write_png_common(png, info, image);
    }
    png_destroy_write_struct(&png, &info);
    fclose(file);
    return ok;
}

bool write_png_memory(const Image &image, std::vector<uint8_t> &out)
{
    out.clear();
    if (image.empty())
        return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
        return false;
    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }

    bool ok = false;
    if (!setjmp(png_jmpbuf(png)))
    {
        png_set_write_fn(png, &out, png_write_to_vector, png_flush_nothing);
        ok = write_png_common(png, info, image);
    }
    png_destroy_write_struct(&png, &info);
    return ok;
}

bool write_zip(const std::string &path, const std::vector<ZipBlob> &entries)
{
    zipFile zf = zipOpen(path.c_str(), APPEND_STATUS_CREATE);
    if (!zf)
        return false;

    bool ok = true;
    for (const ZipBlob &entry : entries)
    {
        zip_fileinfo info = {};
        if (zipOpenNewFileInZip(zf, entry.name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr,
                                Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
        {
            ok = false;
            break;
        }
        if (!entry.data.empty() &&
            zipWriteInFileInZip(zf, entry.data.data(), (unsigned int)entry.data.size()) != ZIP_OK)
        {
            zipCloseFileInZip(zf);
            ok = false;
            break;
        }
        if (zipCloseFileInZip(zf) != ZIP_OK)
        {
            ok = false;
            break;
        }
    }
    zipClose(zf, nullptr);
    return ok;
}

} // namespace ppuviewer
