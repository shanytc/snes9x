#ifndef DEBUG_VIEWER_COMMON_H
#define DEBUG_VIEWER_COMMON_H

#include <windows.h>
#include "port.h"

#define WM_USER_VIEWER_REFRESH  (WM_USER + 0x201)

// Decode one 8x8 SNES tile from VRAM at byte offset vramByteAddr into
// a 64-byte palette-index array laid out row-major (y*8 + x).
// bpp must be 2, 4, or 8. Address is wrapped to 16-bit (VRAM is 64KB).
void DecodeTile8x8(uint32 vramByteAddr, int bpp, uint8 out[64]);

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

#endif // DEBUG_VIEWER_COMMON_H
