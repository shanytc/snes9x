#include "CWatchPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "rsrc/resource.h"
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static const TCHAR *kClassName = TEXT("S9xDebuggerWatchPanel");

enum
{
	IDC_WATCH_LV = 7600,

	IDM_WATCH_ADD = 7700,
	IDM_WATCH_EDIT,
	IDM_WATCH_REMOVE,
	IDM_WATCH_REMOVE_ALL
};

enum { WCOL_NAME = 0, WCOL_VALUE = 1 };

struct WatchPanelState
{
	DbgSystem                sys  = DbgSystem::None;
	HFONT                    font = nullptr;
	HWND                     lv   = nullptr;
	std::vector<std::string> exprs;
};

static WatchPanelState *GetState(HWND h)
{
	return (WatchPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

struct WatchInputState
{
	const char *title;
	const char *prompt;
	char        text[64];
	bool        ok;
};

static INT_PTR CALLBACK WatchInputProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			WatchInputState *st = (WatchInputState *)lp;
			SetWindowLongPtr(dlg, DWLP_USER, (LONG_PTR)st);
			SetWindowTextA(dlg, st->title);
			SetDlgItemTextA(dlg, IDC_DBG_INPUT_PROMPT, st->prompt);
			SetDlgItemTextA(dlg, IDC_DBG_INPUT_EDIT,   st->text);
			SendDlgItemMessage(dlg, IDC_DBG_INPUT_EDIT, EM_SETSEL, 0, -1);
			SetFocus(GetDlgItem(dlg, IDC_DBG_INPUT_EDIT));
			return FALSE;
		}
		case WM_COMMAND:
			if (LOWORD(wp) == IDOK)
			{
				WatchInputState *st = (WatchInputState *)GetWindowLongPtr(dlg, DWLP_USER);
				if (st)
				{
					GetDlgItemTextA(dlg, IDC_DBG_INPUT_EDIT, st->text, (int)sizeof(st->text));
					st->ok = true;
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

static bool PromptText(HWND parent, const char *title, const char *prompt,
                       const char *initial, char *out, size_t cap)
{
	WatchInputState st = {};
	st.title  = title;
	st.prompt = prompt;
	if (initial) { strncpy(st.text, initial, sizeof(st.text) - 1); }

	const INT_PTR rv = DialogBoxParamA(GetModuleHandle(NULL),
	                                   MAKEINTRESOURCEA(IDD_DBG_INPUT), parent,
	                                   WatchInputProc, (LPARAM)&st);
	if (rv != IDOK || !st.ok) return false;
	strncpy(out, st.text, cap - 1);
	out[cap - 1] = 0;
	return true;
}

static int HexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + c - 'a';
	if (c >= 'A' && c <= 'F') return 10 + c - 'A';
	return -1;
}

// Parses "[bank:]addr[,len]" (hex address, decimal length). Returns false if
// no address digits were found.
static bool ParseWatchExpr(const char *s, uint8_t *bank, uint16_t *addr, int *len)
{
	*bank = 0;
	*addr = 0;
	*len  = 1;

	while (*s == ' ' || *s == '\t') s++;
	if (*s == '$') s++;
	else if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	uint32_t first = 0;
	bool any = false;
	while (HexDigit(*s) >= 0) { first = (first << 4) | (uint32_t)HexDigit(*s++); any = true; }
	if (!any) return false;

	if (*s == ':')
	{
		s++;
		uint32_t second = 0;
		bool any2 = false;
		while (HexDigit(*s) >= 0) { second = (second << 4) | (uint32_t)HexDigit(*s++); any2 = true; }
		if (!any2) return false;
		*bank = (uint8_t)(first & 0xFF);
		*addr = (uint16_t)(second & 0xFFFF);
	}
	else
	{
		*bank = (uint8_t)((first >> 16) & 0xFF);
		*addr = (uint16_t)(first & 0xFFFF);
	}

	while (*s == ' ' || *s == '\t') s++;
	if (*s == ',')
	{
		s++;
		int n = 0;
		bool anyn = false;
		while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); anyn = true; }
		if (anyn && n >= 1 && n <= 4) *len = n;
	}
	return true;
}

static void FormatWatchValue(WatchPanelState *st, const char *expr, char *out, size_t cap)
{
	uint8_t  bank = 0;
	uint16_t addr = 0;
	int      len  = 1;
	if (!ParseWatchExpr(expr, &bank, &addr, &len))
	{
		_snprintf_s(out, cap, _TRUNCATE, "?");
		return;
	}

	uint32_t value = 0;
	for (int i = 0; i < len; i++)
	{
		uint8_t b;
		if (st->sys == DbgSystem::Snes)
			b = SnesBackend::ReadByte((((uint32_t)bank << 16) | addr) + (uint32_t)i);
		else
			b = GbBackend::ReadByte((uint16_t)(addr + i));
		value |= (uint32_t)b << (8 * i);
	}

	_snprintf_s(out, cap, _TRUNCATE, "$%0*X (%u)", len * 2, value, value);
}

static void PopulateList(WatchPanelState *st)
{
	ListView_DeleteAllItems(st->lv);

	int row = 0;
	for (size_t i = 0; i < st->exprs.size(); i++, row++)
	{
		LVITEMA item = {};
		item.mask     = LVIF_TEXT;
		item.iItem    = row;
		item.iSubItem = WCOL_NAME;
		item.pszText  = (LPSTR)st->exprs[i].c_str();
		SendMessageA(st->lv, LVM_INSERTITEMA, 0, (LPARAM)&item);

		char val[64];
		FormatWatchValue(st, st->exprs[i].c_str(), val, sizeof(val));
		item.iSubItem = WCOL_VALUE;
		item.pszText  = val;
		SendMessageA(st->lv, LVM_SETITEMA, 0, (LPARAM)&item);
	}

	LVITEMA add = {};
	add.mask     = LVIF_TEXT;
	add.iItem    = row;
	add.iSubItem = WCOL_NAME;
	add.pszText  = (LPSTR)"Click to add...";
	SendMessageA(st->lv, LVM_INSERTITEMA, 0, (LPARAM)&add);
}

static int GetSelectedIndex(HWND lv)
{
	return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void OnAdd(HWND h, WatchPanelState *st)
{
	char buf[64] = "";
	if (!PromptText(h, "Add Watch", "Address  ([bank:]addr[,len], hex):", "", buf, sizeof(buf)))
		return;
	if (buf[0] == 0) return;
	st->exprs.push_back(buf);
	PopulateList(st);
}

static void OnEditRow(HWND h, WatchPanelState *st, int idx)
{
	if (idx < 0 || idx >= (int)st->exprs.size()) { OnAdd(h, st); return; }
	char buf[64] = "";
	if (!PromptText(h, "Edit Watch", "Address  ([bank:]addr[,len], hex):",
	                st->exprs[idx].c_str(), buf, sizeof(buf)))
		return;
	if (buf[0] == 0) st->exprs.erase(st->exprs.begin() + idx);
	else             st->exprs[idx] = buf;
	PopulateList(st);
}

static void OnRemove(HWND h, WatchPanelState *st)
{
	const int idx = GetSelectedIndex(st->lv);
	if (idx < 0 || idx >= (int)st->exprs.size()) return;
	st->exprs.erase(st->exprs.begin() + idx);
	PopulateList(st);
}

static void ShowContextMenu(HWND h, WatchPanelState *st)
{
	const int idx = GetSelectedIndex(st->lv);
	const bool on_entry = (idx >= 0 && idx < (int)st->exprs.size());

	HMENU menu = CreatePopupMenu();
	AppendMenuA(menu, MF_STRING, IDM_WATCH_ADD, "Add Watch...");
	AppendMenuA(menu, MF_STRING | (on_entry ? 0 : MF_GRAYED), IDM_WATCH_EDIT,   "Edit...");
	AppendMenuA(menu, MF_STRING | (on_entry ? 0 : MF_GRAYED), IDM_WATCH_REMOVE, "Remove");
	AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(menu, MF_STRING | (st->exprs.empty() ? MF_GRAYED : 0), IDM_WATCH_REMOVE_ALL, "Remove All");

	DWORD pos = GetMessagePos();
	const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
	                                    GET_X_LPARAM(pos), GET_Y_LPARAM(pos), 0, h, NULL);
	DestroyMenu(menu);

	switch (cmd)
	{
		case IDM_WATCH_ADD:        OnAdd(h, st); break;
		case IDM_WATCH_EDIT:       OnEditRow(h, st, idx); break;
		case IDM_WATCH_REMOVE:     OnRemove(h, st); break;
		case IDM_WATCH_REMOVE_ALL: st->exprs.clear(); PopulateList(st); break;
	}
}

static LRESULT CALLBACK WatchWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			WatchPanelState *st = new WatchPanelState();
			st->sys  = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			WatchPanelState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                        WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                        | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                        0, 0, 0, 0, h, (HMENU)(LONG_PTR)IDC_WATCH_LV,
			                        GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)"Name";  col.cx = 200; SendMessageA(st->lv, LVM_INSERTCOLUMNA, WCOL_NAME,  (LPARAM)&col);
			col.pszText = (LPSTR)"Value"; col.cx = 130; SendMessageA(st->lv, LVM_INSERTCOLUMNA, WCOL_VALUE, (LPARAM)&col);

			PopulateList(st);
			return 0;
		}

		case WM_DESTROY:
		{
			WatchPanelState *st = GetState(h);
			if (st) delete st;
			return 0;
		}

		case WM_SIZE:
		{
			WatchPanelState *st = GetState(h);
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
			WatchPanelState *st = GetState(h);
			if (!st || hdr->hwndFrom != st->lv) break;
			switch (hdr->code)
			{
				case NM_DBLCLK:
				{
					NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
					OnEditRow(h, st, ia->iItem);
					return 0;
				}
				case NM_RCLICK:
					ShowContextMenu(h, st);
					return 0;
				case LVN_KEYDOWN:
				{
					NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
					if      (kd->wVKey == VK_DELETE) OnRemove(h, st);
					else if (kd->wVKey == VK_INSERT) OnAdd(h, st);
					return 0;
				}
			}
			break;
		}
	}
	return DefWindowProc(h, msg, wp, lp);
}

void WatchPanelRegisterClass(HINSTANCE hInst)
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
	wc.lpfnWndProc   = WatchWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND WatchPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void WatchPanelRefresh(HWND h)
{
	WatchPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	for (size_t i = 0; i < st->exprs.size(); i++)
	{
		char val[64];
		FormatWatchValue(st, st->exprs[i].c_str(), val, sizeof(val));
		LVITEMA item = {};
		item.mask     = LVIF_TEXT;
		item.iItem    = (int)i;
		item.iSubItem = WCOL_VALUE;
		item.pszText  = val;
		SendMessageA(st->lv, LVM_SETITEMA, 0, (LPARAM)&item);
	}
}
