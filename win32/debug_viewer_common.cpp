#include "debug_viewer_common.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"
#include "../sa1.h"

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

void DecodeTileBytes8x8(const uint8 *tileBytes, int bpp, uint8 out[64]) {
    for (int y = 0; y < 8; ++y) {
        uint8 row0[8] = {0};
        Decode2bppRow(&tileBytes[y * 2], row0);

        if (bpp == 2) {
            for (int x = 0; x < 8; ++x) out[y * 8 + x] = row0[x];
            continue;
        }

        uint8 row23[8] = {0};
        Decode2bppRow(&tileBytes[16 + y * 2], row23);

        if (bpp == 4) {
            for (int x = 0; x < 8; ++x)
                out[y * 8 + x] = row0[x] | (row23[x] << 2);
            continue;
        }

        uint8 row45[8] = {0}, row67[8] = {0};
        Decode2bppRow(&tileBytes[32 + y * 2], row45);
        Decode2bppRow(&tileBytes[48 + y * 2], row67);
        for (int x = 0; x < 8; ++x) {
            out[y * 8 + x] = row0[x] | (row23[x] << 2) |
                             (row45[x] << 4) | (row67[x] << 6);
        }
    }
}

void DecodeTile8x8(uint32 vramByteAddr, int bpp, uint8 out[64]) {
    const uint8 *vram = Memory.VRAM;
    uint8 buf[64];
    int bytes = (bpp == 2) ? 16 : (bpp == 4) ? 32 : 64;
    for (int i = 0; i < bytes; ++i)
        buf[i] = vram[(vramByteAddr + i) & 0xFFFF];
    DecodeTileBytes8x8(buf, bpp, out);
}

// Walk a CPU-style memory map (Memory.Map or SA1.Map) to read a single byte
// without side effects. Ported from the private S9xDebugGetByte in debug.cpp.
static uint8 ReadViaMap(uint8 * const *map, uint32 addr) {
    addr &= 0xFFFFFF;
    int block = addr >> MEMMAP_SHIFT;
    uint8 *p = map[block];
    if (p >= (uint8 *)CMemory::MAP_LAST) {
        return p[addr & 0xFFFF];
    }
    switch ((pint)p) {
    case CMemory::MAP_LOROM_SRAM:
    case CMemory::MAP_SA1RAM:
        if (!Memory.SRAMMask) return 0;
        return Memory.SRAM[((((addr & 0xff0000) >> 1) | (addr & 0x7fff)) & Memory.SRAMMask)];
    case CMemory::MAP_HIROM_SRAM:
    case CMemory::MAP_RONLY_SRAM:
        if (!Memory.SRAMMask) return 0;
        return Memory.SRAM[((((addr & 0x7fff) - 0x6000 + ((addr & 0xf0000) >> 3))) & Memory.SRAMMask)];
    case CMemory::MAP_BWRAM:
        return Memory.BWRAM[(addr & 0x7fff) - 0x6000];
    default:
        return 0;
    }
}

uint8 ReadTileSourceByte(int source, uint32 addr) {
    switch (source) {
    case TILE_SRC_VRAM:
        return Memory.VRAM[addr & 0xFFFF];

    case TILE_SRC_CPU_BUS:
        return ReadViaMap(Memory.Map, addr);

    case TILE_SRC_CART_ROM: {
        if (Memory.CalculatedSize == 0) return 0;
        return Memory.ROM[addr % Memory.CalculatedSize];
    }

    case TILE_SRC_CART_RAM:
        if (!Memory.SRAMMask) return 0;
        return Memory.SRAM[addr & Memory.SRAMMask];

    case TILE_SRC_SA1_BUS:
        if (!Settings.SA1) return 0;
        return ReadViaMap(SA1.Map, addr);

    case TILE_SRC_SFX_BUS:
        // SuperFX shares the CPU memory map; use it as a best-effort source.
        if (!Settings.SuperFX) return 0;
        return ReadViaMap(Memory.Map, addr);
    }
    return 0;
}

uint32 TileSourceSize(int source) {
    switch (source) {
    case TILE_SRC_VRAM:     return 0x10000;
    case TILE_SRC_CPU_BUS:  return 0x1000000;
    case TILE_SRC_CART_ROM: return Memory.CalculatedSize;
    case TILE_SRC_CART_RAM: return Memory.SRAMMask ? (Memory.SRAMMask + 1u) : 0;
    case TILE_SRC_SA1_BUS:  return Settings.SA1 ? 0x1000000u : 0;
    case TILE_SRC_SFX_BUS:  return Settings.SuperFX ? 0x1000000u : 0;
    }
    return 0;
}

bool TileSourceAvailable(int source) {
    switch (source) {
    case TILE_SRC_VRAM:     return true;
    case TILE_SRC_CPU_BUS:  return Memory.CalculatedSize > 0;
    case TILE_SRC_CART_ROM: return Memory.CalculatedSize > 0;
    case TILE_SRC_CART_RAM: return Memory.SRAMMask > 0;
    case TILE_SRC_SA1_BUS:  return Settings.SA1 != 0;
    case TILE_SRC_SFX_BUS:  return Settings.SuperFX != 0;
    }
    return false;
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
    // Index 0 is transparent — leave the destination pixel untouched so the
    // caller's pre-fill (usually palette[0], the SNES backdrop) shows through.
    // This matches bsnes-plus BaseRenderer::draw8pxTile behaviour.
    for (int y = 0; y < 8; ++y) {
        int srcY = vflip ? (7 - y) : y;
        uint32 *dstRow = dstBGRA + (dstY + y) * dstStridePixels + dstX;
        for (int x = 0; x < 8; ++x) {
            int srcX = hflip ? (7 - x) : x;
            int idx = tile[srcY * 8 + srcX];
            if (tileIndexMask) idx &= tileIndexMask;
            if (idx == 0) continue;
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

namespace {

struct DragPanState {
    bool dragging;
    int  dragStartX, dragStartY;
    int  dragViewX,  dragViewY;
    int *viewX;
    int *viewY;
    int *scale;
    int *maxX;
    int *maxY;
    WNDPROC origProc;
};

LRESULT CALLBACK DragPanWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DragPanState *ps = (DragPanState *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!ps) return DefWindowProc(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
        ps->dragging = true;
        ps->dragStartX = (short)LOWORD(lParam);
        ps->dragStartY = (short)HIWORD(lParam);
        ps->dragViewX  = *ps->viewX;
        ps->dragViewY  = *ps->viewY;
        SetCapture(hWnd);
        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        return 0;

    case WM_MOUSEMOVE:
        if (ps->dragging) {
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            int dx = mx - ps->dragStartX;
            int dy = my - ps->dragStartY;
            int sc = (ps->scale && *ps->scale > 0) ? *ps->scale : 1;
            int nx = ps->dragViewX - dx / sc;
            int ny = ps->dragViewY - dy / sc;

            // Clamp so you can't pan past the end of the source: the lower
            // right edge should align with the lower right of the canvas.
            RECT rc; GetClientRect(hWnd, &rc);
            int visW = (rc.right / sc);
            int visH = (rc.bottom / sc);
            int maxXv = (ps->maxX && *ps->maxX > visW) ? (*ps->maxX - visW) : 0;
            int maxYv = (ps->maxY && *ps->maxY > visH) ? (*ps->maxY - visH) : 0;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > maxXv) nx = maxXv;
            if (ny > maxYv) ny = maxYv;
            *ps->viewX = nx;
            *ps->viewY = ny;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (ps->dragging) {
            ps->dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_SETCURSOR:
        if (ps->dragging) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            return TRUE;
        }
        break;
    }
    return CallWindowProc(ps->origProc, hWnd, msg, wParam, lParam);
}

} // anonymous namespace

void InstallDragPan(HWND canvas, int *viewX, int *viewY,
                    int *scale, int *maxX, int *maxY) {
    DragPanState *ps = new DragPanState();
    ps->dragging = false;
    ps->dragStartX = ps->dragStartY = 0;
    ps->dragViewX = ps->dragViewY = 0;
    ps->viewX = viewX;
    ps->viewY = viewY;
    ps->scale = scale;
    ps->maxX = maxX;
    ps->maxY = maxY;
    ps->origProc = (WNDPROC)GetWindowLongPtr(canvas, GWLP_WNDPROC);
    SetWindowLongPtr(canvas, GWLP_USERDATA, (LONG_PTR)ps);
    SetWindowLongPtr(canvas, GWLP_WNDPROC, (LONG_PTR)DragPanWndProc);
}

void UninstallDragPan(HWND canvas) {
    DragPanState *ps = (DragPanState *)GetWindowLongPtr(canvas, GWLP_USERDATA);
    if (!ps) return;
    SetWindowLongPtr(canvas, GWLP_WNDPROC, (LONG_PTR)ps->origProc);
    SetWindowLongPtr(canvas, GWLP_USERDATA, 0);
    delete ps;
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
