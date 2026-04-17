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

// Canvas dims: 16 tiles across * 8px * zoom; 32 rows * 8px * zoom.
constexpr int kTilesAcross = 16;
constexpr int kTileRows = 32;
constexpr int kZoom = 2; // fixed 2x
constexpr int kCanvasW = kTilesAcross * 8 * kZoom;  // 256
constexpr int kCanvasH = kTileRows * 8 * kZoom;     // 512
constexpr int kSrcW = kTilesAcross * 8;             // 128
constexpr int kSrcH = kTileRows * 8;                // 256

// Palette swatch: 16x16 grid, 8px each = 128x128.
constexpr int kPalSwatchPx = 8;
constexpr int kPalSrcSize = 16 * kPalSwatchPx; // 128

struct VRAMState {
    int bpp;            // 2, 4, 8, or -7 (Mode 7 sentinel)
    uint32 startByte;   // VRAM byte offset for top-left of canvas
    int palRow;         // which palette row (0..15) tints 4bpp view
    bool useCGRAM;
    bool autoUpdate;

    HBITMAP tileBmp;
    uint32 *tileBits;
    HBITMAP palBmp;
    uint32 *palBits;
};

VRAMState *GetState(HWND hDlg) {
    return (VRAMState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

void SetAddrEditHex(HWND hDlg, int id, uint32 v) {
    TCHAR buf[16];
    _sntprintf(buf, 16, _T("0x%04X"), v & 0xFFFF);
    SetDlgItemText(hDlg, id, buf);
}

int BytesPerTile(int bpp) {
    if (bpp == -7) return 128; // Mode 7: 64 bytes char at even addrs spread across 128
    if (bpp == 2) return 16;
    if (bpp == 4) return 32;
    return 64; // 8bpp
}

void RedrawTiles(HWND hDlg) {
    VRAMState *st = GetState(hDlg);
    if (!st || !st->tileBits) return;

    uint32 pal[256];
    if (st->useCGRAM) {
        SnapshotPaletteBGRA(pal);
    } else {
        // Grayscale ramp scaled to the current bit depth so all levels are visible.
        int steps = (st->bpp == 2) ? 4 : (st->bpp == 4) ? 16 : 256;
        for (int i = 0; i < 256; ++i) {
            uint32 v = (uint32)((i % steps) * 255 / (steps - 1));
            pal[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    }

    // Clear background.
    for (int i = 0; i < kSrcW * kSrcH; ++i) st->tileBits[i] = 0xFF202020u;

    int bpp = st->bpp;
    int palOffset = 0;
    int idxMask = 0;
    if (bpp == 4) {
        palOffset = (st->palRow & 0x0F) * 16;
        idxMask = 0x0F;
    } else if (bpp == 2) {
        palOffset = (st->palRow & 0x0F) * 4;
        idxMask = 0x03;
    }

    int tileStride = BytesPerTile(bpp);
    int tilesTotal = kTilesAcross * kTileRows;

    for (int t = 0; t < tilesTotal; ++t) {
        uint8 tile[64];
        uint32 addr = (st->startByte + t * tileStride) & 0xFFFF;
        if (bpp == -7) {
            DecodeMode7Tile8x8((addr / 128) & 0x1FF, tile);
        } else {
            DecodeTile8x8(addr, bpp, tile);
        }
        int tx = (t % kTilesAcross) * 8;
        int ty = (t / kTilesAcross) * 8;
        BlitTile8x8BGRA(tile, pal, palOffset, idxMask,
                        st->tileBits, kSrcW, tx, ty, false, false);
    }

    InvalidateRect(GetDlgItem(hDlg, IDC_VRAMV_CANVAS), NULL, FALSE);
}

void RedrawPalette(HWND hDlg) {
    VRAMState *st = GetState(hDlg);
    if (!st || !st->palBits) return;

    uint32 pal[256];
    SnapshotPaletteBGRA(pal);

    for (int i = 0; i < 256; ++i) {
        int gx = (i % 16) * kPalSwatchPx;
        int gy = (i / 16) * kPalSwatchPx;
        for (int py = 0; py < kPalSwatchPx; ++py) {
            for (int px = 0; px < kPalSwatchPx; ++px) {
                st->palBits[(gy + py) * kPalSrcSize + (gx + px)] = pal[i];
            }
        }
    }
    InvalidateRect(GetDlgItem(hDlg, IDC_VRAMV_PALETTE), NULL, FALSE);
}

void RefreshAll(HWND hDlg) {
    RedrawTiles(hDlg);
    RedrawPalette(hDlg);
}

void ReadBppFromRadios(HWND hDlg, VRAMState *st) {
    if (IsDlgButtonChecked(hDlg, IDC_VRAMV_BPP_2)) st->bpp = 2;
    else if (IsDlgButtonChecked(hDlg, IDC_VRAMV_BPP_8)) st->bpp = 8;
    else if (IsDlgButtonChecked(hDlg, IDC_VRAMV_BPP_MODE7)) st->bpp = -7;
    else st->bpp = 4;
}

void HandleDrawItem(HWND hDlg, DRAWITEMSTRUCT *dis) {
    VRAMState *st = GetState(hDlg);
    if (!st) return;
    int w = dis->rcItem.right - dis->rcItem.left;
    int h = dis->rcItem.bottom - dis->rcItem.top;

    if (dis->CtlID == IDC_VRAMV_CANVAS && st->tileBmp) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kSrcW;
        bmi.bmiHeader.biHeight = -kSrcH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchDIBits(dis->hDC, 0, 0, w, h, 0, 0, kSrcW, kSrcH,
                      st->tileBits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else if (dis->CtlID == IDC_VRAMV_PALETTE && st->palBmp) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kPalSrcSize;
        bmi.bmiHeader.biHeight = -kPalSrcSize;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchDIBits(dis->hDC, 0, 0, w, h, 0, 0, kPalSrcSize, kPalSrcSize,
                      st->palBits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

INT_PTR CALLBACK DlgVRAMViewer(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        VRAMState *st = new VRAMState();
        st->bpp = 4;
        st->startByte = 0;
        st->palRow = 0;
        st->useCGRAM = true;
        st->autoUpdate = true;
        st->tileBmp = CreateBGRADib(kSrcW, kSrcH, &st->tileBits);
        st->palBmp = CreateBGRADib(kPalSrcSize, kPalSrcSize, &st->palBits);
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);

        CheckRadioButton(hDlg, IDC_VRAMV_BPP_2, IDC_VRAMV_BPP_MODE7, IDC_VRAMV_BPP_4);
        CheckDlgButton(hDlg, IDC_VRAMV_USECGRAM, BST_CHECKED);
        CheckDlgButton(hDlg, IDC_VRAMV_AUTOUPDATE, BST_CHECKED);

        SetAddrEditHex(hDlg, IDC_VRAMV_BG1_ADDR, 0x0000);
        SetAddrEditHex(hDlg, IDC_VRAMV_BG2_ADDR, 0x0000);
        SetAddrEditHex(hDlg, IDC_VRAMV_BG3_ADDR, 0x0000);
        SetAddrEditHex(hDlg, IDC_VRAMV_BG4_ADDR, 0x0000);
        SetAddrEditHex(hDlg, IDC_VRAMV_OAM1_ADDR, 0x8000);
        SetAddrEditHex(hDlg, IDC_VRAMV_OAM2_ADDR, 0xA000);

        DebugViewers_Register(hDlg, &st->autoUpdate);
        RefreshAll(hDlg);
        return TRUE;
    }
    case WM_DRAWITEM:
        HandleDrawItem(hDlg, (DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_USER_VIEWER_REFRESH:
        RefreshAll(hDlg);
        return TRUE;

    case WM_COMMAND: {
        VRAMState *st = GetState(hDlg);
        if (!st) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        switch (id) {
        case IDC_VRAMV_BPP_2:
        case IDC_VRAMV_BPP_4:
        case IDC_VRAMV_BPP_8:
        case IDC_VRAMV_BPP_MODE7:
            ReadBppFromRadios(hDlg, st);
            RedrawTiles(hDlg);
            return TRUE;
        case IDC_VRAMV_USECGRAM:
            st->useCGRAM = (IsDlgButtonChecked(hDlg, IDC_VRAMV_USECGRAM) == BST_CHECKED);
            RedrawTiles(hDlg);
            return TRUE;
        case IDC_VRAMV_AUTOUPDATE:
            st->autoUpdate = (IsDlgButtonChecked(hDlg, IDC_VRAMV_AUTOUPDATE) == BST_CHECKED);
            return TRUE;
        case IDC_VRAMV_REFRESH:
            RefreshAll(hDlg);
            return TRUE;
        case IDC_VRAMV_BG1_GOTO:
        case IDC_VRAMV_BG2_GOTO:
        case IDC_VRAMV_BG3_GOTO:
        case IDC_VRAMV_BG4_GOTO:
        case IDC_VRAMV_OAM1_GOTO:
        case IDC_VRAMV_OAM2_GOTO: {
            int editId = IDC_VRAMV_BG1_ADDR + (id - IDC_VRAMV_BG1_GOTO);
            TCHAR buf[32];
            GetDlgItemText(hDlg, editId, buf, 32);
            uint32 addr;
            if (ParseHex(buf, &addr)) {
                st->startByte = addr & 0xFFFF;
                RedrawTiles(hDlg);
            }
            return TRUE;
        }
        case IDCANCEL:
        case IDOK:
            DestroyWindow(hDlg);
            return TRUE;
        }
        (void)code;
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
            if (st->palBmp) DeleteObject(st->palBmp);
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
        gVRAMViewerHWND = CreateDialog(g_hInst, MAKEINTRESOURCE(IDD_VRAM_VIEWER),
                                       GUI.hWnd, DlgVRAMViewer);
        if (gVRAMViewerHWND) ShowWindow(gVRAMViewerHWND, SW_SHOW);
    } else {
        SetActiveWindow(gVRAMViewerHWND);
    }
}
