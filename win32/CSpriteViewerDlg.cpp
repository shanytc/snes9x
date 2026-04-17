// Sprite viewer — bsnes-plus oam-viewer parity:
//  * Top "Screen" view that composes all 128 OBJs at their live (x,y)
//    positions (priority-rotated via PPU.FirstSprite).
//  * ListView with per-row checkboxes to toggle individual sprites in the
//    Screen view.
//  * Right sidebar with preview canvas, zoom combo, background combo
//    (Transparent/Sprite-Palette-0..7/Magenta/Cyan/White/Black),
//    Show Screen Outline checkbox, First Sprite readout.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>

#include "CSpriteViewerDlg.h"
#include "debug_viewer_common.h"
#include "wsnes9x.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"

HWND gSpriteViewerHWND = NULL;
extern HINSTANCE g_hInst;

namespace {

constexpr int kPreviewSrc = 64;
constexpr int kScreenW = 512;   // sprite X goes -256..255; we draw at (x + 256)
constexpr int kScreenH = 256;

enum BgType {
    BG_TRANSPARENT = 0,
    BG_PALETTE_0, BG_PALETTE_1, BG_PALETTE_2, BG_PALETTE_3,
    BG_PALETTE_4, BG_PALETTE_5, BG_PALETTE_6, BG_PALETTE_7,
    BG_MAGENTA, BG_CYAN, BG_WHITE, BG_BLACK
};

struct SPVState {
    int  selected;
    bool autoUpdate;
    int  zoom;              // preview zoom 1..9
    bool showOutline;
    int  bgType;
    bool visible[128];      // per-sprite enable from the listview checkbox

    // Preview canvas DIB (shows selected sprite alone)
    HBITMAP previewBmp;
    uint32 *previewBits;

    // Screen canvas DIB (shows all enabled sprites at their live positions)
    HBITMAP screenBmp;
    uint32 *screenBits;

    // Pan offset for screen canvas.
    int screenViewX, screenViewY;
    int screenCurW, screenCurH; // always (kScreenW, kScreenH)
};

SPVState *GetState(HWND hDlg) {
    return (SPVState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

void SpriteDims(int sizeSel, bool isLarge, int *w, int *h) {
    static const int dims[8][4] = {
        {  8,  8, 16, 16},
        {  8,  8, 32, 32},
        {  8,  8, 64, 64},
        { 16, 16, 32, 32},
        { 16, 16, 64, 64},
        { 32, 32, 64, 64},
        { 16, 32, 32, 64},
        { 16, 32, 32, 32},
    };
    const int *d = dims[sizeSel & 7];
    if (isLarge) { *w = d[2]; *h = d[3]; }
    else         { *w = d[0]; *h = d[1]; }
}

// Address of sprite tile `n` (0..511), matching snes9x tileimpl.h:
//   TileAddr = OBJNameBase + (tile & 0x3FF) * 32
//   if (tile & 0x100) TileAddr += OBJNameSelect
// tileNum * 32 already covers the offset into the second tile table; only
// the NameSelect gap is added for tiles with bit 8 set.
uint32 SpriteTileAddr(uint16 tileNum) {
    uint32 a = PPU.OBJNameBase + (tileNum & 0x3FF) * 32;
    if (tileNum & 0x100) a += PPU.OBJNameSelect;
    return a & 0xFFFF;
}

// Build the tile index for row/column (dx, dy) within an obj.Name base.
// Sprite tile rows are 16 tiles wide and wrap within the current name
// table (bit 8 is preserved; row/col wrap in their 4-bit fields).
uint16 SpriteSubTile(uint16 name, int dx, int dy) {
    uint16 table = (uint16)(name & 0x100);
    uint16 col   = (uint16)((name + dx)       & 0x00F);
    uint16 row   = (uint16)((name + dy * 16)  & 0x0F0);
    return (uint16)(table | row | col);
}

// Draw one sprite into a BGRA buffer of the given stride, at canvas (cx, cy).
// pal is the 256-entry CGRAM snapshot; overwriteBg=true writes idx-0 pixels as
// palette[0] (used by "Background = Sprite Palette N" mode so you see the
// sprite's full tile data incl. transparent pixels).
void DrawSpriteAt(const SOBJ &obj, int cx, int cy,
                  uint32 *dst, int stride, int stridePixels,
                  const uint32 pal[256]) {
    int w, h;
    SpriteDims(PPU.OBJSizeSelect, obj.Size != 0, &w, &h);
    int palOff = 128 + (obj.Palette & 7) * 16;
    int tilesAcross = w / 8;
    int tilesDown   = h / 8;

    for (int ty = 0; ty < tilesDown; ++ty) {
        for (int tx = 0; tx < tilesAcross; ++tx) {
            int dx = obj.HFlip ? (tilesAcross - 1 - tx) : tx;
            int dy = obj.VFlip ? (tilesDown - 1 - ty) : ty;
            uint16 tileNum = SpriteSubTile(obj.Name, dx, dy);
            uint32 addr = SpriteTileAddr(tileNum);
            uint8 tile[64];
            DecodeTile8x8(addr, 4, tile);
            int px = cx + tx * 8;
            int py = cy + ty * 8;
            // Clip against the destination buffer bounds.
            if (px + 8 <= 0 || py + 8 <= 0) continue;
            if (px >= stridePixels || py >= stride) continue;

            // Custom blit: respects buffer bounds per-row.
            for (int yy = 0; yy < 8; ++yy) {
                int sy = obj.VFlip ? (7 - yy) : yy;
                int dpy = py + yy;
                if (dpy < 0 || dpy >= stride) continue;
                uint32 *row = dst + dpy * stridePixels;
                for (int xx = 0; xx < 8; ++xx) {
                    int sx = obj.HFlip ? (7 - xx) : xx;
                    uint8 idx = tile[sy * 8 + sx];
                    if (idx == 0) continue;
                    int dpx = px + xx;
                    if (dpx < 0 || dpx >= stridePixels) continue;
                    row[dpx] = pal[(palOff + idx) & 0xFF];
                }
            }
        }
    }
}

uint32 BackgroundColor(const SPVState *st, const uint32 pal[256]) {
    switch (st->bgType) {
    case BG_TRANSPARENT: return 0xFF303030u;   // dark gray for contrast
    case BG_PALETTE_0: case BG_PALETTE_1: case BG_PALETTE_2: case BG_PALETTE_3:
    case BG_PALETTE_4: case BG_PALETTE_5: case BG_PALETTE_6: case BG_PALETTE_7:
        return pal[128 + (st->bgType - BG_PALETTE_0) * 16];
    case BG_MAGENTA: return 0xFFFF00FFu;
    case BG_CYAN:    return 0xFF00FFFFu;
    case BG_WHITE:   return 0xFFFFFFFFu;
    case BG_BLACK:   return 0xFF000000u;
    }
    return 0xFF303030u;
}

void DrawScreen(HWND hDlg) {
    SPVState *st = GetState(hDlg);
    if (!st || !st->screenBits) return;

    uint32 pal[256];
    SnapshotPaletteBGRA(pal);
    uint32 bg = BackgroundColor(st, pal);

    for (int i = 0; i < kScreenW * kScreenH; ++i) st->screenBits[i] = bg;

    // Lowest-priority first so highest-priority sprite ends up on top.
    // Iteration order per SNES priority rotation: start at FirstSprite, go
    // forward (wrapping); earlier in iteration = higher priority.
    int fs = PPU.FirstSprite & 127;
    for (int i = 127; i >= 0; --i) {
        int id = (fs + i) & 127;
        if (!st->visible[id]) continue;
        const SOBJ &obj = PPU.OBJ[id];
        int w, h;
        SpriteDims(PPU.OBJSizeSelect, obj.Size != 0, &w, &h);
        int cx = obj.HPos + 256;
        int cy = obj.VPos;

        DrawSpriteAt(obj, cx, cy,
                     st->screenBits, kScreenH, kScreenW, pal);
        // Y-wrap: sprites with y >= 240 wrap back through 0.
        if (cy + h > kScreenH) {
            DrawSpriteAt(obj, cx, cy - kScreenH,
                         st->screenBits, kScreenH, kScreenW, pal);
        }
    }

    // Screen outline: visible SNES screen is 256x224, centered at x=256.
    if (st->showOutline) {
        uint32 outline = 0xFFFFFF00u; // yellow
        int x0 = 256, x1 = 256 + 256 - 1;
        int y0 = 0,   y1 = 224 - 1;
        for (int x = x0; x <= x1; ++x) {
            st->screenBits[y0 * kScreenW + x] = outline;
            st->screenBits[y1 * kScreenW + x] = outline;
        }
        for (int y = y0; y <= y1; ++y) {
            st->screenBits[y * kScreenW + x0] = outline;
            st->screenBits[y * kScreenW + x1] = outline;
        }
    }

    InvalidateRect(GetDlgItem(hDlg, IDC_SPV_SCREEN), NULL, FALSE);
}

void DrawPreview(HWND hDlg) {
    SPVState *st = GetState(hDlg);
    if (!st || !st->previewBits) return;

    for (int i = 0; i < kPreviewSrc * kPreviewSrc; ++i)
        st->previewBits[i] = 0xFF101010u;

    int idx = st->selected;
    if (idx < 0 || idx >= 128) {
        InvalidateRect(GetDlgItem(hDlg, IDC_SPV_PREVIEW), NULL, FALSE);
        SetDlgItemText(hDlg, IDC_SPV_DETAILS, _T(""));
        return;
    }

    const SOBJ &obj = PPU.OBJ[idx];
    int w, h;
    SpriteDims(PPU.OBJSizeSelect, obj.Size != 0, &w, &h);

    uint32 pal[256];
    SnapshotPaletteBGRA(pal);

    int ox = (kPreviewSrc - w) / 2; if (ox < 0) ox = 0;
    int oy = (kPreviewSrc - h) / 2; if (oy < 0) oy = 0;

    DrawSpriteAt(obj, ox, oy,
                 st->previewBits, kPreviewSrc, kPreviewSrc, pal);

    TCHAR det[200];
    _sntprintf(det, 200,
               _T("Sprite #%d\nsize %dx%d\npos (%d,%d)\ntile 0x%03X\npal %d  pri %d\nflags %s%s"),
               idx, w, h, obj.HPos, obj.VPos, obj.Name & 0x1FF,
               obj.Palette, obj.Priority,
               obj.HFlip ? _T("H") : _T("-"),
               obj.VFlip ? _T("V") : _T("-"));
    SetDlgItemText(hDlg, IDC_SPV_DETAILS, det);

    InvalidateRect(GetDlgItem(hDlg, IDC_SPV_PREVIEW), NULL, FALSE);
}

void InitializeListRows(HWND hList) {
    for (int i = 0; i < 128; ++i) {
        LVITEM lv = {};
        lv.mask = LVIF_TEXT;
        lv.iItem = i;
        lv.iSubItem = 0;
        TCHAR col[16];
        _sntprintf(col, 16, _T("%d"), i);
        lv.pszText = col;
        SendMessage(hList, LVM_INSERTITEM, 0, (LPARAM)&lv);
        // Start all sprites visible.
        ListView_SetCheckState(hList, i, TRUE);
    }
}

void UpdateListValues(HWND hList) {
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < 128; ++i) {
        const SOBJ &obj = PPU.OBJ[i];
        int w, h;
        SpriteDims(PPU.OBJSizeSelect, obj.Size != 0, &w, &h);

        TCHAR col[40];
        _sntprintf(col, 40, _T("%dx%d"), w, h);
        ListView_SetItemText(hList, i, 1, col);
        _sntprintf(col, 40, _T("%d"), obj.HPos);
        ListView_SetItemText(hList, i, 2, col);
        _sntprintf(col, 40, _T("%d"), obj.VPos);
        ListView_SetItemText(hList, i, 3, col);
        _sntprintf(col, 40, _T("0x%03X"), obj.Name & 0x1FF);
        ListView_SetItemText(hList, i, 4, col);
        _sntprintf(col, 40, _T("%d"), obj.Priority);
        ListView_SetItemText(hList, i, 5, col);
        _sntprintf(col, 40, _T("%d"), obj.Palette);
        ListView_SetItemText(hList, i, 6, col);
        _sntprintf(col, 40, _T("%s%s"),
                   obj.HFlip ? _T("H") : _T("-"),
                   obj.VFlip ? _T("V") : _T("-"));
        ListView_SetItemText(hList, i, 7, col);
    }
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hList, NULL, NULL, RDW_INVALIDATE | RDW_NOERASE);
}

void Refresh(HWND hDlg) {
    HWND hList = GetDlgItem(hDlg, IDC_SPV_LIST);
    UpdateListValues(hList);

    TCHAR buf[8];
    _sntprintf(buf, 8, _T("%d"), PPU.FirstSprite & 127);
    SetDlgItemText(hDlg, IDC_SPV_FIRST_SPRITE, buf);

    DrawScreen(hDlg);
    DrawPreview(hDlg);
}

void HandleDrawItem(HWND hDlg, DRAWITEMSTRUCT *dis) {
    SPVState *st = GetState(hDlg);
    if (!st) return;

    int w = dis->rcItem.right - dis->rcItem.left;
    int h = dis->rcItem.bottom - dis->rcItem.top;

    if (dis->CtlID == IDC_SPV_PREVIEW && st->previewBmp) {
        // Preview renders the single selected sprite 1:1 centred; no user zoom.
        FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(BLACK_BRUSH));
        HDC memDC = CreateCompatibleDC(dis->hDC);
        HGDIOBJ oldBmp = SelectObject(memDC, st->previewBmp);
        int drawW = kPreviewSrc; if (drawW > w) drawW = w;
        int drawH = kPreviewSrc; if (drawH > h) drawH = h;
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchBlt(dis->hDC, 0, 0, drawW, drawH,
                   memDC, 0, 0, drawW, drawH, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
    } else if (dis->CtlID == IDC_SPV_SCREEN && st->screenBmp) {
        FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(BLACK_BRUSH));
        int scale = st->zoom < 1 ? 1 : st->zoom;
        int srcW = w / scale;
        int srcH = h / scale;
        if (srcW > kScreenW - st->screenViewX) srcW = kScreenW - st->screenViewX;
        if (srcH > kScreenH - st->screenViewY) srcH = kScreenH - st->screenViewY;
        if (srcW > 0 && srcH > 0) {
            HDC memDC = CreateCompatibleDC(dis->hDC);
            HGDIOBJ oldBmp = SelectObject(memDC, st->screenBmp);
            SetStretchBltMode(dis->hDC, COLORONCOLOR);
            StretchBlt(dis->hDC,
                       0, 0, srcW * scale, srcH * scale,
                       memDC, st->screenViewX, st->screenViewY, srcW, srcH,
                       SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
        }
    }
}

void SetupListColumns(HWND hList) {
    struct { const TCHAR *name; int width; } cols[] = {
        { _T("#"),     30 },
        { _T("Size"),  48 },
        { _T("X"),     40 },
        { _T("Y"),     40 },
        { _T("Char"),  48 },
        { _T("Pri"),   30 },
        { _T("Pal"),   30 },
        { _T("Flags"), 48 },
    };
    for (int i = 0; i < (int)(sizeof(cols) / sizeof(cols[0])); ++i) {
        LVCOLUMN col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPTSTR)cols[i].name;
        col.cx = cols[i].width;
        col.iSubItem = i;
        SendMessage(hList, LVM_INSERTCOLUMN, i, (LPARAM)&col);
    }
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
        LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
}

void PopulateZoom(HWND hCombo) {
    for (int i = 1; i <= 9; ++i) {
        TCHAR buf[8];
        _sntprintf(buf, 8, _T("%dx"), i);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

void PopulateBgCombo(HWND hCombo) {
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Transparent"));
    for (int i = 0; i < 8; ++i) {
        TCHAR buf[24];
        _sntprintf(buf, 24, _T("Sprite Palette %d"), i);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Magenta"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Cyan"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("White"));
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)_T("Black"));
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

INT_PTR CALLBACK DlgSpriteViewer(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        SPVState *st = new SPVState();
        st->selected = 0;
        st->autoUpdate = true;
        st->zoom = 1;
        st->showOutline = true;
        st->bgType = BG_TRANSPARENT;
        for (int i = 0; i < 128; ++i) st->visible[i] = true;
        st->previewBmp = CreateBGRADib(kPreviewSrc, kPreviewSrc, &st->previewBits);
        st->screenBmp  = CreateBGRADib(kScreenW, kScreenH, &st->screenBits);
        st->screenViewX = 0;
        st->screenViewY = 0;
        st->screenCurW = kScreenW;
        st->screenCurH = kScreenH;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);

        CheckDlgButton(hDlg, IDC_SPV_AUTOUPDATE, BST_CHECKED);
        CheckDlgButton(hDlg, IDC_SPV_SHOW_OUTLINE, BST_CHECKED);
        {
            HWND hList = GetDlgItem(hDlg, IDC_SPV_LIST);
            SetupListColumns(hList);
            InitializeListRows(hList);
            ListView_SetItemState(hList, 0,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        PopulateZoom(GetDlgItem(hDlg, IDC_SPV_ZOOM));
        PopulateBgCombo(GetDlgItem(hDlg, IDC_SPV_BG));

        InstallDragPan(GetDlgItem(hDlg, IDC_SPV_SCREEN),
                       &st->screenViewX, &st->screenViewY,
                       &st->zoom, &st->screenCurW, &st->screenCurH);

        DebugViewers_Register(hDlg, &st->autoUpdate);
        Refresh(hDlg);
        return TRUE;
    }

    case WM_DRAWITEM:
        HandleDrawItem(hDlg, (DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_USER_VIEWER_REFRESH:
        Refresh(hDlg);
        return TRUE;

    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lParam;
        if (n->idFrom == IDC_SPV_LIST) {
            if (n->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *nm = (NMLISTVIEW *)lParam;
                SPVState *st = GetState(hDlg);
                if (!st) return TRUE;

                // Selection change -> redraw preview.
                if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                    st->selected = nm->iItem;
                    DrawPreview(hDlg);
                }
                // Check-state change (OS encodes it in uNewState bits 12..15).
                UINT oldCheck = (nm->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                UINT newCheck = (nm->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                if (oldCheck != 0 && newCheck != 0 && oldCheck != newCheck
                    && nm->iItem >= 0 && nm->iItem < 128) {
                    st->visible[nm->iItem] = (newCheck == 2);
                    DrawScreen(hDlg);
                }
            }
        }
        return TRUE;
    }

    case WM_COMMAND: {
        SPVState *st = GetState(hDlg);
        if (!st) break;
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        switch (id) {
        case IDC_SPV_AUTOUPDATE:
            st->autoUpdate = (IsDlgButtonChecked(hDlg, IDC_SPV_AUTOUPDATE) == BST_CHECKED);
            return TRUE;
        case IDC_SPV_REFRESH:
            Refresh(hDlg);
            return TRUE;
        case IDC_SPV_SHOW_OUTLINE:
            st->showOutline = (IsDlgButtonChecked(hDlg, IDC_SPV_SHOW_OUTLINE) == BST_CHECKED);
            DrawScreen(hDlg);
            return TRUE;
        case IDC_SPV_ZOOM:
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessage(hDlg, IDC_SPV_ZOOM, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    st->zoom = sel + 1;
                    st->screenViewX = 0;
                    st->screenViewY = 0;
                    InvalidateRect(GetDlgItem(hDlg, IDC_SPV_SCREEN), NULL, FALSE);
                }
            }
            return TRUE;
        case IDC_SPV_BG:
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessage(hDlg, IDC_SPV_BG, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    st->bgType = sel;
                    DrawScreen(hDlg);
                }
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
        SPVState *st = GetState(hDlg);
        UninstallDragPan(GetDlgItem(hDlg, IDC_SPV_SCREEN));
        DebugViewers_Unregister(hDlg);
        if (st) {
            if (st->previewBmp) DeleteObject(st->previewBmp);
            if (st->screenBmp)  DeleteObject(st->screenBmp);
            delete st;
            SetWindowLongPtr(hDlg, DWLP_USER, 0);
        }
        gSpriteViewerHWND = NULL;
        return TRUE;
    }
    }
    return FALSE;
}

} // anonymous namespace

void WinShowSpriteViewerDialog() {
    if (!gSpriteViewerHWND) {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);
        gSpriteViewerHWND = CreateDialog(g_hInst, MAKEINTRESOURCE(IDD_SPRITE_VIEWER),
                                         GUI.hWnd, DlgSpriteViewer);
        if (gSpriteViewerHWND) ShowWindow(gSpriteViewerHWND, SW_SHOW);
    } else {
        SetActiveWindow(gSpriteViewerHWND);
    }
}
