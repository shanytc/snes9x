#include "CMemoryViewer.h"
#include "CMemoryRegions.h"
#include "../snes9x.h"
#include <commctrl.h>
#include <windowsx.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerMemoryViewer");

enum
{
	IDC_MV_COMBO = 8000,
	IDC_MV_LV    = 8001
};

enum { MVCOL_ADDR = 0, MVCOL_HEX = 1, MVCOL_ASCII = 2 };

struct MemViewerState
{
	DbgSystem sys           = DbgSystem::None;
	HFONT     font          = nullptr;
	HWND      combo         = nullptr;
	HWND      lv            = nullptr;
	int       current_region = 0;
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

static void DoResize(HWND hwnd, MemViewerState *st)
{
	if (!st) return;
	RECT rc;
	GetClientRect(hwnd, &rc);
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;

	const int top_h   = 28;
	const int padding = 4;
	if (st->combo) MoveWindow(st->combo, padding, padding, 200, 200, TRUE);
	if (st->lv)    MoveWindow(st->lv,    0, top_h, w, h - top_h, TRUE);
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
			break;
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
	if (g_snes_memview && IsWindow(g_snes_memview))
		PostMessage(g_snes_memview, WM_USER_DEBUGGER_REFRESH, 0, 0);
	if (g_gb_memview   && IsWindow(g_gb_memview))
		PostMessage(g_gb_memview,   WM_USER_DEBUGGER_REFRESH, 0, 0);
}
