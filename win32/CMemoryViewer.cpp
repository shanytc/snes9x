#include "CMemoryViewer.h"
#include "CMemoryRegions.h"
#include "CMemHeatmap.h"
#include "CDebuggerDlg.h"   // DebuggerPromptHex
#include "../snes9x.h"
#include <commctrl.h>
#include <windowsx.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerMemoryViewer");

enum
{
	IDC_MV_COMBO     = 8000,
	IDC_MV_LV        = 8001,
	IDC_MV_ZONE      = 8002,
	IDC_MV_CLEAR     = 8003,
	IDC_MV_ZONE_LBL  = 8004,

	IDM_MV_GOTO      = 8100
};

enum { MVCOL_ADDR = 0, MVCOL_HEX = 1, MVCOL_ASCII = 2 };

struct MemViewerState
{
	DbgSystem sys             = DbgSystem::None;
	HFONT     font            = nullptr;
	HWND      combo           = nullptr;
	HWND      lv              = nullptr;
	HWND      zone_combo      = nullptr;
	HWND      zone_label      = nullptr;
	HWND      clear_btn       = nullptr;
	int       current_region  = 0;
	MemZone   zone            = MemZone::None;
	bool      heatmap_acquired = false;
};

static HWND g_snes_memview = NULL;
static HWND g_gb_memview   = NULL;

static MemViewerState *GetState(HWND h)
{
	return (MemViewerState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static const MemRegion *CurRegion(MemViewerState *st)
{
	return MemRegionAt(st->sys, st->current_region);
}

static void UpdateRowCount(MemViewerState *st)
{
	const MemRegion *r = CurRegion(st);
	const uint32_t size = r ? r->get_size() : 0;
	const int rows = (int)((size + 15) / 16);
	ListView_SetItemCountEx(st->lv, rows, LVSICF_NOINVALIDATEALL);
	InvalidateRect(st->lv, NULL, FALSE);
}

// Scroll the view to a region offset (matching what the Address column shows),
// placing it near the top of the list and selecting that row.
static void GoToAddress(MemViewerState *st, uint32_t addr)
{
	const MemRegion *r = CurRegion(st);
	if (!r) return;
	const uint32_t size = r->get_size();
	if (size == 0) return;
	if (addr >= size) addr = size - 1;

	const int total = (int)((size + 15) / 16);
	int row = (int)(addr / 16u);
	if (row >= total) row = total - 1;

	// Anchor one page below so the target lands near the top, then ensure
	// the target row itself is visible and selected.
	const int per_page = ListView_GetCountPerPage(st->lv);
	if (per_page > 2)
	{
		int anchor = row + per_page - 1;
		if (anchor > total - 1) anchor = total - 1;
		ListView_EnsureVisible(st->lv, anchor, FALSE);
	}
	ListView_EnsureVisible(st->lv, row, FALSE);
	ListView_SetItemState(st->lv, row, LVIS_SELECTED | LVIS_FOCUSED,
	                      LVIS_SELECTED | LVIS_FOCUSED);
	InvalidateRect(st->lv, NULL, FALSE);
}

static void DoGoToPrompt(HWND h, MemViewerState *st)
{
	const MemRegion *r = CurRegion(st);
	if (!r) return;
	uint32_t addr = 0;
	if (DebuggerPromptHex(h, TEXT("Go to Address"),
	                      TEXT("Offset in current region (hex):"),
	                      TEXT("0"), &addr))
		GoToAddress(st, addr);
}

static void PopulateCombo(MemViewerState *st)
{
	SendMessage(st->combo, CB_RESETCONTENT, 0, 0);
	const int n = MemRegionCount(st->sys);
	for (int i = 0; i < n; i++)
	{
		const MemRegion *r = MemRegionAt(st->sys, i);
		if (r) SendMessageA(st->combo, CB_ADDSTRING, 0, (LPARAM)r->name);
	}
	if (n > 0)
	{
		SendMessage(st->combo, CB_SETCURSEL, 0, 0);
		st->current_region = 0;
	}
}

static void PopulateZoneCombo(MemViewerState *st)
{
	SendMessageA(st->zone_combo, CB_RESETCONTENT, 0, 0);
	SendMessageA(st->zone_combo, CB_ADDSTRING, 0, (LPARAM)"None");
	SendMessageA(st->zone_combo, CB_ADDSTRING, 0, (LPARAM)"Read zone");
	SendMessageA(st->zone_combo, CB_ADDSTRING, 0, (LPARAM)"Write zone");
	SendMessage(st->zone_combo, CB_SETCURSEL, 0, 0);
}

static void SetZone(MemViewerState *st, MemZone z)
{
	if (st->zone == z) return;
	const bool now_active = z != MemZone::None;
	st->zone = z;
	if (now_active && !st->heatmap_acquired)
	{
		MemHeatmap::Acquire();
		st->heatmap_acquired = true;
	}
	else if (!now_active && st->heatmap_acquired)
	{
		MemHeatmap::Release();
		st->heatmap_acquired = false;
	}
	InvalidateRect(st->lv, NULL, FALSE);
}

static void OnGetDispInfo(MemViewerState *st, NMLVDISPINFOA *di)
{
	const MemRegion *r = CurRegion(st);
	if (!r) { di->item.pszText[0] = 0; return; }

	const int idx = di->item.iItem;
	const uint32_t base = (uint32_t)idx * 16u;
	const uint32_t size = r->get_size();

	if (!(di->item.mask & LVIF_TEXT)) return;

	char *out = di->item.pszText;
	const int cap = di->item.cchTextMax;

	if (di->item.iSubItem == MVCOL_ADDR)
	{
		const unsigned w = r->addr_format_width;
		char fmt[16];
		_snprintf_s(fmt, 16, _TRUNCATE, "%%0%uX", w);
		_snprintf_s(out, cap, _TRUNCATE, fmt, (unsigned)base);
		return;
	}

	if (di->item.iSubItem == MVCOL_HEX)
	{
		char *p = out;
		int remaining = cap;
		for (uint32_t i = 0; i < 16; i++)
		{
			const uint32_t off = base + i;
			if (off < size)
			{
				const uint8_t b = r->read_byte(off);
				int w = _snprintf_s(p, remaining, _TRUNCATE,
				                    (i == 0) ? "%02X" : " %02X", b);
				if (w < 0) break;
				p += w;
				remaining -= w;
			}
			else
			{
				int w = _snprintf_s(p, remaining, _TRUNCATE,
				                    (i == 0) ? "  " : "   ");
				if (w < 0) break;
				p += w;
				remaining -= w;
			}
			if (remaining <= 1) break;
		}
		return;
	}

	if (di->item.iSubItem == MVCOL_ASCII)
	{
		char *p = out;
		int n = 0;
		for (uint32_t i = 0; i < 16 && n < cap - 1; i++)
		{
			const uint32_t off = base + i;
			char c = '.';
			if (off < size)
			{
				const uint8_t b = r->read_byte(off);
				c = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
			}
			else
			{
				c = ' ';
			}
			p[n++] = c;
		}
		p[n] = 0;
		return;
	}
}

static uint16_t HeatmapValue(MemViewerState *st, const MemRegion *r, uint32_t off)
{
	if (!r || !r->to_cpu_addr) return 0;
	const uint32_t addr = r->to_cpu_addr(off);
	if (addr == kNoCpuAddr) return 0;
	const bool reads = st->zone == MemZone::Reads;
	return st->sys == DbgSystem::Snes ? MemHeatmap::GetSnes(addr, reads)
	                                  : MemHeatmap::GetGb((uint16_t)addr, reads);
}

static COLORREF HeatmapColor(uint16_t count, bool &text_white)
{
	text_white = false;
	if (count == 0) return RGB(255, 255, 255);
	// Counters are halved once per second, so the value tracks roughly the
	// access rate. Saturate at ~1000 hits/sec so a byte being touched every
	// frame reaches deep red, and rare writes stay pale pink.
	static const double kDenom = log(1024.0);
	double t = log((double)count + 1.0) / kDenom;
	if (t > 1.0) t = 1.0;
	if (t < 0.0) t = 0.0;
	const int g = (int)((1.0 - t) * 255.0 + 0.5);
	const int b = g;
	if (t >= 0.55) text_white = true;
	return RGB(255, g, b);
}

static void DrawHexCell(MemViewerState *st, NMLVCUSTOMDRAW *cd)
{
	const MemRegion *r = CurRegion(st);
	if (!r) return;

	const int row = (int)cd->nmcd.dwItemSpec;
	const uint32_t base = (uint32_t)row * 16u;
	const uint32_t size = r->get_size();

	RECT rc;
	rc.left = LVIR_BOUNDS;
	ListView_GetSubItemRect(st->lv, row, MVCOL_HEX, LVIR_BOUNDS, &rc);

	HDC hdc = cd->nmcd.hdc;

	const bool selected =
		(ListView_GetItemState(st->lv, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;

	const COLORREF bg_row = selected ? GetSysColor(COLOR_HIGHLIGHT)
	                                 : GetSysColor(COLOR_WINDOW);
	HBRUSH bg_brush = CreateSolidBrush(bg_row);
	FillRect(hdc, &rc, bg_brush);
	DeleteObject(bg_brush);

	HFONT old_font = (HFONT)SelectObject(hdc, st->font);
	SetBkMode(hdc, TRANSPARENT);

	TEXTMETRIC tm;
	GetTextMetrics(hdc, &tm);
	const int char_w = tm.tmAveCharWidth;
	const int pad_left = 6;
	const int y = rc.top + (rc.bottom - rc.top - tm.tmHeight) / 2;

	int x = rc.left + pad_left;
	for (uint32_t i = 0; i < 16; i++)
	{
		const uint32_t off = base + i;
		char buf[4];
		bool has_byte = (off < size);

		if (has_byte)
		{
			const uint8_t b = r->read_byte(off);
			_snprintf_s(buf, 4, _TRUNCATE, "%02X", b);
		}
		else
		{
			buf[0] = ' '; buf[1] = ' '; buf[2] = 0;
		}

		RECT cell;
		cell.left   = x - (char_w / 2);
		cell.right  = x + char_w * 2 + (char_w + 1) / 2;
		cell.top    = rc.top;
		cell.bottom = rc.bottom;

		bool text_white = false;
		if (has_byte && st->zone != MemZone::None)
		{
			const uint16_t count = HeatmapValue(st, r, off);
			if (count > 0)
			{
				const COLORREF heat = HeatmapColor(count, text_white);
				HBRUSH hb = CreateSolidBrush(heat);
				FillRect(hdc, &cell, hb);
				DeleteObject(hb);
			}
		}

		COLORREF text_color;
		if (text_white)
			text_color = RGB(255, 255, 255);
		else if (selected)
			text_color = GetSysColor(COLOR_HIGHLIGHTTEXT);
		else
			text_color = GetSysColor(COLOR_WINDOWTEXT);
		SetTextColor(hdc, text_color);

		TextOutA(hdc, x, y, buf, (int)strlen(buf));

		x += char_w * 3;
	}

	SelectObject(hdc, old_font);
}

static LRESULT HandleCustomDraw(MemViewerState *st, NMLVCUSTOMDRAW *cd)
{
	switch (cd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
			return CDRF_NOTIFYITEMDRAW;
		case CDDS_ITEMPREPAINT:
			return CDRF_NOTIFYSUBITEMDRAW;
		case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
			if (cd->iSubItem == MVCOL_HEX && st->zone != MemZone::None)
			{
				DrawHexCell(st, cd);
				return CDRF_SKIPDEFAULT;
			}
			return CDRF_DODEFAULT;
	}
	return CDRF_DODEFAULT;
}

static void DoResize(HWND hwnd, MemViewerState *st)
{
	if (!st) return;
	RECT rc;
	GetClientRect(hwnd, &rc);
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;

	const int top_h   = 28;
	const int padding = 4;
	if (st->combo)      MoveWindow(st->combo,      padding,        padding, 180, 200, TRUE);
	if (st->zone_label) MoveWindow(st->zone_label, padding + 192,  padding + 4, 36, 18, TRUE);
	if (st->zone_combo) MoveWindow(st->zone_combo, padding + 230,  padding, 120, 200, TRUE);
	if (st->clear_btn)  MoveWindow(st->clear_btn,  padding + 358,  padding, 60,  20,  TRUE);
	if (st->lv)         MoveWindow(st->lv,         0, top_h, w, h - top_h, TRUE);
}

static LRESULT CALLBACK MemViewerWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			MemViewerState *st = new MemViewerState();
			st->sys  = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			MemViewerState *st = GetState(h);

			st->combo = CreateWindowEx(0, TEXT("COMBOBOX"), TEXT(""),
			                           WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                           | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
			                           0, 0, 0, 0,
			                           h, (HMENU)(LONG_PTR)IDC_MV_COMBO,
			                           GetModuleHandle(NULL), NULL);
			SendMessage(st->combo, WM_SETFONT,
			            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

			st->zone_label = CreateWindowExA(0, "STATIC", "Zone:",
			                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
			                                 0, 0, 0, 0, h,
			                                 (HMENU)(LONG_PTR)IDC_MV_ZONE_LBL,
			                                 GetModuleHandle(NULL), NULL);
			SendMessage(st->zone_label, WM_SETFONT,
			            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

			st->zone_combo = CreateWindowEx(0, TEXT("COMBOBOX"), TEXT(""),
			                                WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                                | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
			                                0, 0, 0, 0,
			                                h, (HMENU)(LONG_PTR)IDC_MV_ZONE,
			                                GetModuleHandle(NULL), NULL);
			SendMessage(st->zone_combo, WM_SETFONT,
			            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

			st->clear_btn = CreateWindowExA(0, "BUTTON", "Clear",
			                                WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                                | BS_PUSHBUTTON,
			                                0, 0, 0, 0, h,
			                                (HMENU)(LONG_PTR)IDC_MV_CLEAR,
			                                GetModuleHandle(NULL), NULL);
			SendMessage(st->clear_btn, WM_SETFONT,
			            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                        WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                        | LVS_REPORT | LVS_OWNERDATA
			                        | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                        0, 0, 0, 0, h, (HMENU)(LONG_PTR)IDC_MV_LV,
			                        GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv,
			    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)"Address"; col.cx =  90; SendMessageA(st->lv, LVM_INSERTCOLUMNA, MVCOL_ADDR,  (LPARAM)&col);
			col.pszText = (LPSTR)"Hex";     col.cx = 400; SendMessageA(st->lv, LVM_INSERTCOLUMNA, MVCOL_HEX,   (LPARAM)&col);
			col.pszText = (LPSTR)"ASCII";   col.cx = 160; SendMessageA(st->lv, LVM_INSERTCOLUMNA, MVCOL_ASCII, (LPARAM)&col);

			PopulateCombo(st);
			PopulateZoneCombo(st);
			UpdateRowCount(st);
			DoResize(h, st);
			return 0;
		}

		case WM_SIZE:
		{
			DoResize(h, GetState(h));
			return 0;
		}

		case WM_COMMAND:
		{
			MemViewerState *st = GetState(h);
			if (!st) break;
			if (LOWORD(wp) == IDC_MV_COMBO && HIWORD(wp) == CBN_SELCHANGE)
			{
				int sel = (int)SendMessage(st->combo, CB_GETCURSEL, 0, 0);
				if (sel >= 0)
				{
					st->current_region = sel;
					UpdateRowCount(st);
					ListView_EnsureVisible(st->lv, 0, FALSE);
				}
				return 0;
			}
			if (LOWORD(wp) == IDC_MV_ZONE && HIWORD(wp) == CBN_SELCHANGE)
			{
				int sel = (int)SendMessage(st->zone_combo, CB_GETCURSEL, 0, 0);
				MemZone z = MemZone::None;
				if (sel == 1) z = MemZone::Reads;
				else if (sel == 2) z = MemZone::Writes;
				SetZone(st, z);
				return 0;
			}
			if (LOWORD(wp) == IDC_MV_CLEAR && HIWORD(wp) == BN_CLICKED)
			{
				MemHeatmap::Reset();
				InvalidateRect(st->lv, NULL, FALSE);
				return 0;
			}
			break;
		}

		case WM_NOTIFY:
		{
			NMHDR *hdr = (NMHDR *)lp;
			MemViewerState *st = GetState(h);
			if (!st || hdr->hwndFrom != st->lv) break;
			if (hdr->code == LVN_GETDISPINFOA)
			{
				OnGetDispInfo(st, (NMLVDISPINFOA *)lp);
				return 0;
			}
			if (hdr->code == NM_CUSTOMDRAW)
			{
				return HandleCustomDraw(st, (NMLVCUSTOMDRAW *)lp);
			}
			break;
		}

		case WM_CONTEXTMENU:
		{
			MemViewerState *st = GetState(h);
			if (!st || (HWND)wp != st->lv) break;

			int x = GET_X_LPARAM(lp);
			int y = GET_Y_LPARAM(lp);
			if (x == -1 && y == -1)   // keyboard (Shift+F10 / Menu key)
			{
				RECT rc; GetWindowRect(st->lv, &rc);
				x = rc.left + 16; y = rc.top + 16;
			}

			HMENU menu = CreatePopupMenu();
			AppendMenuA(menu, MF_STRING, IDM_MV_GOTO, "Go to Address...");
			const int cmd = (int)TrackPopupMenu(menu,
			    TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
			    x, y, 0, h, NULL);
			DestroyMenu(menu);

			if (cmd == IDM_MV_GOTO) DoGoToPrompt(h, st);
			return 0;
		}

		case WM_USER_DEBUGGER_REFRESH:
		{
			MemViewerState *st = GetState(h);
			if (st && st->lv) InvalidateRect(st->lv, NULL, FALSE);
			return 0;
		}

		case WM_CLOSE:
			DestroyWindow(h);
			return 0;

		case WM_DESTROY:
		{
			MemViewerState *st = GetState(h);
			if (st)
			{
				if (st->heatmap_acquired) MemHeatmap::Release();
				if (st->font) DeleteObject(st->font);
				if (g_snes_memview == h) g_snes_memview = NULL;
				if (g_gb_memview   == h) g_gb_memview   = NULL;
				delete st;
				SetWindowLongPtr(h, GWLP_USERDATA, 0);
			}
			return 0;
		}
	}
	return DefWindowProc(h, msg, wp, lp);
}

void MemoryViewerGlobalInit(HINSTANCE hInst)
{
	static bool done = false;
	if (done) return;
	done = true;

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = MemViewerWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = kClassName;
	wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassEx(&wc);
}

HWND OpenMemoryViewer(DbgSystem sys)
{
	MemoryViewerGlobalInit(GetModuleHandle(NULL));

	HWND *slot = (sys == DbgSystem::Snes) ? &g_snes_memview : &g_gb_memview;
	if (*slot && IsWindow(*slot))
	{
		ShowWindow(*slot, SW_SHOW);
		SetForegroundWindow(*slot);
		return *slot;
	}

	const TCHAR *title = (sys == DbgSystem::Snes) ? TEXT("Memory Viewer - SNES")
	                                              : TEXT("Memory Viewer - Game Boy");
	*slot = CreateWindowEx(0, kClassName, title,
	                       WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
	                       CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
	                       NULL, NULL, GetModuleHandle(NULL),
	                       (LPVOID)(intptr_t)sys);
	if (*slot)
	{
		ShowWindow(*slot, SW_SHOW);
		UpdateWindow(*slot);
	}
	return *slot;
}

void MemoryViewerRefreshAll()
{
	static int frame_tick = 0;
	if (++frame_tick >= 60)
	{
		frame_tick = 0;
		MemHeatmap::Decay();
	}
	if (g_snes_memview && IsWindow(g_snes_memview))
		PostMessage(g_snes_memview, WM_USER_DEBUGGER_REFRESH, 0, 0);
	if (g_gb_memview   && IsWindow(g_gb_memview))
		PostMessage(g_gb_memview,   WM_USER_DEBUGGER_REFRESH, 0, 0);
}
