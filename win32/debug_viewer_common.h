#ifndef DEBUG_VIEWER_COMMON_H
#define DEBUG_VIEWER_COMMON_H

#include <windows.h>
#include "port.h"

#define WM_USER_VIEWER_REFRESH  (WM_USER + 0x201)

// Decode one 8x8 SNES tile from VRAM at byte offset vramByteAddr into
// a 64-byte palette-index array laid out row-major (y*8 + x).
// bpp must be 2, 4, or 8. Address is wrapped to 16-bit (VRAM is 64KB).
void DecodeTile8x8(uint32 vramByteAddr, int bpp, uint8 out[64]);

// Decode one 8x8 tile from an already-collected byte buffer.
// buffer must hold at least bytesPerTile(bpp) bytes (16/32/64 for 2/4/8bpp).
void DecodeTileBytes8x8(const uint8 *tileBytes, int bpp, uint8 out[64]);

// Tile data source. Mirrors bsnes-plus TileRenderer::Source.
enum TileSource {
    TILE_SRC_VRAM = 0,
    TILE_SRC_CPU_BUS,
    TILE_SRC_CART_ROM,
    TILE_SRC_CART_RAM,
    TILE_SRC_SA1_BUS,
    TILE_SRC_SFX_BUS,
    TILE_SRC_COUNT
};

// Read one byte from a tile source without side effects.
// Returns 0 when the source is unavailable (e.g. SA1 bus on a non-SA1 ROM).
uint8 ReadTileSourceByte(int source, uint32 addr);

// Largest valid byte address for the given source (inclusive upper bound + 1).
uint32 TileSourceSize(int source);

// True when this source currently has usable data (ROM loaded, SA1/SFX chip
// present, etc.).
bool TileSourceAvailable(int source);

// Decode one 8x8 Mode 7 tile: tile bytes live at even VRAM bytes only,
// character base is implicit (tileIndex * 128 covers 8 rows * 8 pixels * 2 bytes).
void DecodeMode7Tile8x8(uint32 tileIndex, uint8 out[64]);

// Snapshot current CGRAM as top-down BGRA8 (0xAARRGGBB-compatible for GDI DIB).
// No brightness/fade applied so viewer shows true palette.
void SnapshotPaletteBGRA(uint32 outBGRA[256]);

// Composite an 8x8 decoded tile into a BGRA canvas at (dstX, dstY) on a
// row-stride 'dstStridePixels'. paletteOffset is added to each tile index
// before indexing palBGRA; index 0 is drawn transparent only when drawTransparentAsZero
// is false — otherwise index 0 is treated as a true zero color.
// If tileIndexMask is non-zero it's AND'd with the final palette index (e.g. 0x0F for 4bpp).
void BlitTile8x8BGRA(const uint8 tile[64], const uint32 palBGRA[256],
                     int paletteOffset, int tileIndexMask,
                     uint32 *dstBGRA, int dstStridePixels,
                     int dstX, int dstY, bool hflip, bool vflip);

// Create a 32bpp top-down DIB section. On return *outBits points to BGRA pixel rows
// (4 bytes per pixel, width*4 bytes per row, no padding since 4-byte aligned already).
HBITMAP CreateBGRADib(int width, int height, uint32 **outBits);

// Viewer registry: per-frame pump from the main emulation loop posts
// WM_USER_VIEWER_REFRESH to every registered dialog whose auto-update is on.
struct ViewerEntry {
    HWND hDlg;
    bool *autoUpdateFlag; // pointer into dialog state
};
void DebugViewers_Register(HWND hDlg, bool *autoUpdateFlag);
void DebugViewers_Unregister(HWND hDlg);
void DebugViewers_OnFrame();

// Utility: parse a hex string like "0x1A00" or "1a00" into a uint32.
// Returns false on malformed input.
bool ParseHex(const TCHAR *text, uint32 *out);

// Subclass the given static control so left-click-and-drag updates the
// caller's viewX/viewY offsets (in source pixels). `scale` is read each
// mouse move to scale the mouse delta. `maxX`/`maxY` set the clamp upper
// bounds (exclusive). All four pointers must outlive the control.
void InstallDragPan(HWND canvas,
                    int *viewX, int *viewY,
                    int *scale, int *maxX, int *maxY);
void UninstallDragPan(HWND canvas);

#endif // DEBUG_VIEWER_COMMON_H
