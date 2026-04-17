// VRAM tile viewer — algorithm mirrors bsnes-plus
// (bsnes/ui-qt/debugger/ppu/tile-renderer.cpp + base-renderer.cpp).

#include <windows.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>

#include "CVRAMViewerDlg.h"
#include "debug_viewer_common.h"
#include "wsnes9x.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"

HWND gVRAMViewerHWND = NULL;
extern HINSTANCE g_hInst;

namespace {

constexpr int kSrcMax = 2048;  // large enough for 64-wide * 256 rows * 8 px
constexpr int kPalSrcSize = 128;

enum BitDepth { BD_2BPP = 0, BD_4BPP, BD_8BPP, BD_MODE7, BD_MODE7_EXTBG };

struct VRAMState {
    int  bitDepth;
    uint32 address;          // VRAM byte offset top-left of tile grid
    int  widthTiles;         // tiles per row (8..64)
    int  zoom;               // 1..9
    bool showGrid;
    bool useCGRAM;
    bool autoUpdate;
    int  paletteOffset;      // 0..255, index into CGRAM for colors-per-tile block

    int  selectedTile;       // -1 if none

    HBITMAP tileBmp;
    uint32 *tileBits;
    HBITMAP palBmp;
    uint32 *palBits;
    int  curSrcW, curSrcH;
};

VRAMState *GetState(HWND hDlg) {
    return (VRAMState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

int BytesPerTile(int bd) {
    switch (bd) {
    case BD_2BPP: return 16;
    case BD_4BPP: return 32;
    case BD_8BPP: return 64;
    case BD_MODE7:
    case BD_MODE7_EXTBG: return 128;
    }
    return 32;
}

int ColorsPerTile(int bd) {
    switch (bd) {
    case BD_2BPP: return 4;
    case BD_4BPP: return 16;
    case BD_8BPP:
    case BD_MODE7:
    case BD_MODE7_EXTBG: return 256;
    }
    return 16;
}

bool IsMode7(int bd) { return bd == BD_MODE7 || bd == BD_MODE7_EXTBG; }

uint32 AddressMask(int bd) {
    switch (bd) {
    case BD_8BPP: return 0xFFC0;
    case BD_4BPP: return 0xFFE0;
    case BD_2BPP: return 0xFFF0;
    default: return 0;
    }
}

int MaxTiles(int bd) {
    switch (bd) {
    case BD_2BPP: return 4096;
    case BD_4BPP: return 2048;
    case BD_8BPP: return 1024;
    case BD_MODE7:
    case BD_MODE7_EXTBG: return 256;
    }
    return 1024;
}

// Build palette to index with. For CGRAM mode the real palette is used as-is,
// otherwise a grayscale ramp scaled to the bit depth.
void BuildPaletteBGRA(const VRAMState *st, uint32 pal[256]) {
    if (st->useCGRAM) {
        SnapshotPaletteBGRA(pal);
    } else {
        int n = ColorsPerTile(st->bitDepth);
        if (n < 2) n = 2;
        for (int i = 0; i < 256; ++i) {
            int c = i % n;
            uint32 v = (uint32)(c * 255 / (n - 1));
            pal[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    }
}

void DrawMode7TilePixel(uint32 *dstBGRA, int dstStride, uint32 tileIdx,
                        bool extbg, const uint32 pal[256]) {
    const uint8 *vram = Memory.VRAM;
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            uint8 pix = vram[((tileIdx * 128 + 1) + py * 16 + px * 2) & 0xFFFF];
            if (extbg) pix &= 0x7F;
            if (pix != 0) dstBGRA[py * dstStride + px] = pal[pix];
        }
    }
}

void DrawTileIntoCanvas(VRAMState *st, int tileIdx, const uint32 pal[256]) {
    int tilesX = st->widthTiles;
    int gx = (tileIdx % tilesX) * 8;
    int gy = (tileIdx / tilesX) * 8;
    if (gx + 8 > kSrcMax || gy + 8 > kSrcMax) return;

    uint32 *dst = st->tileBits + gy * kSrcMax + gx;

    if (IsMode7(st->bitDepth)) {
        DrawMode7TilePixel(dst, kSrcMax, tileIdx & 0xFF,
                           st->bitDepth == BD_MODE7_EXTBG, pal);
        return;
    }

    uint32 addr = (st->address + tileIdx * BytesPerTile(st->bitDepth))
                  & AddressMask(st->bitDepth);
    uint8 tile[64];
    DecodeTile8x8(addr, (st->bitDepth == BD_2BPP) ? 2 :
                          (st->bitDepth == BD_4BPP) ? 4 : 8, tile);
    // paletteOffset is clamped to colorsPerTile-aligned start by bsnes, but
    // we use it as a direct CGRAM index so users can scroll anywhere.
    BlitTile8x8BGRA(tile, pal, st->paletteOffset & 0xFF, 0,
                    st->tileBits, kSrcMax, gx, gy, false, false);
}

void RedrawTiles(HWND hDlg) {
    VRAMState *st = GetState(hDlg);
    if (!st || !st->tileBits) return;

    uint32 pal[256];
    BuildPaletteBGRA(st, pal);

    // Calc how many tiles to display and canvas size.
    int maxT = MaxTiles(st->bitDepth);
    if (!IsMode7(st->bitDepth)) {
        int firstTile = st->address / BytesPerTile(st->bitDepth);
        if (firstTile >= maxT) firstTile = 0;
        maxT -= firstTile;
    }
    int tilesX = st->widthTiles;
    if (tilesX < 8) tilesX = 8;
    if (tilesX > 64) tilesX = 64;
    int rows = (maxT + tilesX - 1) / tilesX;

    int pxW = tilesX * 8;
    int pxH = rows * 8;
    if (pxH > kSrcMax) pxH = kSrcMax;
    st->curSrcW = pxW;
    st->curSrcH = pxH;

    // Background = palette[0] so index-0 pixels fall through to the backdrop.
    uint32 bg = pal[0];
    for (int i = 0; i < kSrcMax * kSrcMax; ++i) st->tileBits[i] = bg;

    int total = rows * tilesX;
    if (total > maxT) total = maxT;
    for (int t = 0; t < total; ++t) DrawTileIntoCanvas(st, t, pal);

    if (st->showGrid) {
        uint32 line = 0x80808080u;
        for (int y = 0; y < pxH; y += 8) {
            uint32 *row = st->tileBits + y * kSrcMax;
            for (int x = 0; x < pxW; ++x) row[x] = line;
        }
        for (int y = 0; y < pxH; ++y) {
            uint32 *row = st->tileBits + y * kSrcMax;
            for (int x = 0; x < pxW; x += 8) row[x] = line;
        }
    }

    InvalidateRect(GetDlgItem(hDlg, IDC_VRAMV_CANVAS), NULL, FALSE);
}

void RedrawPalette(HWND hDlg) {
    VRAMState *st = GetState(hDlg);
    if (!st || !st->palBits) return;

    uint32 pal[256];
    SnapshotPaletteBGRA(pal);

    // 16 cols × 16 rows at 8×4.5 px each. Use 8×4 cells (132×72 canvas).
    const int cellW = 8, cellH = 4;
    for (int i = 0; i < 256; ++i) {
        int gx = (i % 16) * cellW;
        int gy = (i / 16) * cellH;
        for (int yy = 0; yy < cellH; ++yy) {
            for (int xx = 0; xx < cellW; ++xx) {
                st->palBits[(gy + yy) * kPalSrcSize + (gx + xx)] = pal[i];
            }
        }
    }

    // Outline the currently selected palette offset cell in bright white.
    int sel = st->paletteOffset & 0xFF;
    int sx = (sel % 16) * cellW;
    int sy = (sel / 16) * cellH;
    for (int xx = 0; xx < cellW; ++xx) {
        st->palBits[sy * kPalSrcSize + sx + xx] = 0xFFFFFFFFu;
        st->palBits[(sy + cellH - 1) * kPalSrcSize + sx + xx] = 0xFFFFFFFFu;
    }
    for (int yy = 0; yy < cellH; ++yy) {
        st->palBits[(sy + yy) * kPalSrcSize + sx] = 0xFFFFFFFFu;
        st->palBits[(sy + yy) * kPalSrcSize + sx + cellW - 1] = 0xFFFFFFFFu;
    }

    InvalidateRect(GetDlgItem(hDlg, IDC_VRAMV_PALETTE), NULL, FALSE);
}

void RefreshBaseAddresses(HWND hDlg) {
    TCHAR buf[16];
    uint32 addrs[6] = {
        (uint32)(PPU.BG[0].NameBase << 1) & 0xFFFF,
        (uint32)(PPU.BG[1].NameBase << 1) & 0xFFFF,
        (uint32)(PPU.BG[2].NameBase << 1) & 0xFFFF,
        (uint32)(PPU.BG[3].NameBase << 1) & 0xFFFF,
        (uint32)PPU.OBJNameBase & 0xFFFF,
        (uint32)(PPU.OBJNameBase + 0x2000 + PPU.OBJNameSelect) & 0xFFFF,
    };
    int ids[6] = {
        IDC_VRAMV_BG1_ADDR, IDC_VRAMV_BG2_ADDR,
        IDC_VRAMV_BG3_ADDR, IDC_VRAMV_BG4_ADDR,
        IDC_VRAMV_OAM1_ADDR, IDC_VRAMV_OAM2_ADDR,
    };
    for (int i = 0; i < 6; ++i) {
        _sntprintf(buf, 16, _T("0x%04X"), addrs[i]);
        SetDlgItemText(hDlg, ids[i], buf);
    }
}

void UpdateAddressEdit(HWND hDlg, VRAMState *st) {
    TCHAR buf[16];
    _sntprintf(buf, 16, _T("0x%04X"), st->address & 0xFFFF);
    SetDlgItemText(hDlg, IDC_VRAMV_ADDRESS, buf);
}

void UpdateTileInfo(HWND hDlg, VRAMState *st) {
    TCHAR buf[128];
    if (st->selectedTile < 0) {
        SetDlgItemText(hDlg, IDC_VRAMV_TILEINFO, _T(""));
        return;
    }
    uint32 tileAddr;
    if (IsMode7(st->bitDepth)) {
        tileAddr = (st->selectedTile & 0xFF) * 128 + 1;
    } else {
        tileAddr = (st->address + st->selectedTile * BytesPerTile(st->bitDepth))
                   & 0xFFFF;
    }
    _sntprintf(buf, 128, _T("Selected tile #%d\nAddress 0x%04X"),
               st->selectedTile, tileAddr);
    SetDlgItemText(hDlg, IDC_VRAMV_TILEINFO, buf);
}

void HandleDrawItem(HWND hDlg, DRAWITEMSTRUCT *dis) {
    VRAMState *st = GetState(hDlg);
    if (!st) return;
    int w = dis->rcItem.right - dis->rcItem.left;
    int h = dis->rcItem.bottom - dis->rcItem.top;

    if (dis->CtlID == IDC_VRAMV_CANVAS && st->tileBmp) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kSrcMax;
        bmi.bmiHeader.biHeight = -kSrcMax;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(BLACK_BRUSH));
        int scale = st->zoom < 1 ? 1 : st->zoom;
        int drawW = st->curSrcW * scale;
        int drawH = st->curSrcH * scale;
        if (drawW > w) drawW = w;
        if (drawH > h) drawH = h;
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchDIBits(dis->hDC, 0, 0, drawW, drawH,
                      0, 0, drawW / scale, drawH / scale,
                      st->tileBits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else if (dis->CtlID == IDC_VRAMV_PALETTE && st->palBmp) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kPalSrcSize;
        bmi.bmiHeader.biHeight = -(kPalSrcSize / 2);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchDIBits(dis->hDC, 0, 0, w, h, 0, 0, kPalSrcSize, kPalSrcSize / 2,
                      st->palBits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

void HandleCanvasClick(HWND hDlg, VRAMState *st) {
    POINT pt; GetCursorPos(&pt);
    HWND hCanvas = GetDlgItem(hDlg, IDC_VRAMV_CANVAS);
    ScreenToClient(hCanvas, &pt);
    RECT rc; GetClientRect(hCanvas, &rc);
    int scale = st->zoom < 1 ? 1 : st->zoom;
    int pxX = pt.x / scale;
    int pxY = pt.y / scale;
    if (pxX < 0 || pxX >= st->curSrcW || pxY < 0 || pxY >= st->curSrcH) return;
    int tileX = pxX / 8;
    int tileY = pxY / 8;
    st->selectedTile = tileY * st->widthTiles + tileX;
    UpdateTileInfo(hDlg, st);
}

void HandlePaletteClick(HWND hDlg, VRAMState *st) {
    POINT pt; GetCursorPos(&pt);
    HWND hPal = GetDlgItem(hDlg, IDC_VRAMV_PALETTE);
    ScreenToClient(hPal, &pt);
    RECT rc; GetClientRect(hPal, &rc);
    int w = rc.right, h = rc.bottom;
    if (pt.x < 0 || pt.x >= w || pt.y < 0 || pt.y >= h) return;
    // Canvas is 16 cols x 16 rows.
    int col = pt.x * 16 / w;
    int row = pt.y * 16 / h;
    if (col < 0) col = 0; if (col > 15) col = 15;
    if (row < 0) row = 0; if (row > 15) row = 15;
    st->paletteOffset = row * 16 + col;
    st->useCGRAM = true;
    CheckDlgButton(hDlg, IDC_VRAMV_USECGRAM, BST_CHECKED);
    RedrawPalette(hDlg);
    RedrawTiles(hDlg);
}

void PopulateZoom(HWND hCombo) {
    for (int i = 1; i <= 9; ++i) {
        TCHAR buf[8];
        _sntprintf(buf, 8, _T("%dx"), i);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(hCombo, CB_SETCURSEL, 2, 0); // default 3x like bsnes
}

void PopulateSource(HWND hCombo) {
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("VRAM"));
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

void PopulateBitDepth(HWND hCombo) {
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("2bpp"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("4bpp"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("8bpp"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Mode 7"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Mode 7 EXTBG"));
    SendMessage(hCombo, CB_SETCURSEL, BD_4BPP, 0);
}

void StepAddress(HWND hDlg, VRAMState *st, bool forward) {
    if (IsMode7(st->bitDepth)) return;
    uint32 step = BytesPerTile(st->bitDepth) * (uint32)st->widthTiles;
    if (forward) {
        st->address = (st->address + step) & AddressMask(st->bitDepth);
    } else {
        st->address = (st->address >= step) ? (st->address - step) : 0;
    }
    UpdateAddressEdit(hDlg, st);
    RedrawTiles(hDlg);
}

INT_PTR CALLBACK DlgVRAMViewer(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        VRAMState *st = new VRAMState();
        st->bitDepth = BD_4BPP;
        st->address = 0;
        st->widthTiles = 16;
        st->zoom = 3;
        st->showGrid = false;
        st->useCGRAM = true;
        st->autoUpdate = true;
        st->paletteOffset = 0;
        st->selectedTile = -1;
        st->tileBmp = CreateBGRADib(kSrcMax, kSrcMax, &st->tileBits);
        st->palBmp  = CreateBGRADib(kPalSrcSize, kPalSrcSize / 2, &st->palBits);
        st->curSrcW = 128;
        st->curSrcH = 128;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);

        PopulateZoom(GetDlgItem(hDlg, IDC_VRAMV_ZOOM));
        PopulateSource(GetDlgItem(hDlg, IDC_VRAMV_SOURCE));
        PopulateBitDepth(GetDlgItem(hDlg, IDC_VRAMV_BITDEPTH));

        CheckDlgButton(hDlg, IDC_VRAMV_USECGRAM, BST_CHECKED);
        CheckDlgButton(hDlg, IDC_VRAMV_AUTOUPDATE, BST_CHECKED);
        SetDlgItemInt(hDlg, IDC_VRAMV_WIDTH, st->widthTiles, FALSE);
        SendDlgItemMessage(hDlg, IDC_VRAMV_WIDTH_SPIN, UDM_SETRANGE, 0, MAKELPARAM(64, 8));
        SendDlgItemMessage(hDlg, IDC_VRAMV_WIDTH_SPIN, UDM_SETPOS, 0, MAKELPARAM(st->widthTiles, 0));

        UpdateAddressEdit(hDlg, st);
        RefreshBaseAddresses(hDlg);

        DebugViewers_Register(hDlg, &st->autoUpdate);
        RedrawPalette(hDlg);
        RedrawTiles(hDlg);
        return TRUE;
    }

    case WM_DRAWITEM:
        HandleDrawItem(hDlg, (DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_USER_VIEWER_REFRESH: {
        RefreshBaseAddresses(hDlg);
        RedrawPalette(hDlg);
        RedrawTiles(hDlg);
        return TRUE;
    }

    case WM_COMMAND: {
        VRAMState *st = GetState(hDlg);
        if (!st) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        switch (id) {
        case IDC_VRAMV_ZOOM:
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessage(hDlg, IDC_VRAMV_ZOOM, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    st->zoom = sel + 1;
                    InvalidateRect(GetDlgItem(hDlg, IDC_VRAMV_CANVAS), NULL, FALSE);
                }
            }
            return TRUE;

        case IDC_VRAMV_SHOWGRID:
            st->showGrid = (IsDlgButtonChecked(hDlg, IDC_VRAMV_SHOWGRID) == BST_CHECKED);
            RedrawTiles(hDlg);
            return TRUE;

        case IDC_VRAMV_AUTOUPDATE:
            st->autoUpdate = (IsDlgButtonChecked(hDlg, IDC_VRAMV_AUTOUPDATE) == BST_CHECKED);
            return TRUE;

        case IDC_VRAMV_REFRESH:
            RefreshBaseAddresses(hDlg);
            RedrawPalette(hDlg);
            RedrawTiles(hDlg);
            return TRUE;

        case IDC_VRAMV_USECGRAM:
            st->useCGRAM = (IsDlgButtonChecked(hDlg, IDC_VRAMV_USECGRAM) == BST_CHECKED);
            RedrawTiles(hDlg);
            return TRUE;

        case IDC_VRAMV_BITDEPTH:
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessage(hDlg, IDC_VRAMV_BITDEPTH, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    st->bitDepth = sel;
                    st->selectedTile = -1;
                    UpdateTileInfo(hDlg, st);
                    RedrawTiles(hDlg);
                }
            }
            return TRUE;

        case IDC_VRAMV_WIDTH:
            if (code == EN_CHANGE) {
                BOOL ok = FALSE;
                int v = (int)GetDlgItemInt(hDlg, IDC_VRAMV_WIDTH, &ok, FALSE);
                if (ok) {
                    if (v < 8) v = 8;
                    if (v > 64) v = 64;
                    if (v != st->widthTiles) {
                        st->widthTiles = v;
                        RedrawTiles(hDlg);
                    }
                }
            }
            return TRUE;

        case IDC_VRAMV_ADDRESS:
            if (code == EN_CHANGE) {
                TCHAR buf[32];
                GetDlgItemText(hDlg, IDC_VRAMV_ADDRESS, buf, 32);
                uint32 v;
                if (ParseHex(buf, &v)) {
                    st->address = v & AddressMask(st->bitDepth);
                    RedrawTiles(hDlg);
                }
            }
            return TRUE;

        case IDC_VRAMV_PREV: StepAddress(hDlg, st, false); return TRUE;
        case IDC_VRAMV_NEXT: StepAddress(hDlg, st, true);  return TRUE;

        case IDC_VRAMV_BG1_GOTO:
        case IDC_VRAMV_BG2_GOTO:
        case IDC_VRAMV_BG3_GOTO:
        case IDC_VRAMV_BG4_GOTO:
        case IDC_VRAMV_OAM1_GOTO:
        case IDC_VRAMV_OAM2_GOTO: {
            int idx = id - IDC_VRAMV_BG1_GOTO;  // 0..5
            uint32 addr = 0;
            if (idx < 4)        addr = (uint32)(PPU.BG[idx].NameBase << 1) & 0xFFFF;
            else if (idx == 4)  addr = (uint32)PPU.OBJNameBase & 0xFFFF;
            else                addr = (uint32)(PPU.OBJNameBase + 0x2000 + PPU.OBJNameSelect) & 0xFFFF;

            // OAM targets are always 4bpp, BGs use their live mode's bpp.
            if (idx >= 4) {
                st->bitDepth = BD_4BPP;
            } else {
                int mode = PPU.BGMode & 7;
                if (mode == 7) {
                    st->bitDepth = BD_4BPP; // can't sensibly map BG1..4 in mode 7 here
                } else {
                    static const int bppT[8][4] = {
                        {2,2,2,2},{4,4,2,0},{4,4,0,0},{8,4,0,0},
                        {8,2,0,0},{4,2,0,0},{4,0,0,0},{0,0,0,0}
                    };
                    int bpp = bppT[mode][idx];
                    st->bitDepth = (bpp == 2) ? BD_2BPP : (bpp == 4) ? BD_4BPP :
                                   (bpp == 8) ? BD_8BPP : BD_4BPP;
                }
            }
            SendDlgItemMessage(hDlg, IDC_VRAMV_BITDEPTH, CB_SETCURSEL,
                               st->bitDepth, 0);
            st->address = addr & AddressMask(st->bitDepth);
            st->selectedTile = -1;
            UpdateAddressEdit(hDlg, st);
            UpdateTileInfo(hDlg, st);
            RedrawTiles(hDlg);
            return TRUE;
        }

        case IDC_VRAMV_CANVAS:
            if (code == STN_CLICKED) {
                HandleCanvasClick(hDlg, st);
            }
            return TRUE;

        case IDC_VRAMV_PALETTE:
            if (code == STN_CLICKED) {
                HandlePaletteClick(hDlg, st);
            }
            return TRUE;

        case IDCANCEL:
        case IDOK:
            DestroyWindow(hDlg);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return TRUE;

    case WM_DESTROY: {
        VRAMState *st = GetState(hDlg);
        DebugViewers_Unregister(hDlg);
        if (st) {
            if (st->tileBmp) DeleteObject(st->tileBmp);
            if (st->palBmp)  DeleteObject(st->palBmp);
            delete st;
            SetWindowLongPtr(hDlg, DWLP_USER, 0);
        }
        gVRAMViewerHWND = NULL;
        return TRUE;
    }
    }
    return FALSE;
}

} // anonymous namespace

void WinShowVRAMViewerDialog() {
    if (!gVRAMViewerHWND) {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&icc);
        gVRAMViewerHWND = CreateDialog(g_hInst, MAKEINTRESOURCE(IDD_VRAM_VIEWER),
                                       GUI.hWnd, DlgVRAMViewer);
        if (gVRAMViewerHWND) ShowWindow(gVRAMViewerHWND, SW_SHOW);
    } else {
        SetActiveWindow(gVRAMViewerHWND);
    }
}
