#include "debug_viewer_common.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"

#include <tchar.h>
#include <vector>

namespace {

std::vector<ViewerEntry> g_viewers;

// SNES 2bpp planar: each 8x8 tile is 16 bytes. Byte pair (p0,p1) encodes 8 pixels:
//   pixel[x] bit0 = (p0 >> (7-x)) & 1, bit1 = (p1 >> (7-x)) & 1
// 4bpp: first 16 bytes are planes 0/1 (like 2bpp), next 16 bytes are planes 2/3.
// 8bpp: 64 bytes, plane pairs interleaved every 16 bytes (planes 0/1, 2/3, 4/5, 6/7).
void Decode2bppRow(const uint8 *p0p1, uint8 out[8]) {
    uint8 b0 = p0p1[0], b1 = p0p1[1];
    for (int x = 0; x < 8; ++x) {
        int bit = 7 - x;
        uint8 c = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
        out[x] = c;
    }
}

} // anonymous namespace

void DecodeTile8x8(uint32 vramByteAddr, int bpp, uint8 out[64]) {
    vramByteAddr &= 0xFFFF;
    const uint8 *vram = Memory.VRAM;

    for (int y = 0; y < 8; ++y) {
        uint8 row0[8] = {0};
        Decode2bppRow(&vram[(vramByteAddr + y * 2) & 0xFFFF], row0);

        if (bpp == 2) {
            for (int x = 0; x < 8; ++x) out[y * 8 + x] = row0[x];
            continue;
        }

        // 4bpp: plane 2+3 live 16 bytes further in.
        uint8 row23[8] = {0};
        Decode2bppRow(&vram[(vramByteAddr + 16 + y * 2) & 0xFFFF], row23);

        if (bpp == 4) {
            for (int x = 0; x < 8; ++x)
                out[y * 8 + x] = row0[x] | (row23[x] << 2);
            continue;
        }

        // 8bpp: additional 4 planes at +32 and +48 bytes.
        uint8 row45[8] = {0}, row67[8] = {0};
        Decode2bppRow(&vram[(vramByteAddr + 32 + y * 2) & 0xFFFF], row45);
        Decode2bppRow(&vram[(vramByteAddr + 48 + y * 2) & 0xFFFF], row67);

        for (int x = 0; x < 8; ++x) {
            out[y * 8 + x] = row0[x] | (row23[x] << 2) |
                             (row45[x] << 4) | (row67[x] << 6);
        }
    }
}

void DecodeMode7Tile8x8(uint32 tileIndex, uint8 out[64]) {
    // Mode 7 packs char data in even bytes, screen data in odd. Each tile
    // is 64 bytes of char data (8 rows * 8 bytes at even offsets).
    // Tile N lives at VRAM byte offset (N * 128) with pixels at (offset + pixel*2).
    const uint8 *vram = Memory.VRAM;
    uint32 base = (tileIndex * 128) & 0xFFFF;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            out[y * 8 + x] = vram[(base + y * 16 + x * 2) & 0xFFFF];
        }
    }
}

void SnapshotPaletteBGRA(uint32 outBGRA[256]) {
    for (int i = 0; i < 256; ++i) {
        uint16 bgr = PPU.CGDATA[i];
        uint32 r = (bgr & 0x001F);
        uint32 g = (bgr & 0x03E0) >> 5;
        uint32 b = (bgr & 0x7C00) >> 10;
        // 5-bit to 8-bit with the low-3-bit replicate trick.
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        outBGRA[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

void BlitTile8x8BGRA(const uint8 tile[64], const uint32 palBGRA[256],
                     int paletteOffset, int tileIndexMask,
                     uint32 *dstBGRA, int dstStridePixels,
                     int dstX, int dstY, bool hflip, bool vflip) {
    for (int y = 0; y < 8; ++y) {
        int srcY = vflip ? (7 - y) : y;
        uint32 *dstRow = dstBGRA + (dstY + y) * dstStridePixels + dstX;
        for (int x = 0; x < 8; ++x) {
            int srcX = hflip ? (7 - x) : x;
            int idx = tile[srcY * 8 + srcX];
            if (tileIndexMask) idx &= tileIndexMask;
            int palIdx = (paletteOffset + idx) & 0xFF;
            dstRow[x] = palBGRA[palIdx];
        }
    }
}

HBITMAP CreateBGRADib(int width, int height, uint32 **outBits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (outBits) *outBits = (uint32 *)bits;
    return hbm;
}

void DebugViewers_Register(HWND hDlg, bool *autoUpdateFlag) {
    ViewerEntry e{ hDlg, autoUpdateFlag };
    g_viewers.push_back(e);
}

void DebugViewers_Unregister(HWND hDlg) {
    for (auto it = g_viewers.begin(); it != g_viewers.end(); ++it) {
        if (it->hDlg == hDlg) {
            g_viewers.erase(it);
            return;
        }
    }
}

void DebugViewers_OnFrame() {
    for (const auto &e : g_viewers) {
        if (e.autoUpdateFlag && *e.autoUpdateFlag && IsWindow(e.hDlg)) {
            PostMessage(e.hDlg, WM_USER_VIEWER_REFRESH, 0, 0);
        }
    }
}

bool ParseHex(const TCHAR *text, uint32 *out) {
    while (*text == _T(' ') || *text == _T('\t')) ++text;
    if (text[0] == _T('0') && (text[1] == _T('x') || text[1] == _T('X')))
        text += 2;
    else if (text[0] == _T('$'))
        text += 1;
    if (!*text) return false;
    uint32 v = 0;
    while (*text) {
        TCHAR c = *text++;
        uint32 d;
        if (c >= _T('0') && c <= _T('9')) d = c - _T('0');
        else if (c >= _T('a') && c <= _T('f')) d = 10 + c - _T('a');
        else if (c >= _T('A') && c <= _T('F')) d = 10 + c - _T('A');
        else return false;
        v = (v << 4) | d;
        if (v > 0xFFFFFFFFu) return false;
    }
    *out = v;
    return true;
}
