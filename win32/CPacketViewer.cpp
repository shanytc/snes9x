#include "CPacketViewer.h"
#include "CDebugger.h"      // WM_USER_DEBUGGER_REFRESH
#include "../sgb/sgb.h"
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerPacketViewer");

enum { IDC_PV_LV = 8200 };
enum { PVCOL_SEQ = 0, PVCOL_TCYC = 1, PVCOL_CMD = 2, PVCOL_LEN = 3, PVCOL_BYTES = 4 };

struct PacketViewerState
{
	HFONT             font  = nullptr;
	HWND              lv    = nullptr;
	SgbPacketLogEntry cache[128];
	uint32_t          cache_count = 0;
	uint32_t          shown_newest_seq = 0xFFFFFFFFu;
};

static HWND g_packet_view = NULL;

static PacketViewerState *GetState(HWND h)
{
	return (PacketViewerState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static const char *SgbCmdName(int cmd)
{
	switch (cmd)
	{
		case 0x00: return "PAL01";    case 0x01: return "PAL23";
		case 0x02: return "PAL03";    case 0x03: return "PAL12";
		case 0x04: return "ATTR_BLK"; case 0x05: return "ATTR_LIN";
		case 0x06: return "ATTR_DIV"; case 0x07: return "ATTR_CHR";
		case 0x08: return "SOUND";    case 0x09: return "SOU_TRN";
		case 0x0A: return "PAL_SET";  case 0x0B: return "PAL_TRN";
		case 0x0C: return "ATRC_EN";  case 0x0D: return "TEST_EN";
		case 0x0E: return "ICON_EN";  case 0x0F: return "DATA_SND";
		case 0x10: return "DATA_TRN"; case 0x11: return "MLT_REQ";
		case 0x12: return "JUMP";     case 0x13: return "CHR_TRN";
		case 0x14: return "PCT_TRN";  case 0x15: return "ATTR_TRN";
		case 0x16: return "ATTR_SET"; case 0x17: return "MASK_EN";
		case 0x18: return "OBJ_TRN";  case 0x19: return "PAL_PRI";
		default:   return "?";
	}
}

// Re-pull the log; only rebuild/scroll when a new packet has actually arrived
// so the user can scroll back through history without it snapping to the end.
static void Refresh(PacketViewerState *st)
{
	const uint32_t n      = S9xSGBGetPacketLog(st->cache, 128);
	const uint32_t newest = n ? st->cache[n - 1].seq : 0xFFFFFFFFu;
	const bool changed = (n != st->cache_count) || (newest != st->shown_newest_seq);
	st->cache_count = n;
	if (!changed) return;
	ListView_SetItemCountEx(st->lv, (int)n, LVSICF_NOINVALIDATEALL);
	if (n) ListView_EnsureVisible(st->lv, (int)n - 1, FALSE);   // follow newest
	InvalidateRect(st->lv, NULL, FALSE);
	st->shown_newest_seq = newest;
}

static void OnGetDispInfo(PacketViewerState *st, NMLVDISPINFOA *di)
{
	if (!(di->item.mask & LVIF_TEXT)) return;
	char *out = di->item.pszText;
	const int cap = di->item.cchTextMax;
	out[0] = 0;

	const int idx = di->item.iItem;
	if (idx < 0 || (uint32_t)idx >= st->cache_count) return;

	const SgbPacketLogEntry &e = st->cache[idx];
	const int cmd = e.bytes[0] >> 3;
	const int len = (e.bytes[0] & 0x07) ? (e.bytes[0] & 0x07) : 1;

	switch (di->item.iSubItem)
	{
		case PVCOL_SEQ:
			_snprintf_s(out, cap, _TRUNCATE, "%u", e.seq);
			break;
		case PVCOL_TCYC:
			_snprintf_s(out, cap, _TRUNCATE, "%llu", (unsigned long long)e.t_cycle);
			break;
		case PVCOL_CMD:
			_snprintf_s(out, cap, _TRUNCATE, "%02X %s", cmd, SgbCmdName(cmd));
			break;
		case PVCOL_LEN:
			_snprintf_s(out, cap, _TRUNCATE, "%d", len);
			break;
		case PVCOL_BYTES:
		{
			char *p = out; int rem = cap;
			for (int i = 0; i < 16 && rem > 3; i++)
			{
				int w = _snprintf_s(p, rem, _TRUNCATE, (i == 0) ? "%02X" : " %02X",
				                    e.bytes[i]);
				if (w < 0) break;
				p += w; rem -= w;
			}
			break;
		}
	}
}

static LRESULT CALLBACK PvWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			PacketViewerState *st = new PacketViewerState();
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                      DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                      ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			PacketViewerState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                        WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                        | LVS_REPORT | LVS_OWNERDATA
			                        | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                        0, 0, 0, 0, h, (HMENU)(LONG_PTR)IDC_PV_LV,
			                        GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv,
			    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)"#";       col.cx =  60; SendMessageA(st->lv, LVM_INSERTCOLUMNA, PVCOL_SEQ,   (LPARAM)&col);
			col.pszText = (LPSTR)"T-Cycle"; col.cx = 110; SendMessageA(st->lv, LVM_INSERTCOLUMNA, PVCOL_TCYC,  (LPARAM)&col);
			col.pszText = (LPSTR)"Command"; col.cx = 120; SendMessageA(st->lv, LVM_INSERTCOLUMNA, PVCOL_CMD,   (LPARAM)&col);
			col.pszText = (LPSTR)"Pkts";    col.cx =  44; SendMessageA(st->lv, LVM_INSERTCOLUMNA, PVCOL_LEN,   (LPARAM)&col);
			col.pszText = (LPSTR)"Bytes";   col.cx = 360; SendMessageA(st->lv, LVM_INSERTCOLUMNA, PVCOL_BYTES, (LPARAM)&col);

			Refresh(st);
			return 0;
		}

		case WM_SIZE:
		{
			PacketViewerState *st = GetState(h);
			if (st && st->lv)
			{
				RECT rc; GetClientRect(h, &rc);
				MoveWindow(st->lv, 0, 0, rc.right, rc.bottom, TRUE);
			}
			return 0;
		}

		case WM_NOTIFY:
		{
			NMHDR *hdr = (NMHDR *)lp;
			PacketViewerState *st = GetState(h);
			if (st && hdr->hwndFrom == st->lv && hdr->code == LVN_GETDISPINFOA)
			{
				OnGetDispInfo(st, (NMLVDISPINFOA *)lp);
				return 0;
			}
			break;
		}

		case WM_USER_DEBUGGER_REFRESH:
		{
			PacketViewerState *st = GetState(h);
			if (st && st->lv) Refresh(st);
			return 0;
		}

		case WM_CLOSE:
			DestroyWindow(h);
			return 0;

		case WM_DESTROY:
		{
			PacketViewerState *st = GetState(h);
			if (st)
			{
				if (st->font) DeleteObject(st->font);
				if (g_packet_view == h) g_packet_view = NULL;
				delete st;
				SetWindowLongPtr(h, GWLP_USERDATA, 0);
			}
			return 0;
		}
	}
	return DefWindowProc(h, msg, wp, lp);
}

void PacketViewerGlobalInit(HINSTANCE hInst)
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
	wc.lpfnWndProc   = PvWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = kClassName;
	wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassEx(&wc);
}

HWND OpenPacketViewer(void)
{
	PacketViewerGlobalInit(GetModuleHandle(NULL));

	if (g_packet_view && IsWindow(g_packet_view))
	{
		ShowWindow(g_packet_view, SW_SHOW);
		SetForegroundWindow(g_packet_view);
		return g_packet_view;
	}

	g_packet_view = CreateWindowEx(0, kClassName, TEXT("SGB Packet Trace"),
	                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
	                               CW_USEDEFAULT, CW_USEDEFAULT, 720, 480,
	                               NULL, NULL, GetModuleHandle(NULL), NULL);
	if (g_packet_view)
	{
		ShowWindow(g_packet_view, SW_SHOW);
		UpdateWindow(g_packet_view);
	}
	return g_packet_view;
}

void PacketViewerRefreshAll(void)
{
	if (g_packet_view && IsWindow(g_packet_view))
		PostMessage(g_packet_view, WM_USER_DEBUGGER_REFRESH, 0, 0);
}
