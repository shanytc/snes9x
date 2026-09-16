/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include <cstdint>
#include <string>
#include <vector>

/* Everything the Emulation > S-PPU viewers need that is not a window, shared
 * by the Qt and GTK ports: the VRAM / tilemap / OAM readers, the renderers
 * that turn them into pixels, and the PNG / ZIP export. The windows only draw
 * what this hands them and route clicks back. win32 keeps its own copy of
 * this logic in debug_viewer_common.cpp and the three viewer dialogs, which
 * this mirrors; the algorithms come from bsnes-plus, as they do there. */
namespace ppuviewer
{

/* 0xAARRGGBB, which is both QImage::Format_ARGB32 and Cairo's ARGB32 on the
 * little-endian hosts these ports build for. Opaque unless said otherwise. */
using Pixel = uint32_t;

struct Image
{
    std::vector<Pixel> pixels;
    int width = 0;
    int height = 0;

    void resize(int w, int h);
    void fill(Pixel color);
    Pixel *row(int y) { return pixels.data() + (size_t)y * width; }
    const Pixel *row(int y) const { return pixels.data() + (size_t)y * width; }
    bool empty() const { return width <= 0 || height <= 0; }
};

/* ---- Tile data sources ---------------------------------------------------
 * The Tile Viewer can decode from anywhere the SNES could have fetched the
 * data, not just the copy that reached VRAM. Mirrors bsnes-plus
 * TileRenderer::Source. */
enum TileSource
{
    kSourceVRAM = 0,
    kSourceCPUBus,
    kSourceCartROM,
    kSourceCartRAM,
    kSourceSA1Bus,
    kSourceSFXBus,
    kSourceCount
};

const char *source_name(int source);
/* Reads one byte without side effects; 0 when the source is unavailable
 * (an SA-1 bus on a cartridge with no SA-1, say). */
uint8_t read_source_byte(int source, uint32_t address);
uint32_t source_size(int source);  // exclusive upper bound, 0 when absent
bool source_available(int source); // has usable data right now

/* ---- Decoding ------------------------------------------------------------ */

/* One 8x8 tile into a 64-byte row-major palette-index array. bpp is 2, 4
 * or 8; VRAM addresses wrap to 16 bits. */
void decode_tile_bytes(const uint8_t *tile_bytes, int bpp, uint8_t out[64]);
void decode_tile(uint32_t vram_byte_address, int bpp, uint8_t out[64]);

/* CGRAM as it stands, with no brightness or fade applied, so the viewers
 * show the true palette. */
void snapshot_palette(Pixel out[256]);

/* ---- Tile Viewer --------------------------------------------------------- */

enum BitDepth
{
    kBpp2 = 0,
    kBpp4,
    kBpp8,
    kMode7,
    kMode7ExtBG,
    kBitDepthCount
};

enum TileLayout
{
    kLayoutNormal = 0,  // tiles in VRAM order
    kLayout8x16,        // the two halves of an 8x16 object on one line
    kLayout16x16,       // the four quarters of a 16x16 object on one line
    kTileLayoutCount
};

const char *bit_depth_name(int depth);
const char *tile_layout_name(int layout);

struct TileViewerState
{
    int source = kSourceVRAM;
    int bit_depth = kBpp4;
    int layout = kLayoutNormal;
    uint32_t address = 0; // byte offset into the selected source
    int width_tiles = 16; // tiles per row, 8..64
    bool show_grid = false;
    bool use_cgram = true; // false draws a grayscale ramp instead
    int palette_offset = 0; // 0..255, the CGRAM index a tile's colour 0 maps to
};

/* Tallest canvas render_tiles will produce, in pixels; a 24-bit source at
 * 2bpp would otherwise ask for half a gigabyte of tiles. */
constexpr int kTileCanvasMaxHeight = 2048;

void render_tiles(const TileViewerState &state, Image &out);

int tile_bytes(int bit_depth);      // 16 / 32 / 64, or 128 for Mode 7
int tile_colors(int bit_depth);     // 4 / 16 / 256
bool is_mode7(int bit_depth);
uint32_t align_address(int bit_depth, uint32_t address); // to a tile boundary
uint32_t address_mask(int source);  // 0xFFFF for VRAM, 0xFFFFFF elsewhere

/* Tile under a canvas pixel, or -1 where the layout leaves a gap. */
int tile_at_pixel(const TileViewerState &state, int x, int y);
/* Where that tile's data lives, for the readout under the canvas. */
uint32_t tile_address(const TileViewerState &state, int tile_index);

/* The address the < and > buttons step to: one canvas row of tiles. */
void step_address(TileViewerState &state, bool forward);

/* ---- Base tile addresses -------------------------------------------------
 * The PPU's own character bases, for the readout and its goto buttons:
 * BG1-BG4 then the two object name tables. */
enum { kBaseAddressCount = 6 };
void base_addresses(uint32_t out[kBaseAddressCount]);
/* The bit depth `which` is drawn at in the live BG mode; objects are always
 * 4bpp. Invalid layer/mode pairs fall back to 4bpp, as win32 does. */
int base_address_bit_depth(int which);

/* ---- Viewer background colours --------------------------------------------
 * The key fill colours a viewer's "Background:" list offers after its own
 * leading entries, kept in one list so the names and the colours cannot drift
 * apart, as win32's debug_viewer_common does. Transparent is dark gray on the
 * canvas, for contrast, and alpha 0 in an exported PNG. */
enum ViewerBgColor
{
    kViewerBgTransparent = 0,
    kViewerBgMagenta,
    kViewerBgCyan,
    kViewerBgWhite,
    kViewerBgBlack,
    kViewerBgCount
};

const char *viewer_bg_name(int color);
Pixel viewer_bg_color(int color, bool for_export);

/* ---- Tilemap Viewer ------------------------------------------------------ */

enum TilemapBitDepth
{
    kTilemapBpp2 = 0,
    kTilemapBpp4,
    kTilemapBpp8,
    kTilemapMode7,
    kTilemapBitDepthCount
};

/* The Tilemap Viewer's "Background:" list: the backdrop it has always drawn
 * first, then the shared key colours, so entry N > 0 is ViewerBgColor N - 1.
 * Mirrors win32's TMVState::bgType. */
enum { kTilemapBackdrop = 0, kTilemapBackgroundCount = 1 + kViewerBgCount };

const char *tilemap_background_name(int background);
Pixel tilemap_background_color(int background, const Pixel palette[256], bool for_export);

struct TilemapViewerState
{
    int bg = 0; // 0..3
    bool show_grid = false;
    int background = kTilemapBackdrop; // the fill the map is drawn over

    /* Draw a layer as some other screen mode would, rather than the one the
     * game has set. */
    bool custom_screen_mode = false;
    int custom_mode = 1; // 0..7

    /* Ignore the PPU's registers for this layer and use these instead. */
    bool override_tilemap = false;
    int override_bit_depth = kTilemapBpp4;
    int override_map_size = 0;  // 0=32x32 1=64x32 2=32x64 3=64x64
    int override_tile_size = 0; // 0=8x8, 1=16x16
    uint32_t override_map_address = 0;
    uint32_t override_tile_address = 0;
};

struct TilemapInfo
{
    int mode = 0;
    int bg = 0;
    int bpp = 0; // 0 when this layer does not exist in this mode
    int tile_size = 8;
    int width_tiles = 32;
    int height_tiles = 32;
    uint32_t map_address = 0;
    uint32_t tile_address = 0;
    bool mode7 = false;
    bool valid = false; // false when bpp == 0
};

TilemapInfo resolve_tilemap(const TilemapViewerState &state);
void render_tilemap(const TilemapViewerState &state, Image &out, TilemapInfo *info,
                    bool for_export = false);
int tilemap_bit_depth_index(int bpp); // 2/4/8 -> kTilemapBpp2/4/8

/* ---- Sprite Viewer ------------------------------------------------------- */

/* One OAM entry, parsed from raw PPU.OAMData so that what gets listed and
 * what gets drawn come from a single sample of PPU state. */
struct Sprite
{
    int16_t x = 0;  // 9-bit signed, -256..255
    uint8_t y = 0;
    uint16_t name = 0; // 9-bit character number
    uint8_t palette = 0;
    uint8_t priority = 0;
    bool hflip = false;
    bool vflip = false;
    bool large = false;
};

struct OAMSnapshot
{
    Sprite sprites[128];
    uint8_t size_select = 0;  // OBJSizeSelect, 0..7
    uint8_t first_sprite = 0; // the priority rotation's starting object
};

void snapshot_oam(OAMSnapshot &out);
void sprite_size(int size_select, bool large, int *width, int *height);

/* The Screen view draws all 128 objects where they sit; sprite X runs
 * -256..255, so the canvas is twice the SNES screen and the visible area
 * starts at x = 256. */
constexpr int kSpriteScreenWidth = 512;
constexpr int kSpriteScreenHeight = 256;
constexpr int kSpriteScreenVisibleX = 256;
constexpr int kSpriteScreenVisibleWidth = 256;
constexpr int kSpriteScreenVisibleHeight = 224;

enum SpriteBackground
{
    kBackgroundTransparent = 0,
    kBackgroundPalette0,
    kBackgroundPalette7 = kBackgroundPalette0 + 7,
    kBackgroundMagenta,
    kBackgroundCyan,
    kBackgroundWhite,
    kBackgroundBlack,
    kSpriteBackgroundCount
};

const char *sprite_background_name(int background); // nullptr for the palettes
Pixel sprite_background_color(int background, const Pixel palette[256]);

/* The selected object on its own, at 1:1. `transparent` leaves the pixels
 * the object does not cover at alpha 0, which is what the PNG export wants;
 * otherwise they get the chosen background colour. */
void render_sprite(const OAMSnapshot &oam, int index, Image &out,
                   int background, bool transparent);

/* All enabled objects where they sit, back to front in the PPU's priority
 * rotation. `outline` draws the visible screen's border in yellow; the PNG
 * export leaves it off. */
void render_sprite_screen(const OAMSnapshot &oam, const bool visible[128],
                          int background, bool outline, bool transparent,
                          Image &out);

/* ---- Text ---------------------------------------------------------------- */

/* Parses "0x1A00", "$1a00" or "1A00" into a value. False on anything else,
 * which is how the address fields reject a half-typed entry. */
bool parse_hex(const std::string &text, uint32_t *out);

/* ---- Export -------------------------------------------------------------- */

struct ZipBlob
{
    std::string name; // the name inside the archive
    std::vector<uint8_t> data;
};

bool write_png(const std::string &path, const Image &image);
bool write_png_memory(const Image &image, std::vector<uint8_t> &out);
bool write_zip(const std::string &path, const std::vector<ZipBlob> &entries);

} // namespace ppuviewer
