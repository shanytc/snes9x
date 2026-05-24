#include "CDisasmPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../65c816.h"
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <tchar.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerDisasmPanel");

enum
{
	IDC_DISASM_LV = 7000,

	IDM_DISASM_TOGGLE_BP = 7100,
	IDM_DISASM_COPY_ADDR,
	IDM_DISASM_COPY_LINE,
	IDM_DISASM_MOVE_PC,
	IDM_DISASM_RUN_TO_LOCATION,
	IDM_DISASM_GO_TO_LOCATION,
	IDM_DISASM_GO_TO_PC
};

enum { COL_MARGIN = 0, COL_ADDR = 1, COL_BYTES = 2, COL_CODE = 3 };

struct DisasmLine
{
	uint32_t pc;
	uint8_t  length;
	bool     is_sub_start;
};

struct DisasmPanelState
{
	DbgSystem    sys              = DbgSystem::None;
	HFONT        font             = nullptr;
	HWND         lv               = nullptr;
	uint32_t     view_start_pc    = 0;
	bool         view_initialized = false;
	DisasmLine  *lines            = nullptr;
	int          line_count       = 0;
	int          line_cap         = 0;
	uint32_t     rclick_pc        = 0;
	bool         rclick_valid     = false;
};

static void EnsureCap(DisasmPanelState *st, int need)
{
	if (st->line_cap >= need) return;
	int new_cap = st->line_cap > 0 ? st->line_cap : 256;
	while (new_cap < need) new_cap *= 2;
	DisasmLine *new_arr = (DisasmLine *)malloc(sizeof(DisasmLine) * new_cap);
	if (st->lines && st->line_count > 0)
		memcpy(new_arr, st->lines, sizeof(DisasmLine) * st->line_count);
	free(st->lines);
	st->lines    = new_arr;
	st->line_cap = new_cap;
}

static void PushLine(DisasmPanelState *st, const DisasmLine &ln)
{
	EnsureCap(st, st->line_count + 1);
	st->lines[st->line_count++] = ln;
}

static void ClearLines(DisasmPanelState *st)
{
	st->line_count = 0;
}

static bool IsCtlFlowEnd(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x60 || op == 0x6B || op == 0x40
		    || op == 0x4C || op == 0x5C
		    || op == 0x82 || op == 0x80;
	return op == 0xC9 || op == 0xD9
	    || op == 0xC3 || op == 0xE9
	    || op == 0x18;
}

static DisasmPanelState *GetState(HWND h)
{
	return (DisasmPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static uint32_t GetCurrentPC(DbgSystem sys)
{
	if (sys == DbgSystem::Snes)
	{
		SnesStatus s = {};
		SnesBackend::GetStatus(&s);
		return ((uint32_t)s.prog_bank << 16) | s.PC;
	}
	GbStatus s = {};
	GbBackend::GetStatus(&s);
	return s.pc;
}

static uint32_t AdvancePC(DbgSystem sys, uint32_t pc, uint8_t len)
{
	if (sys == DbgSystem::Snes)
		return (pc & 0xFF0000) | ((uint16_t)(pc + len) & 0xFFFF);
	return (uint16_t)(pc + len);
}

static int DisasmOne(DbgSystem sys, uint32_t pc, char *line, size_t cap, uint8_t *out_bytes, int *out_byte_count)
{
	if (sys == DbgSystem::Snes)
	{
		DisasmResult65816 r = {};
		uint8_t len = SnesBackend::Disassemble(pc, &r);
		_snprintf_s(line, cap, _TRUNCATE, "%-4s %s", r.mnemonic, r.operand);
		for (int i = 0; i < len && i < 4; i++) out_bytes[i] = r.bytes[i];
		*out_byte_count = len;
		return len;
	}
	DisasmResultGb r = {};
	uint8_t len = GbBackend::Disassemble((uint16_t)pc, &r);
	_snprintf_s(line, cap, _TRUNCATE, "%-4s %s", r.mnemonic, r.operand);
	for (int i = 0; i < len && i < 3; i++) out_bytes[i] = r.bytes[i];
	*out_byte_count = len;
	return len;
}

static void ResetLines(DisasmPanelState *st, uint32_t new_start_pc)
{
	st->view_start_pc = new_start_pc;
	ClearLines(st);
	DisasmLine first{};
	first.pc           = new_start_pc;
	first.length       = 0;
	first.is_sub_start = false;
	PushLine(st, first);
	ListView_SetItemCountEx(st->lv, 100000, LVSICF_NOINVALIDATEALL);
	InvalidateRect(st->lv, NULL, FALSE);
}

static void EnsureLineCached(DisasmPanelState *st, int index)
{
	if (st->line_count == 0) return;
	while (st->line_count <= index)
	{
		const uint32_t back_pc     = st->lines[st->line_count - 1].pc;
		uint8_t        back_length = st->lines[st->line_count - 1].length;

		char ln[160]; uint8_t bytes[4]; int byte_count;
		int len = DisasmOne(st->sys, back_pc, ln, sizeof(ln), bytes, &byte_count);
		if (back_length == 0)
		{
			back_length = (uint8_t)len;
			st->lines[st->line_count - 1].length = back_length;
		}

		DisasmLine next{};
		next.pc           = AdvancePC(st->sys, back_pc, back_length);
		next.length       = 0;
		next.is_sub_start = IsCtlFlowEnd(st->sys, bytes[0]);
		PushLine(st, next);
	}
}

static int FindIndexForPC(DisasmPanelState *st, uint32_t pc)
{
	for (int i = 0; i < st->line_count; i++)
		if (st->lines[i].pc == pc) return i;
	return -1;
}

static void OnGetDispInfo(DisasmPanelState *st, NMLVDISPINFOA *di)
{
	const int idx = di->item.iItem;
	EnsureLineCached(st, idx);

	if (idx < 0 || idx >= st->line_count) return;

	DisasmLine &ln = st->lines[idx];
	if (ln.length == 0)
	{
		char buf[160]; uint8_t b[4]; int c;
		int len = DisasmOne(st->sys, ln.pc, buf, sizeof(buf), b, &c);
		ln.length = (uint8_t)len;
	}

	char line[160]; uint8_t bytes[4]; int byte_count;
	DisasmOne(st->sys, ln.pc, line, sizeof(line), bytes, &byte_count);

	if (di->item.mask & LVIF_TEXT)
	{
		char *out = di->item.pszText;
		const int cap = di->item.cchTextMax;
		switch (di->item.iSubItem)
		{
			case COL_MARGIN:
				out[0] = 0;
				break;
			case COL_ADDR:
				if (st->sys == DbgSystem::Snes)
					_snprintf_s(out, cap, _TRUNCATE, "%s%02X:%04X",
					            ln.is_sub_start ? "* " : "  ",
					            (unsigned)(ln.pc >> 16) & 0xFF,
					            (unsigned)(ln.pc & 0xFFFF));
				else
					_snprintf_s(out, cap, _TRUNCATE, "%s%04X",
					            ln.is_sub_start ? "* " : "  ",
					            (unsigned)(ln.pc & 0xFFFF));
				break;
			case COL_BYTES:
				if (byte_count == 1)      _snprintf_s(out, cap, _TRUNCATE, "%02X", bytes[0]);
				else if (byte_count == 2) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X", bytes[0], bytes[1]);
				else if (byte_count == 3) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X", bytes[0], bytes[1], bytes[2]);
				else                      _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X %02X", bytes[0], bytes[1], bytes[2], bytes[3]);
				break;
			case COL_CODE:
				_snprintf_s(out, cap, _TRUNCATE, "%s", line);
				break;
		}
	}
}

static LRESULT OnCustomDraw(DisasmPanelState *st, NMLVCUSTOMDRAW *cd)
{
	switch (cd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
			return CDRF_NOTIFYITEMDRAW;

		case CDDS_ITEMPREPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx < st->line_count)
			{
				const uint32_t row_pc = st->lines[idx].pc;
				const uint32_t cur_pc = GetCurrentPC(st->sys);
				if (row_pc == cur_pc)
				{
					cd->clrTextBk = RGB(255, 255, 180);
					cd->clrText   = RGB(0, 0, 0);
				}
				else if (st->lines[idx].is_sub_start)
				{
					cd->clrText = RGB(0, 96, 0);
				}
			}
			return CDRF_NOTIFYPOSTPAINT | CDRF_NEWFONT;
		}

		case CDDS_ITEMPOSTPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx >= st->line_count) return CDRF_DODEFAULT;

			RECT row_rect = {};
			ListView_GetItemRect(st->lv, idx, &row_rect, LVIR_BOUNDS);
			const int margin_w = ListView_GetColumnWidth(st->lv, COL_MARGIN);
			RECT mr = { row_rect.left, row_rect.top,
			            row_rect.left + margin_w, row_rect.bottom };

			const uint32_t row_pc = st->lines[idx].pc;
			const uint32_t cur_pc = GetCurrentPC(st->sys);

			bool has_bp = false;
			if (st->sys == DbgSystem::Snes)
				has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Snes,
				                                     (uint8_t)(row_pc >> 16),
				                                     (uint16_t)(row_pc & 0xFFFF));
			else
				has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Gb, 0, (uint16_t)row_pc);

			HDC dc = cd->nmcd.hdc;
			if (has_bp)
			{
				HBRUSH red = CreateSolidBrush(RGB(200, 0, 0));
				RECT dot = { mr.left + 4, mr.top + 3, mr.left + 14, mr.top + 13 };
				FillRect(dc, &dot, red);
				DeleteObject(red);
			}
			if (row_pc == cur_pc)
			{
				HBRUSH yel = CreateSolidBrush(RGB(220, 200, 0));
				const int cy = (mr.top + mr.bottom) / 2;
				POINT arrow[3] = {
					{ mr.left + 14, mr.top + 2 },
					{ mr.right - 3, cy },
					{ mr.left + 14, mr.bottom - 2 }
				};
				HRGN hr = CreatePolygonRgn(arrow, 3, ALTERNATE);
				FillRgn(dc, hr, yel);
				DeleteObject(hr);
				DeleteObject(yel);
			}
			return CDRF_DODEFAULT;
		}
	}
	return CDRF_DODEFAULT;
}

static HMENU BuildContextMenu(uint32_t pc, DbgSystem sys)
{
	char buf[64];
	HMENU m = CreatePopupMenu();

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%02X:%04X)\tF9",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%04X)\tF9",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_TOGGLE_BP, buf);

	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_ADDR, "Copy Address");
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_LINE, "Copy Selected\tCtrl+C");
	AppendMenuA(m, MF_SEPARATOR, 0, NULL);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%02X:%04X)",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%04X)",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_MOVE_PC, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%02X:%04X)\tCtrl+F11",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%04X)\tCtrl+F11",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_RUN_TO_LOCATION, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%02X:%04X)",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%04X)",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_LOCATION, buf);

	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_PC, "Go to current PC");

	return m;
}

static void CopySelectionToClipboard(HWND parent, DisasmPanelState *st)
{
	std::string out;
	int idx = -1;
	while ((idx = ListView_GetNextItem(st->lv, idx, LVNI_SELECTED)) != -1)
	{
		EnsureLineCached(st, idx);
		if (idx >= st->line_count) break;
		const DisasmLine &ln = st->lines[idx];
		char line[160]; uint8_t bytes[4]; int byte_count;
		DisasmOne(st->sys, ln.pc, line, sizeof(line), bytes, &byte_count);

		char addr[16];
		if (st->sys == DbgSystem::Snes)
			_snprintf_s(addr, 16, _TRUNCATE, "%02X:%04X",
			            (unsigned)(ln.pc >> 16) & 0xFF, (unsigned)(ln.pc & 0xFFFF));
		else
			_snprintf_s(addr, 16, _TRUNCATE, "%04X", (unsigned)(ln.pc & 0xFFFF));

		char row[256];
		_snprintf_s(row, 256, _TRUNCATE, "%s  %s\r\n", addr, line);
		out += row;
	}
	if (out.empty()) return;

	if (!OpenClipboard(parent)) return;
	EmptyClipboard();
	const size_t n = out.size();
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n + 1);
	if (g)
	{
		char *p = (char *)GlobalLock(g);
		memcpy(p, out.c_str(), n + 1);
		GlobalUnlock(g);
		SetClipboardData(CF_TEXT, g);
	}
	CloseClipboard();
}

static void OnContextMenuCommand(HWND parent, DisasmPanelState *st, int cmd)
{
	if (!st->rclick_valid && cmd != IDM_DISASM_COPY_LINE && cmd != IDM_DISASM_GO_TO_PC)
		return;
	const uint32_t pc = st->rclick_pc;
	const uint8_t  bank = (st->sys == DbgSystem::Snes) ? (uint8_t)(pc >> 16) : (uint8_t)0;
	const uint16_t addr = (uint16_t)(pc & 0xFFFF);

	switch (cmd)
	{
		case IDM_DISASM_TOGGLE_BP:
			gDebugger.ToggleExecBreakpoint(st->sys, bank, addr);
			InvalidateRect(st->lv, NULL, FALSE);
			break;
		case IDM_DISASM_COPY_ADDR:
		{
			char buf[16];
			if (st->sys == DbgSystem::Snes)
				_snprintf_s(buf, 16, _TRUNCATE, "%02X:%04X", bank, addr);
			else
				_snprintf_s(buf, 16, _TRUNCATE, "%04X", addr);
			if (OpenClipboard(parent))
			{
				EmptyClipboard();
				HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, strlen(buf) + 1);
				if (g)
				{
					char *p = (char *)GlobalLock(g);
					strcpy(p, buf);
					GlobalUnlock(g);
					SetClipboardData(CF_TEXT, g);
				}
				CloseClipboard();
			}
			break;
		}
		case IDM_DISASM_COPY_LINE:
			CopySelectionToClipboard(parent, st);
			break;
		case IDM_DISASM_MOVE_PC:
			if (st->sys == DbgSystem::Snes)
			{
				Registers.PB  = bank;
				Registers.PCw = addr;
				S9xDebuggerRefreshAll();
			}
			break;
		case IDM_DISASM_RUN_TO_LOCATION:
			gDebugger.AddExecBreakpoint(st->sys, bank, addr);
			gDebugger.Run();
			break;
		case IDM_DISASM_GO_TO_LOCATION:
			ResetLines(st, pc);
			break;
		case IDM_DISASM_GO_TO_PC:
		{
			const uint32_t cur = GetCurrentPC(st->sys);
			ResetLines(st, cur);
			st->view_initialized = true;
			break;
		}
	}
}

static void OnRightClick(HWND parent, DisasmPanelState *st)
{
	LVHITTESTINFO hti = {};
	DWORD pos = GetMessagePos();
	POINT pt = { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
	hti.pt = pt;
	ScreenToClient(st->lv, &hti.pt);
	int idx = ListView_HitTest(st->lv, &hti);

	if (idx >= 0 && idx < st->line_count)
	{
		st->rclick_pc    = st->lines[idx].pc;
		st->rclick_valid = true;

		if (ListView_GetItemState(st->lv, idx, LVIS_SELECTED) == 0)
		{
			ListView_SetItemState(st->lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_SetItemState(st->lv, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		}
	}
	else
	{
		st->rclick_valid = false;
		st->rclick_pc    = 0;
	}

	HMENU menu = BuildContextMenu(st->rclick_pc, st->sys);
	int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
	                              pt.x, pt.y, 0, parent, NULL);
	DestroyMenu(menu);
	if (cmd) OnContextMenuCommand(parent, st, cmd);
}

static void EnsurePCVisible(DisasmPanelState *st)
{
	const uint32_t cur_pc = GetCurrentPC(st->sys);

	if (!st->view_initialized)
	{
		ResetLines(st, cur_pc);
		st->view_initialized = true;
		ListView_EnsureVisible(st->lv, 0, FALSE);
		return;
	}

	int top = ListView_GetTopIndex(st->lv);
	int per_page = ListView_GetCountPerPage(st->lv);
	int bottom = top + per_page;

	EnsureLineCached(st, bottom + 8);

	const int idx = FindIndexForPC(st, cur_pc);
	if (idx < 0 || idx < top || idx > bottom + 4)
	{
		ResetLines(st, cur_pc);
		ListView_EnsureVisible(st->lv, 0, FALSE);
		return;
	}

	if (idx >= top && idx <= bottom)
		return;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			DisasmPanelState *st = new DisasmPanelState();
			st->sys = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			EnsureCap(st, 256);
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			DisasmPanelState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                       WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                       | LVS_REPORT | LVS_OWNERDATA
			                       | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                       0, 0, 0, 0,
			                       h, (HMENU)(LONG_PTR)IDC_DISASM_LV,
			                       GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv,
			    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)" ";        col.cx = 32;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_MARGIN, (LPARAM)&col);
			col.pszText = (LPSTR)"Address";  col.cx = 70;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_ADDR,   (LPARAM)&col);
			col.pszText = (LPSTR)"Bytes";    col.cx = 95;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_BYTES,  (LPARAM)&col);
			col.pszText = (LPSTR)"Code";     col.cx = 320; SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_CODE,   (LPARAM)&col);

			ListView_SetItemCountEx(st->lv, 0, LVSICF_NOINVALIDATEALL);
			return 0;
		}

		case WM_DESTROY:
		{
			DisasmPanelState *st = GetState(h);
			if (st)
			{
				if (st->font) DeleteObject(st->font);
				free(st->lines);
				delete st;
			}
			return 0;
		}

		case WM_SIZE:
		{
			DisasmPanelState *st = GetState(h);
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
			DisasmPanelState *st = GetState(h);
			if (!st || hdr->hwndFrom != st->lv) break;

			switch (hdr->code)
			{
				case LVN_GETDISPINFOA:
					OnGetDispInfo(st, (NMLVDISPINFOA *)lp);
					return 0;
				case NM_CUSTOMDRAW:
					return OnCustomDraw(st, (NMLVCUSTOMDRAW *)lp);
				case NM_RCLICK:
					OnRightClick(h, st);
					return 0;
				case NM_DBLCLK:
				{
					NMITEMACTIVATE *act = (NMITEMACTIVATE *)lp;
					if (act->iItem >= 0 && act->iItem < st->line_count)
					{
						const uint32_t pc = st->lines[act->iItem].pc;
						const uint8_t  b  = (st->sys == DbgSystem::Snes) ? (uint8_t)(pc >> 16) : (uint8_t)0;
						const uint16_t a  = (uint16_t)(pc & 0xFFFF);
						gDebugger.ToggleExecBreakpoint(st->sys, b, a);
						InvalidateRect(st->lv, NULL, FALSE);
					}
					return 0;
				}
			}
			break;
		}
	}

	return DefWindowProc(h, msg, wp, lp);
}

void DisasmPanelRegisterClass(HINSTANCE hInst)
{
	static bool done = false;
	if (done) return;
	done = true;

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND DisasmPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void DisasmPanelRefresh(HWND h)
{
	DisasmPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	EnsurePCVisible(st);
	InvalidateRect(st->lv, NULL, FALSE);
}
