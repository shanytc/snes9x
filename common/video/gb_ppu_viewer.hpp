/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "ppu_viewer.hpp"

/* Everything the Emulation > GB-PPU viewers need that is not a window, shared
 * by the Qt and GTK ports: the Game Boy / Color / Super Game Boy tile, tilemap
 * and sprite renderers, reading the GB core through the S9xSGB accessors.
 * Mirrors win32's CGBTileViewerDlg, CGBTilemapViewerDlg, CGBSpriteViewerDlg
 * and gb_viewer_common.h. Images are ppuviewer::Image, so the S-PPU viewers'
 * canvas widgets and PNG export serve these too. */
namespace gbviewer
{

using ppuviewer::Image;
using ppuviewer::Pixel;

/* Which colours the Tile Viewer draws tiles through. Auto is CGB BG palette
 * N on a colour cart and the DMG BGP register otherwise. */
enum PaletteMode
{
    kPalAuto = 0,
    kPalBGP,
    kPalOBP0,
    kPalOBP1,
    kPalCgbBG,
    kPalCgbOBJ,
    kPalSGB,
    kPaletteModeCount
};

const char *palette_mode_name(int mode);

/* ---- Tile Viewer: the 384 tiles at 0x8000-0x97FF of one VRAM bank -------- */

constexpr int kTileCount = 384;

struct TileViewerState
{
    int width_tiles = 16; // 8..32
    int bank = 0;         // VRAM bank, 1 only means something on CGB
    int palette_mode = kPalAuto;
    int palette_index = 0; // 0..7
    bool show_grid = false;
};

void render_tiles(const TileViewerState &state, Image &out);
int tile_at_pixel(const TileViewerState &state, int x, int y); // -1 off the tiles
uint16_t tile_address(int tile); // its address in the GB memory map

/* ---- Tilemap Viewer: one 32x32 map, 256x256 pixels ----------------------- */

enum { kMapBG = 0, kMapWindow, kMapCount };
enum { kTileDataAuto = 0, kTileData8000, kTileData8800, kTileDataCount };

const char *map_name(int map);
const char *tile_data_name(int tile_data);

/* The "Background:" list: palette colour 0, then the shared key colours, so
 * entry N > 0 is ppuviewer::ViewerBgColor N - 1. */
enum { kMapBackgroundPalette = 0, kMapBackgroundCount = 1 + ppuviewer::kViewerBgCount };
const char *map_background_name(int background);

struct TilemapViewerState
{
    int map = kMapBG;
    int tile_data = kTileDataAuto; // Auto follows LCDC bit 4
    int background = kMapBackgroundPalette;
    bool show_grid = false;
    bool show_viewport = true; // the 160x144 screen the map is scrolled to
};

void render_tilemap(const TilemapViewerState &state, Image &out, bool for_export = false);

struct TilemapCell
{
    int x = 0, y = 0;
    uint16_t map_address = 0; // in the GB memory map
    uint8_t tile = 0;
    uint8_t attr = 0; // CGB only
    bool cgb = false;
};

TilemapCell tilemap_cell(const TilemapViewerState &state, int x, int y); // source pixel

/* ---- Sprite Viewer: all 40 objects in OAM coordinate space --------------- */

constexpr int kSpriteCount = 40;

struct SpriteViewerState
{
    int selected = 0; // 0..39, outlined in yellow
    int background = ppuviewer::kViewerBgTransparent;
    bool show_viewport = true; // the visible screen, at OAM +8,+16
};

void render_sprites(const SpriteViewerState &state, Image &out, bool for_export = false);

struct SpriteInfo
{
    int x = 0, y = 0; // OAM coordinates; the screen position is x-8, y-16
    int tile = 0;
    int flags = 0;
    bool cgb = false;
    bool valid = false; // false with no GB core to read
};

SpriteInfo sprite_info(int index);

} // namespace gbviewer
