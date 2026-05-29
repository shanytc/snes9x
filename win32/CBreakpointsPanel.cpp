#include "CBreakpointsPanel.h"
#include "rsrc/resource.h"
#include <commctrl.h>
#include <windowsx.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void NotifyRefresh(HWND h)
{
	HWND root = h ? GetAncestor(h, GA_ROOT) : NULL;
	if (root) PostMessage(root, WM_USER_DEBUGGER_REFRESH, 0, 0);
}

static const TCHAR *kClassName = TEXT("S9xDebuggerBreakpointsPanel");

enum
{
	IDC_BP_LV = 7400,

	IDM_BP_ADD = 7500,
	IDM_BP_EDIT,
	IDM_BP_DELETE,
	IDM_BP_TOGGLE,
	IDM_BP_DELETE_ALL
};

enum { BPCOL_ENABLED = 0, BPCOL_TYPE = 1, BPCOL_ADDR = 2, BPCOL_COND = 3 };

struct BpPanelState
{
	DbgSystem sys              = DbgSystem::None;
	HFONT     font             = nullptr;
	HWND      lv               = nullptr;
	uint32_t  shown_version    = 0;
};

static BpPanelState *GetState(HWND h)
{
	return (BpPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static const char *MemTypeName(int mt)
{
	switch (mt)
	{
		case DBG_MEM_CPU:       return "CPU Memory";
		case DBG_MEM_PRG_ROM:   return "PRG ROM";
		case DBG_MEM_WORK_RAM:  return "Work RAM";
		case DBG_MEM_VIDEO_RAM: return "Video RAM";
		case DBG_MEM_OAM:       return "OAM (Sprites)";
		case DBG_MEM_CG_RAM:    return "CG RAM (Palette)";
		case DBG_MEM_REGISTER:  return "Register";
		case DBG_MEM_SRAM:      return "SRAM";
	}
	return "?";
}

static void TypeString(const DbgBreakpoint &bp, char *out, size_t cap)
{
	char modes[8] = "";
	int n = 0;
	if (bp.brk_execute) modes[n++] = 'X';
	if (bp.brk_read)    modes[n++] = 'R';
	if (bp.brk_write)   modes[n++] = 'W';
	modes[n] = 0;
	if (n == 0) strcpy(modes, "-");
	_snprintf_s(out, cap, _TRUNCATE, "%s %s", modes, MemTypeName(bp.memory_type));
}

static void PopulateList(BpPanelState *st)
{
	const size_t n = gDebugger.BreakpointCount();
	ListView_DeleteAllItems(st->lv);

	for (size_t i = 0; i < n; i++)
	{
		const DbgBreakpoint *bp = gDebugger.GetBreakpoint(i);
		if (!bp) continue;

		LVITEMA item = {};
		item.mask     = LVIF_TEXT | LVIF_PARAM;
		item.iItem    = (int)i;
		item.iSubItem = 0;
		item.pszText  = (LPSTR)(bp->enabled ? "[x]" : "[ ]");
		item.lParam   = (LPARAM)i;
		SendMessageA(st->lv, LVM_INSERTITEMA, 0, (LPARAM)&item);

		char type_buf[48];
		TypeString(*bp, type_buf, sizeof(type_buf));
		item.mask     = LVIF_TEXT;
		item.iSubItem = BPCOL_TYPE;
		item.pszText  = type_buf;
		SendMessageA(st->lv, LVM_SETITEMA, 0, (LPARAM)&item);

		char addr_buf[24];
		_snprintf_s(addr_buf, 24, _TRUNCATE, "%02X:%04X",
		            (unsigned)bp->bank & 0xFF, (unsigned)bp->address & 0xFFFF);
		item.iSubItem = BPCOL_ADDR;
		item.pszText  = addr_buf;
		SendMessageA(st->lv, LVM_SETITEMA, 0, (LPARAM)&item);

		item.iSubItem = BPCOL_COND;
		item.pszText  = (LPSTR)bp->condition;
		SendMessageA(st->lv, LVM_SETITEMA, 0, (LPARAM)&item);
	}

	st->shown_version = gDebugger.BreakpointsVersion();
}

static int GetSelectedIndex(HWND lv)
{
	return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void OnAdd(HWND parent, BpPanelState *st)
{
	DbgBreakpoint bp{};
	bp.system      = st->sys;
	bp.enabled     = true;
	bp.brk_execute = true;
	bp.memory_type = DBG_MEM_CPU;
	bp.break_exec  = true;
	if (ShowEditBreakpointDialog(parent, st->sys, &bp))
	{
		gDebugger.AddBreakpoint(bp);
		PopulateList(st);
		NotifyRefresh(parent);
	}
}

static void OnEdit(HWND parent, BpPanelState *st)
{
	const int idx = GetSelectedIndex(st->lv);
	if (idx < 0) return;
	const DbgBreakpoint *src = gDebugger.GetBreakpoint((size_t)idx);
	if (!src) return;
	DbgBreakpoint bp = *src;
	if (ShowEditBreakpointDialog(parent, st->sys, &bp))
	{
		gDebugger.UpdateBreakpoint((size_t)idx, bp);
		PopulateList(st);
		NotifyRefresh(parent);
	}
}

static void OnDelete(HWND parent, BpPanelState *st)
{
	int idx;
	bool any = false;
	while ((idx = GetSelectedIndex(st->lv)) >= 0)
	{
		gDebugger.RemoveBreakpointAt((size_t)idx);
		PopulateList(st);
		any = true;
	}
	if (any) NotifyRefresh(parent);
}

static void OnToggleEnabled(HWND parent, BpPanelState *st)
{
	int idx = -1;
	bool any = false;
	while ((idx = ListView_GetNextItem(st->lv, idx, LVNI_SELECTED)) >= 0)
	{
		gDebugger.ToggleBreakpointEnabledAt((size_t)idx);
		any = true;
	}
	PopulateList(st);
	if (any) NotifyRefresh(parent);
}

static void OnDeleteAll(HWND parent, BpPanelState *st)
{
	const bool any = gDebugger.BreakpointCount() > 0;
	while (gDebugger.BreakpointCount() > 0)
		gDebugger.RemoveBreakpointAt(0);
	PopulateList(st);
	if (any) NotifyRefresh(parent);
}

static void ShowContextMenu(HWND parent, BpPanelState *st)
{
	HMENU m = CreatePopupMenu();
	const bool has_sel = GetSelectedIndex(st->lv) >= 0;
	AppendMenuA(m, MF_STRING, IDM_BP_ADD, "&Add...");
	AppendMenuA(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), IDM_BP_EDIT, "&Edit...");
	AppendMenuA(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), IDM_BP_TOGGLE, "&Toggle enabled");
	AppendMenuA(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), IDM_BP_DELETE, "&Delete");
	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_BP_DELETE_ALL, "Delete &all");

	DWORD pos = GetMessagePos();
	int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
	                               GET_X_LPARAM(pos), GET_Y_LPARAM(pos),
	                               0, parent, NULL);
	DestroyMenu(m);

	switch (cmd)
	{
		case IDM_BP_ADD:        OnAdd(parent, st); break;
		case IDM_BP_EDIT:       OnEdit(parent, st); break;
		case IDM_BP_TOGGLE:     OnToggleEnabled(parent, st); break;
		case IDM_BP_DELETE:     OnDelete(parent, st); break;
		case IDM_BP_DELETE_ALL: OnDeleteAll(parent, st); break;
	}
}

static LRESULT CALLBACK BpWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			BpPanelState *st = new BpPanelState();
			st->sys  = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			BpPanelState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                        WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                        | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                        0, 0, 0, 0, h, (HMENU)(LONG_PTR)IDC_BP_LV,
			                        GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)"En";       col.cx = 30;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, BPCOL_ENABLED, (LPARAM)&col);
			col.pszText = (LPSTR)"Type";     col.cx = 120; SendMessageA(st->lv, LVM_INSERTCOLUMNA, BPCOL_TYPE,    (LPARAM)&col);
			col.pszText = (LPSTR)"Address";  col.cx = 70;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, BPCOL_ADDR,    (LPARAM)&col);
			col.pszText = (LPSTR)"Condition";col.cx = 180; SendMessageA(st->lv, LVM_INSERTCOLUMNA, BPCOL_COND,    (LPARAM)&col);

			PopulateList(st);
			return 0;
		}

		case WM_DESTROY:
		{
			BpPanelState *st = GetState(h);
			if (st) delete st;
			return 0;
		}

		case WM_SIZE:
		{
			BpPanelState *st = GetState(h);
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
			BpPanelState *st = GetState(h);
			if (!st || hdr->hwndFrom != st->lv) break;
			switch (hdr->code)
			{
				case NM_DBLCLK:   OnEdit(h, st);           return 0;
				case NM_RCLICK:   ShowContextMenu(h, st);  return 0;
				case LVN_KEYDOWN:
				{
					NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
					if      (kd->wVKey == VK_DELETE) OnDelete(h, st);
					else if (kd->wVKey == VK_SPACE)  OnToggleEnabled(h, st);
					else if (kd->wVKey == VK_INSERT) OnAdd(h, st);
					return 0;
				}
			}
			break;
		}
	}
	return DefWindowProc(h, msg, wp, lp);
}

void BreakpointsPanelRegisterClass(HINSTANCE hInst)
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
	wc.lpfnWndProc   = BpWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND BreakpointsPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                     0, 0, 0, 0, parent, (HMENU)(LONG_PTR)id,
	                     GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void BreakpointsPanelRefresh(HWND h)
{
	BpPanelState *st = GetState(h);
	if (!st || !st->lv) return;
	if (st->shown_version == gDebugger.BreakpointsVersion()) return;
	PopulateList(st);
}

struct EditDlgState
{
	DbgSystem      sys;
	DbgBreakpoint *bp;
	bool           accepted;
};

struct MemTypeEntry { const char *name; int memtype; };

static const MemTypeEntry kMemTypesSnes[] =
{
	{ "CPU Memory",        DBG_MEM_CPU       },
	{ "PRG ROM",           DBG_MEM_PRG_ROM   },
	{ "Work RAM",          DBG_MEM_WORK_RAM  },
	{ "Video RAM",         DBG_MEM_VIDEO_RAM },
	{ "OAM (Sprites)",     DBG_MEM_OAM       },
	{ "CG RAM (Palette)",  DBG_MEM_CG_RAM    },
	{ "Register",          DBG_MEM_REGISTER  },
	{ "SRAM",              DBG_MEM_SRAM      }
};

static const MemTypeEntry kMemTypesGb[] =
{
	{ "GB - CPU Memory",     DBG_MEM_CPU       },
	{ "GB - PRG ROM",        DBG_MEM_PRG_ROM   },
	{ "GB - Work RAM",       DBG_MEM_WORK_RAM  },
	{ "GB - High RAM",       DBG_MEM_HIGH_RAM  },
	{ "GB - Boot ROM",       DBG_MEM_BOOT_ROM  },
	{ "GB - Video RAM",      DBG_MEM_VIDEO_RAM },
	{ "GB - Sprite RAM",     DBG_MEM_OAM       },
	{ "GB - I/O Registers",  DBG_MEM_REGISTER  },
	{ "GB - SRAM",           DBG_MEM_SRAM      }
};

static bool MemTypeRange(DbgSystem sys, int memtype, uint32_t *begin, uint32_t *end)
{
	if (sys == DbgSystem::Gb)
	{
		switch (memtype)
		{
			case DBG_MEM_CPU:       *begin = 0x0000; *end = 0xFFFF; return true;
			case DBG_MEM_PRG_ROM:   *begin = 0x0000; *end = 0x7FFF; return true;
			case DBG_MEM_VIDEO_RAM: *begin = 0x8000; *end = 0x9FFF; return true;
			case DBG_MEM_SRAM:      *begin = 0xA000; *end = 0xBFFF; return true;
			case DBG_MEM_WORK_RAM:  *begin = 0xC000; *end = 0xDFFF; return true;
			case DBG_MEM_OAM:       *begin = 0xFE00; *end = 0xFE9F; return true;
			case DBG_MEM_REGISTER:  *begin = 0xFF00; *end = 0xFF7F; return true;
			case DBG_MEM_HIGH_RAM:  *begin = 0xFF80; *end = 0xFFFE; return true;
			case DBG_MEM_BOOT_ROM:  *begin = 0x0000; *end = 0x00FF; return true;
		}
		return false;
	}

	switch (memtype)
	{
		case DBG_MEM_CPU:       *begin = 0x000000; *end = 0xFFFFFF; return true;
		case DBG_MEM_PRG_ROM:   *begin = 0x000000; *end = 0xFFFFFF; return true;
		case DBG_MEM_WORK_RAM:  *begin = 0x7E0000; *end = 0x7FFFFF; return true;
		case DBG_MEM_VIDEO_RAM: *begin = 0x000000; *end = 0x00FFFF; return true;
		case DBG_MEM_OAM:       *begin = 0x000000; *end = 0x00021F; return true;
		case DBG_MEM_CG_RAM:    *begin = 0x000000; *end = 0x0001FF; return true;
		case DBG_MEM_REGISTER:  *begin = 0x002100; *end = 0x0043FF; return true;
	}
	return false;
}

static void UpdateRangeLabel(HWND dlg, DbgSystem sys, int memtype)
{
	uint32_t b = 0, e = 0;
	char buf[48];
	if (MemTypeRange(sys, memtype, &b, &e))
	{
		if (sys == DbgSystem::Snes)
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "($%06X - $%06X)", b, e);
		else
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "($%04X - $%04X)", b, e);
	}
	else
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		            sys == DbgSystem::Snes ? "(Max: $FFFFFF)" : "(Max: $FFFF)");
	}
	SetDlgItemTextA(dlg, IDC_DBG_BP_MAX, buf);
}

static uint32_t ParseHex(const char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '$') s++;
	else if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	uint32_t v = 0;
	while (*s)
	{
		const char c = *s++;
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
		else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
		else break;
		v = (v << 4) | (uint32_t)d;
	}
	return v;
}

static INT_PTR CALLBACK EditDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			EditDlgState *st = (EditDlgState *)lp;
			SetWindowLongPtr(dlg, DWLP_USER, (LONG_PTR)st);

			HWND combo = GetDlgItem(dlg, IDC_DBG_BP_TYPE);
			const MemTypeEntry *types;
			int n;
			if (st->sys == DbgSystem::Snes)
			{
				types = kMemTypesSnes;
				n     = (int)(sizeof(kMemTypesSnes)/sizeof(kMemTypesSnes[0]));
			}
			else
			{
				types = kMemTypesGb;
				n     = (int)(sizeof(kMemTypesGb)/sizeof(kMemTypesGb[0]));
			}
			int sel_idx = 0;
			for (int i = 0; i < n; i++)
			{
				int idx = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)types[i].name);
				SendMessage(combo, CB_SETITEMDATA, idx, (LPARAM)types[i].memtype);
				if (types[i].memtype == st->bp->memory_type) sel_idx = idx;
			}
			SendMessage(combo, CB_SETCURSEL, sel_idx, 0);

			UpdateRangeLabel(dlg, st->sys, st->bp->memory_type);

			CheckDlgButton(dlg, IDC_DBG_BP_EXEC,      st->bp->brk_execute ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_DBG_BP_READ,      st->bp->brk_read    ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_DBG_BP_WRITE,     st->bp->brk_write   ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_DBG_BP_MARK,      st->bp->mark_event  ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_DBG_BP_BREAKEXEC, st->bp->break_exec  ? BST_CHECKED : BST_UNCHECKED);

			char addr_buf[24];
			if (st->sys == DbgSystem::Snes)
				_snprintf_s(addr_buf, 24, _TRUNCATE, "%02X%04X",
				            (unsigned)st->bp->bank & 0xFF, (unsigned)st->bp->address);
			else
				_snprintf_s(addr_buf, 24, _TRUNCATE, "%04X", (unsigned)st->bp->address);
			SetDlgItemTextA(dlg, IDC_DBG_BP_ADDR, addr_buf);

			SetDlgItemTextA(dlg, IDC_DBG_BP_COND, st->bp->condition);

			SetFocus(GetDlgItem(dlg, IDC_DBG_BP_ADDR));
			return FALSE;
		}

		case WM_COMMAND:
			if (LOWORD(wp) == IDC_DBG_BP_TYPE && HIWORD(wp) == CBN_SELCHANGE)
			{
				EditDlgState *st = (EditDlgState *)GetWindowLongPtr(dlg, DWLP_USER);
				if (st)
				{
					HWND combo = (HWND)lp;
					int sel = (int)SendMessage(combo, CB_GETCURSEL, 0, 0);
					if (sel >= 0)
						UpdateRangeLabel(dlg, st->sys,
						                 (int)SendMessage(combo, CB_GETITEMDATA, sel, 0));
				}
				return TRUE;
			}
			if (LOWORD(wp) == IDOK)
			{
				EditDlgState *st = (EditDlgState *)GetWindowLongPtr(dlg, DWLP_USER);
				if (st)
				{
					HWND combo = GetDlgItem(dlg, IDC_DBG_BP_TYPE);
					int sel = (int)SendMessage(combo, CB_GETCURSEL, 0, 0);
					if (sel >= 0)
						st->bp->memory_type = (int)SendMessage(combo, CB_GETITEMDATA, sel, 0);

					st->bp->brk_execute = IsDlgButtonChecked(dlg, IDC_DBG_BP_EXEC)      == BST_CHECKED;
					st->bp->brk_read    = IsDlgButtonChecked(dlg, IDC_DBG_BP_READ)      == BST_CHECKED;
					st->bp->brk_write   = IsDlgButtonChecked(dlg, IDC_DBG_BP_WRITE)     == BST_CHECKED;
					st->bp->mark_event  = IsDlgButtonChecked(dlg, IDC_DBG_BP_MARK)      == BST_CHECKED;
					st->bp->break_exec  = IsDlgButtonChecked(dlg, IDC_DBG_BP_BREAKEXEC) == BST_CHECKED;

					char addr_buf[64] = "";
					GetDlgItemTextA(dlg, IDC_DBG_BP_ADDR, addr_buf, sizeof(addr_buf));
					uint32_t v = ParseHex(addr_buf);
					if (st->sys == DbgSystem::Snes)
					{
						st->bp->bank    = (uint8_t)((v >> 16) & 0xFF);
						st->bp->address = (uint16_t)(v & 0xFFFF);
					}
					else
					{
						st->bp->bank    = 0;
						st->bp->address = (uint16_t)(v & 0xFFFF);
					}

					GetDlgItemTextA(dlg, IDC_DBG_BP_COND, st->bp->condition, sizeof(st->bp->condition));
					st->bp->enabled = true;
					st->accepted = true;
				}
				EndDialog(dlg, IDOK);
				return TRUE;
			}
			if (LOWORD(wp) == IDCANCEL)
			{
				EndDialog(dlg, IDCANCEL);
				return TRUE;
			}
			break;
	}
	return FALSE;
}

bool ShowEditBreakpointDialog(HWND parent, DbgSystem sys, DbgBreakpoint *bp)
{
	EditDlgState st{};
	st.sys      = sys;
	st.bp       = bp;
	st.accepted = false;
	bp->system  = sys;
	const INT_PTR rv = DialogBoxParam(GetModuleHandle(NULL),
	                                  MAKEINTRESOURCE(IDD_DBG_BREAKPOINT), parent,
	                                  EditDlgProc, (LPARAM)&st);
	return rv == IDOK && st.accepted;
}
