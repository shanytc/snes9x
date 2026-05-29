#include "CCallStackPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "../sgb/sgb.h"
#include <commctrl.h>
#include <stdio.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerCallStackPanel");

enum { IDC_CS_LV = 7800 };
enum { CSCOL_FUNC = 0, CSCOL_PC = 1, CSCOL_ROM = 2 };

struct CallStackPanelState
{
	DbgSystem sys           = DbgSystem::None;
	HFONT     font          = nullptr;
	HWND      lv            = nullptr;
	uint32_t  shown_version = 0xFFFFFFFFu;
	uint32_t  shown_pc      = 0xFFFFFFFFu;
};

static CallStackPanelState *GetState(HWND h)
{
	return (CallStackPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static void FuncName(DbgSystem sys, uint32_t addr, char *out, size_t cap)
{
	if (sys == DbgSystem::Snes) _snprintf_s(out, cap, _TRUNCATE, "sub_%06X", addr & 0xFFFFFF);
	else                        _snprintf_s(out, cap, _TRUNCATE, "sub_%04X", addr & 0xFFFF);
}

static void PcText(DbgSystem sys, uint32_t pc, char *out, size_t cap)
{
	if (sys == DbgSystem::Snes)
		_snprintf_s(out, cap, _TRUNCATE, "$%02X:%04X", (pc >> 16) & 0xFF, pc & 0xFFFF);
	else
		_snprintf_s(out, cap, _TRUNCATE, "$%04X", pc & 0xFFFF);
}

static void RomText(DbgSystem sys, uint32_t pc, uint8_t bank, char *out, size_t cap)
{
	if (sys == DbgSystem::Gb)
	{
		const uint16_t a = (uint16_t)(pc & 0xFFFF);
		if      (a < 0x4000) _snprintf_s(out, cap, _TRUNCATE, "$%06X [PRG]", a);
		else if (a < 0x8000) _snprintf_s(out, cap, _TRUNCATE, "$%06X [PRG]",
		                                 (uint32_t)bank * 0x4000 + (a - 0x4000));
		else                 _snprintf_s(out, cap, _TRUNCATE, "-");
	}
	else
	{
		_snprintf_s(out, cap, _TRUNCATE, "$%06X [PRG]", pc & 0xFFFFFF);
	}
}

static void AddRow(HWND lv, int row, DbgSystem sys, const char *func,
                   uint32_t pc, uint8_t bank)
{
	char pc_buf[24], rom_buf[24];
	PcText(sys, pc, pc_buf, sizeof(pc_buf));
	RomText(sys, pc, bank, rom_buf, sizeof(rom_buf));

	LVITEMA item = {};
	item.mask     = LVIF_TEXT | LVIF_PARAM;
	item.iItem    = row;
	item.iSubItem = CSCOL_FUNC;
	item.pszText  = (LPSTR)func;
	item.lParam   = (LPARAM)pc;   // display address to jump to on double-click
	SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&item);

	// Subitems: LVM_SETITEM rejects LVIF_PARAM on a subitem (lParam lives on
	// the item only), so set text-only here or the columns render blank.
	item.mask     = LVIF_TEXT;
	item.iSubItem = CSCOL_PC;
	item.pszText  = pc_buf;
	SendMessageA(lv, LVM_SETITEMA, 0, (LPARAM)&item);

	item.iSubItem = CSCOL_ROM;
	item.pszText  = rom_buf;
	SendMessageA(lv, LVM_SETITEMA, 0, (LPARAM)&item);
}

static void Rebuild(CallStackPanelState *st)
{
	ListView_DeleteAllItems(st->lv);

	uint32_t cur_pc   = 0;
	uint8_t  cur_bank = 0;
	if (st->sys == DbgSystem::Snes)
	{
		SnesStatus ss = {};
		SnesBackend::GetStatus(&ss);
		cur_pc = ((uint32_t)ss.prog_bank << 16) | ss.PC;
	}
	else
	{
		cur_pc = GbBackend::CurrentDisplayPC();
		const uint16_t a = (uint16_t)(cur_pc & 0xFFFF);
		if (a >= 0x4000 && a < 0x8000)
			cur_bank = (uint8_t)(S9xSGBGetCurrentRomBank() & 0xFF);
	}

	const std::vector<DbgCallFrame> &cs = gDebugger.CallStack(st->sys);
	const int n = (int)cs.size();

	int row = 0;
	char name[40];

	FuncName(st->sys, n > 0 ? cs[n - 1].target : cur_pc, name, sizeof(name));
	AddRow(st->lv, row++, st->sys, name, cur_pc, cur_bank);

	for (int i = n - 1; i >= 1; i--)
	{
		FuncName(st->sys, cs[i - 1].target, name, sizeof(name));
		AddRow(st->lv, row++, st->sys, name, cs[i].caller_pc, cs[i].caller_bank);
	}

	const uint32_t bot_pc   = (n > 0) ? cs[0].caller_pc   : cur_pc;
	const uint8_t  bot_bank = (n > 0) ? cs[0].caller_bank : cur_bank;
	AddRow(st->lv, row++, st->sys, "[bottom of stack]", bot_pc, bot_bank);

	st->shown_version = gDebugger.CallStackVersion();
	st->shown_pc      = cur_pc;
}

static LRESULT CALLBACK CsWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			CallStackPanelState *st = new CallStackPanelState();
			st->sys  = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			CallStackPanelState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                        WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                        | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                        0, 0, 0, 0, h, (HMENU)(LONG_PTR)IDC_CS_LV,
			                        GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)"Function";    col.cx = 130; SendMessageA(st->lv, LVM_INSERTCOLUMNA, CSCOL_FUNC, (LPARAM)&col);
			col.pszText = (LPSTR)"PC Address";  col.cx = 90;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, CSCOL_PC,   (LPARAM)&col);
			col.pszText = (LPSTR)"ROM Address"; col.cx = 110; SendMessageA(st->lv, LVM_INSERTCOLUMNA, CSCOL_ROM,  (LPARAM)&col);

			Rebuild(st);
			return 0;
		}

		case WM_DESTROY:
		{
			CallStackPanelState *st = GetState(h);
			if (st) delete st;
			return 0;
		}

		case WM_SIZE:
		{
			CallStackPanelState *st = GetState(h);
			if (st && st->lv)
			{
				RECT rc; GetClientRect(h, &rc);
				MoveWindow(st->lv, 0, 0, rc.right, rc.bottom, TRUE);
			}
			return 0;
		}

		case WM_NOTIFY:
		{
			NMHDR *nh = (NMHDR *)lp;
			if (nh && nh->idFrom == IDC_CS_LV &&
			    (nh->code == NM_DBLCLK || nh->code == NM_RETURN))
			{
				int sel = (nh->code == NM_DBLCLK)
				            ? (int)((NMITEMACTIVATE *)nh)->iItem
				            : ListView_GetNextItem(nh->hwndFrom, -1, LVNI_SELECTED);
				if (sel >= 0)
				{
					LVITEMA it = {};
					it.mask  = LVIF_PARAM;
					it.iItem = sel;
					if (SendMessageA(nh->hwndFrom, LVM_GETITEMA, 0, (LPARAM)&it))
					{
						HWND root = GetAncestor(h, GA_ROOT);
						if (root)
							PostMessage(root, WM_USER_DEBUGGER_GOTO, 0, (LPARAM)it.lParam);
					}
				}
				return 0;
			}
			break;
		}
	}
	return DefWindowProc(h, msg, wp, lp);
}

void CallStackPanelRegisterClass(HINSTANCE hInst)
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
	wc.lpfnWndProc   = CsWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND CallStackPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void CallStackPanelRefresh(HWND h)
{
	CallStackPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	uint32_t cur_pc = 0;
	if (st->sys == DbgSystem::Snes)
	{
		SnesStatus ss = {};
		SnesBackend::GetStatus(&ss);
		cur_pc = ((uint32_t)ss.prog_bank << 16) | ss.PC;
	}
	else
	{
		cur_pc = GbBackend::CurrentDisplayPC();
	}

	if (st->shown_version == gDebugger.CallStackVersion() && st->shown_pc == cur_pc)
		return;

	Rebuild(st);
}
