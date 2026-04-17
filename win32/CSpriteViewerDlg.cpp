#include <windows.h>
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
constexpr int kPreviewDst = 128;

struct SPVState {
    int  selected;
    bool autoUpdate;
    HBITMAP bmp;
    uint32 *bits;
};

SPVState *GetState(HWND hDlg) {
    return (SPVState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

// Decode OBJSizeSelect into (smallWH, largeWH). For non-square sizes we return
// the maximum dim for a square-ish preview canvas; full non-square handling is
// handled by caller separately.
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

// One-time: insert 128 empty rows. Text for columns 1..7 is filled by UpdateListValues.
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
    }
}

// Per-frame: update only cell text. Suppress redraw during the bulk update so
// the control repaints once at the end rather than once per SetItemText.
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
    // Ask for one consolidated repaint without erasing the background (avoids flashes).
    RedrawWindow(hList, NULL, NULL, RDW_INVALIDATE | RDW_NOERASE);
}

void DrawPreview(HWND hDlg) {
    SPVState *st = GetState(hDlg);
    if (!st || !st->bits) return;

    for (int i = 0; i < kPreviewSrc * kPreviewSrc; ++i)
        st->bits[i] = 0xFF101010u;

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

    // Sprites use CGRAM entries 128..255 with palette nibble 0..7 -> 128 + pal*16.
    int palOff = 128 + (obj.Palette & 7) * 16;

    int tilesAcross = w / 8;
    int tilesDown   = h / 8;
    uint16 name = obj.Name;

    // Center sprite inside preview canvas.
    int ox = (kPreviewSrc - w) / 2;
    int oy = (kPreviewSrc - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    for (int ty = 0; ty < tilesDown; ++ty) {
        for (int tx = 0; tx < tilesAcross; ++tx) {
            int dx = obj.HFlip ? (tilesAcross - 1 - tx) : tx;
            int dy = obj.VFlip ? (tilesDown - 1 - ty) : ty;
            // Tile row is 16-wide: wrap column nibble, keep row nibble & high bit.
            uint16 col = (uint16)((name + dx) & 0x0F);
            uint16 row = (uint16)((name + dy * 16) & 0xFF0);
            uint16 tileNum = (uint16)(col | row);
            uint32 addr = (PPU.OBJNameBase + tileNum * 32 +
                           (tileNum >= 256 ? PPU.OBJNameSelect : 0)) & 0xFFFF;
            uint8 tile[64];
            DecodeTile8x8(addr, 4, tile);
            int px = ox + tx * 8;
            int py = oy + ty * 8;
            if (px + 8 <= kPreviewSrc && py + 8 <= kPreviewSrc) {
                BlitTile8x8BGRA(tile, pal, palOff, 0x0F,
                                st->bits, kPreviewSrc,
                                px, py, obj.HFlip, obj.VFlip);
            }
        }
    }

    TCHAR det[160];
    _sntprintf(det, 160,
               _T("Sprite #%d  size %dx%d  pos (%d,%d)  tile 0x%03X  pal %d  pri %d  flags %s%s"),
               idx, w, h, obj.HPos, obj.VPos, obj.Name & 0x1FF,
               obj.Palette, obj.Priority,
               obj.HFlip ? _T("H") : _T("-"),
               obj.VFlip ? _T("V") : _T("-"));
    SetDlgItemText(hDlg, IDC_SPV_DETAILS, det);

    InvalidateRect(GetDlgItem(hDlg, IDC_SPV_PREVIEW), NULL, FALSE);
}

void Refresh(HWND hDlg) {
    HWND hList = GetDlgItem(hDlg, IDC_SPV_LIST);
    UpdateListValues(hList);
    DrawPreview(hDlg);
}

void HandleDrawItem(HWND hDlg, DRAWITEMSTRUCT *dis) {
    SPVState *st = GetState(hDlg);
    if (!st || dis->CtlID != IDC_SPV_PREVIEW || !st->bmp) return;

    int w = dis->rcItem.right - dis->rcItem.left;
    int h = dis->rcItem.bottom - dis->rcItem.top;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kPreviewSrc;
    bmi.bmiHeader.biHeight = -kPreviewSrc;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dis->hDC, COLORONCOLOR);
    StretchDIBits(dis->hDC, 0, 0, w, h, 0, 0, kPreviewSrc, kPreviewSrc,
                  st->bits, &bmi, DIB_RGB_COLORS, SRCCOPY);
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
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
}

INT_PTR CALLBACK DlgSpriteViewer(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        SPVState *st = new SPVState();
        st->selected = 0;
        st->autoUpdate = true;
        st->bmp = CreateBGRADib(kPreviewSrc, kPreviewSrc, &st->bits);
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);

        CheckDlgButton(hDlg, IDC_SPV_AUTOUPDATE, BST_CHECKED);
        {
            HWND hList = GetDlgItem(hDlg, IDC_SPV_LIST);
            SetupListColumns(hList);
            InitializeListRows(hList);
            ListView_SetItemState(hList, 0,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }

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
        if (n->idFrom == IDC_SPV_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nm = (NMLISTVIEW *)lParam;
            if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                SPVState *st = GetState(hDlg);
                if (st) {
                    st->selected = nm->iItem;
                    DrawPreview(hDlg);
                }
            }
        }
        return TRUE;
    }

    case WM_COMMAND: {
        SPVState *st = GetState(hDlg);
        if (!st) break;
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_SPV_AUTOUPDATE:
            st->autoUpdate =
                (IsDlgButtonChecked(hDlg, IDC_SPV_AUTOUPDATE) == BST_CHECKED);
            return TRUE;
        case IDC_SPV_REFRESH:
            Refresh(hDlg);
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
        DebugViewers_Unregister(hDlg);
        if (st) {
            if (st->bmp) DeleteObject(st->bmp);
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
