#include <windows.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>

#include "CDebuggerDlg.h"
#include "CDebugger.h"
#include "CDebuggerSplitter.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "CDisasmPanel.h"
#include "CStatusPanel.h"
#include "CBreakpointsPanel.h"
#include "CMemoryViewer.h"
#include "rsrc/resource.h"
#include "../snes9x.h"

static HACCEL g_dbg_accel = NULL;

HACCEL S9xDebuggerGetAcceleratorsForWindow(HWND wnd);

struct InputDlgState
{
	const TCHAR *prompt;
	const TCHAR *title;
	TCHAR        result[64];
	bool         accepted;
};

static INT_PTR CALLBACK InputDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			InputDlgState *st = (InputDlgState *)lp;
			SetWindowLongPtr(dlg, DWLP_USER, (LONG_PTR)st);
			SetWindowText(dlg, st->title);
			SetDlgItemText(dlg, IDC_DBG_INPUT_PROMPT, st->prompt);
			SetDlgItemText(dlg, IDC_DBG_INPUT_EDIT,   st->result);
			SendDlgItemMessage(dlg, IDC_DBG_INPUT_EDIT, EM_SETSEL, 0, -1);
			SetFocus(GetDlgItem(dlg, IDC_DBG_INPUT_EDIT));
			return FALSE;
		}
		case WM_COMMAND:
			if (LOWORD(wp) == IDOK)
			{
				InputDlgState *st = (InputDlgState *)GetWindowLongPtr(dlg, DWLP_USER);
				if (st)
				{
					GetDlgItemText(dlg, IDC_DBG_INPUT_EDIT, st->result,
					               (int)(sizeof(st->result)/sizeof(TCHAR)));
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

static bool PromptHex(HWND parent, const TCHAR *title, const TCHAR *prompt,
                      const TCHAR *initial, uint32_t *out_value)
{
	InputDlgState st = {};
	st.title  = title;
	st.prompt = prompt;
	if (initial) _tcsncpy(st.result, initial, 63);

	const INT_PTR rv = DialogBoxParam(GetModuleHandle(NULL),
	                                  MAKEINTRESOURCE(IDD_DBG_INPUT), parent,
	                                  InputDlgProc, (LPARAM)&st);
	if (rv != IDOK || !st.accepted) return false;

	const TCHAR *s = st.result;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '$') s++;
	else if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	uint32_t v = 0;
	bool any = false;
	while (*s)
	{
		TCHAR c = *s++;
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
		else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
		else break;
		v = (v << 4) | (uint32_t)d;
		any = true;
	}
	if (!any) return false;
	*out_value = v;
	return true;
}

#pragma comment(lib, "comctl32.lib")

static const TCHAR *kSnesClassName = TEXT("S9xDebuggerSnesWnd");
static const TCHAR *kGbClassName   = TEXT("S9xDebuggerGbWnd");

enum
{
	IDC_DBG_TOOLBAR = 5100,
	IDC_DBG_STATUSBAR,
	IDC_DBG_DISASM,
	IDC_DBG_STATUS_PANEL,
	IDC_DBG_LABELS,
	IDC_DBG_SETTINGS,
	IDC_DBG_WATCH,
	IDC_DBG_BREAKPOINTS,
	IDC_DBG_CALLSTACK,
	IDC_DBG_SPLIT_MAIN,
	IDC_DBG_SPLIT_BODY,
	IDC_DBG_SPLIT_RIGHT,
	IDC_DBG_SPLIT_MID,
	IDC_DBG_SPLIT_BOTTOM,
	IDC_DBG_SPLIT_BOTTOM2,

	IDM_DBG_FILE_EXIT = 5200,
	IDM_DBG_RUN,
	IDM_DBG_PAUSE,
	IDM_DBG_STEP_IN,
	IDM_DBG_STEP_OVER,
	IDM_DBG_STEP_OUT,
	IDM_DBG_FRAME_STEP,
	IDM_DBG_RESET,
	IDM_DBG_GOTO,
	IDM_DBG_FIND,
	IDM_DBG_STEP_BACK,
	IDM_DBG_STEP_BACK_SCANLINE,
	IDM_DBG_STEP_BACK_FRAME,
	IDM_DBG_RUN_ONE_CPU,
	IDM_DBG_RUN_ONE_PPU,
	IDM_DBG_RUN_ONE_SCANLINE,
	IDM_DBG_RUN_TO_NMI,
	IDM_DBG_RUN_TO_IRQ,
	IDM_DBG_RUN_TO_SCANLINE,
	IDM_DBG_BREAK_IN,
	IDM_DBG_RELOAD_ROM,
	IDM_DBG_MEMORY_VIEWER,

	IDB_DBG_RUN = 5300,
	IDB_DBG_PAUSE,
	IDB_DBG_STEP_IN,
	IDB_DBG_STEP_OVER,
	IDB_DBG_STEP_OUT,
	IDB_DBG_FRAME_STEP,
	IDB_DBG_RESET
};

struct DbgDlgState
{
	DbgSystem sys;
	HWND      hToolbar;
	HWND      hStatusbar;
	HWND      hSplitMain;
	HWND      hSplitBody;
	HWND      hSplitRight;
	HWND      hSplitMid;
	HWND      hSplitBottom;
	HWND      hSplitBottom2;
	HWND      hDisasm;
	HWND      hStatus;
	HWND      hLabels;
	HWND      hSettings;
	HWND      hWatch;
	HWND      hBreakpoints;
	HWND      hCallstack;
};

static DbgDlgState *GetState(HWND h)
{
	return (DbgDlgState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static HMENU BuildMenu()
{
	HMENU menu = CreateMenu();

	HMENU mFile = CreatePopupMenu();
	AppendMenu(mFile, MF_STRING, IDM_DBG_FILE_EXIT, TEXT("E&xit"));
	AppendMenu(menu, MF_POPUP, (UINT_PTR)mFile, TEXT("&File"));

	HMENU mDebug = CreatePopupMenu();
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RUN,                TEXT("&Continue\tF5"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_PAUSE,              TEXT("&Break\tShift+F5"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_IN,            TEXT("Step &Into\tF11"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_OVER,          TEXT("Step &Over\tF10"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_OUT,           TEXT("Step Ou&t\tShift+F11"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING | MF_GRAYED, IDM_DBG_STEP_BACK,          TEXT("Step Back\tShift+F10"));
	AppendMenu(mDebug, MF_STRING | MF_GRAYED, IDM_DBG_STEP_BACK_SCANLINE, TEXT("Step Back (1 scanline)\tShift+F7"));
	AppendMenu(mDebug, MF_STRING | MF_GRAYED, IDM_DBG_STEP_BACK_FRAME,    TEXT("Step Back (1 frame)\tShift+F8"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING | MF_GRAYED, IDM_DBG_RUN_ONE_CPU,      TEXT("Run one CPU cycle"));
	AppendMenu(mDebug, MF_STRING | MF_GRAYED, IDM_DBG_RUN_ONE_PPU,      TEXT("Run one PPU cycle\tF6"));
	AppendMenu(mDebug, MF_STRING,             IDM_DBG_RUN_ONE_SCANLINE, TEXT("Run one scanline\tF7"));
	AppendMenu(mDebug, MF_STRING,             IDM_DBG_FRAME_STEP,       TEXT("Run one frame\tF8"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RUN_TO_NMI,      TEXT("Run to NMI"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RUN_TO_IRQ,      TEXT("Run to IRQ"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RUN_TO_SCANLINE, TEXT("Run to scanline...\tAlt+B"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_BREAK_IN,        TEXT("Break in...\tCtrl+B"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RESET,           TEXT("&Reset\tCtrl+R"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RELOAD_ROM,      TEXT("Reload ROM\tCtrl+Shift+R"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_MEMORY_VIEWER,   TEXT("&Memory Viewer...\tCtrl+M"));
	AppendMenu(menu, MF_POPUP, (UINT_PTR)mDebug, TEXT("&Debug"));

	HMENU mSearch = CreatePopupMenu();
	AppendMenu(mSearch, MF_STRING | MF_GRAYED, IDM_DBG_GOTO, TEXT("&Go to address...\tCtrl+G"));
	AppendMenu(mSearch, MF_STRING | MF_GRAYED, IDM_DBG_FIND, TEXT("&Find sequence..."));
	AppendMenu(menu, MF_POPUP, (UINT_PTR)mSearch, TEXT("&Search"));

	HMENU mSettings = CreatePopupMenu();
	AppendMenu(mSettings, MF_STRING | MF_GRAYED, 0, TEXT("(forwarded to sidebar)"));
	AppendMenu(menu, MF_POPUP, (UINT_PTR)mSettings, TEXT("&Settings"));

	return menu;
}

static HWND CreateToolbar(HWND parent)
{
	HWND tb = CreateWindowEx(0, TOOLBARCLASSNAME, NULL,
	                         WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NOPARENTALIGN | CCS_NORESIZE | CCS_NODIVIDER,
	                         0, 0, 0, 0,
	                         parent, (HMENU)(LONG_PTR)IDC_DBG_TOOLBAR, GetModuleHandle(NULL), NULL);
	if (!tb) return NULL;

	SendMessage(tb, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
	SendMessage(tb, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);

	TBBUTTON btns[] =
	{
		{ I_IMAGENONE, IDM_DBG_RUN,        TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Run") },
		{ I_IMAGENONE, IDM_DBG_PAUSE,      TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Pause") },
		{ 0, 0, 0, BTNS_SEP, {0}, 0, 0 },
		{ I_IMAGENONE, IDM_DBG_STEP_IN,    TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Step In") },
		{ I_IMAGENONE, IDM_DBG_STEP_OVER,  TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Step Over") },
		{ I_IMAGENONE, IDM_DBG_STEP_OUT,   TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Step Out") },
		{ I_IMAGENONE, IDM_DBG_FRAME_STEP, TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Frame") },
		{ 0, 0, 0, BTNS_SEP, {0}, 0, 0 },
		{ I_IMAGENONE, IDM_DBG_RESET,      TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)TEXT("Reset") }
	};
	SendMessage(tb, TB_ADDBUTTONS, sizeof(btns)/sizeof(btns[0]), (LPARAM)btns);
	SendMessage(tb, TB_AUTOSIZE, 0, 0);
	return tb;
}

static HWND CreatePlaceholder(HWND parent, int id, const TCHAR *text)
{
	HWND h = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("STATIC"), text,
	                       WS_CHILD | WS_VISIBLE | SS_CENTER,
	                       0, 0, 0, 0,
	                       parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), NULL);
	if (h)
		SendMessage(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
	return h;
}

static void ResizeLayout(HWND hwnd)
{
	DbgDlgState *st = GetState(hwnd);
	if (!st) return;

	RECT rc;
	GetClientRect(hwnd, &rc);
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;

	int tbH = 0;
	if (st->hToolbar)
	{
		RECT tbRc; GetWindowRect(st->hToolbar, &tbRc);
		tbH = tbRc.bottom - tbRc.top;
		MoveWindow(st->hToolbar, 0, 0, w, tbH, TRUE);
	}

	int sbH = 0;
	if (st->hStatusbar)
	{
		RECT sbRc; GetWindowRect(st->hStatusbar, &sbRc);
		sbH = sbRc.bottom - sbRc.top;
		MoveWindow(st->hStatusbar, 0, h - sbH, w, sbH, TRUE);
	}

	const int bodyTop = tbH;
	const int bodyBot = h - sbH;
	if (st->hSplitMain && bodyBot > bodyTop)
		MoveWindow(st->hSplitMain, 0, bodyTop, w, bodyBot - bodyTop, TRUE);
}

static void OnCommand(HWND hwnd, DbgDlgState *st, WPARAM wp)
{
	const int id = LOWORD(wp);
	switch (id)
	{
		case IDM_DBG_FILE_EXIT:
			DestroyWindow(hwnd);
			break;
		case IDM_DBG_RUN:
			gDebugger.Run();
			DebuggerDlgRefresh(hwnd);
			break;
		case IDM_DBG_PAUSE:
			gDebugger.Pause(st->sys);
			DebuggerDlgRefresh(hwnd);
			break;
		case IDM_DBG_STEP_IN:
			gDebugger.StepIn(st->sys);
			break;
		case IDM_DBG_STEP_OVER:
			gDebugger.StepOver(st->sys);
			break;
		case IDM_DBG_STEP_OUT:
			gDebugger.StepOut(st->sys);
			break;
		case IDM_DBG_FRAME_STEP:
			gDebugger.FrameStep();
			break;
		case IDM_DBG_RUN_ONE_SCANLINE:
			gDebugger.StepOneScanline(st->sys);
			break;
		case IDM_DBG_RUN_TO_NMI:
			gDebugger.RunToNmi(st->sys);
			break;
		case IDM_DBG_RUN_TO_IRQ:
			gDebugger.RunToIrq(st->sys);
			break;
		case IDM_DBG_RUN_TO_SCANLINE:
		{
			uint32_t v = 0;
			if (PromptHex(hwnd, TEXT("Run to scanline"),
			              TEXT("Target scanline (hex, e.g. E1 for VBlank):"),
			              TEXT("E1"), &v))
				gDebugger.RunToScanline(st->sys, (int)v);
			break;
		}
		case IDM_DBG_BREAK_IN:
		{
			uint32_t v = 0;
			if (PromptHex(hwnd, TEXT("Break in"),
			              TEXT("Cycles from now (hex):"),
			              TEXT("400"), &v))
				gDebugger.BreakIn(st->sys, (uint64_t)v);
			break;
		}
		case IDM_DBG_RESET:
			gDebugger.ResetMachine(st->sys);
			DebuggerDlgRefresh(hwnd);
			break;
		case IDM_DBG_RELOAD_ROM:
			gDebugger.ReloadRom();
			DebuggerDlgRefresh(hwnd);
			break;
		case IDM_DBG_MEMORY_VIEWER:
			OpenMemoryViewer(st->sys);
			break;
		default:
			break;
	}
}

static LRESULT CALLBACK DebuggerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			DbgDlgState *st = new DbgDlgState();
			memset(st, 0, sizeof(*st));
			st->sys = (DbgSystem)(intptr_t)cs->lpCreateParams;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			DbgDlgState *st = GetState(hwnd);
			HFONT guiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

			st->hToolbar   = CreateToolbar(hwnd);
			if (st->hToolbar)
				SendMessage(st->hToolbar, WM_SETFONT, (WPARAM)guiFont, TRUE);

			st->hStatusbar = CreateWindowEx(0, STATUSCLASSNAME, NULL,
			                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
			                                0, 0, 0, 0,
			                                hwnd, (HMENU)(LONG_PTR)IDC_DBG_STATUSBAR,
			                                GetModuleHandle(NULL), NULL);
			SendMessage(st->hStatusbar, WM_SETFONT, (WPARAM)guiFont, TRUE);
			int sbParts[3] = { 200, 400, -1 };
			SendMessage(st->hStatusbar, SB_SETPARTS, 3, (LPARAM)sbParts);
			SendMessage(st->hStatusbar, SB_SETTEXT, 0, (LPARAM)TEXT("Running"));
			SendMessage(st->hStatusbar, SB_SETTEXT, 1, (LPARAM)TEXT("Code: --%  Data: --%"));
			SendMessage(st->hStatusbar, SB_SETTEXT, 2, (LPARAM)TEXT("0 cycles elapsed"));

			st->hSplitMain    = SplitterCreate(hwnd,         SPLIT_VERT, IDC_DBG_SPLIT_MAIN);
			st->hSplitBody    = SplitterCreate(st->hSplitMain, SPLIT_HORZ, IDC_DBG_SPLIT_BODY);
			st->hSplitBottom  = SplitterCreate(st->hSplitMain, SPLIT_HORZ, IDC_DBG_SPLIT_BOTTOM);
			st->hSplitRight   = SplitterCreate(st->hSplitBody, SPLIT_HORZ, IDC_DBG_SPLIT_RIGHT);
			st->hSplitMid     = SplitterCreate(st->hSplitRight, SPLIT_VERT, IDC_DBG_SPLIT_MID);
			st->hSplitBottom2 = SplitterCreate(st->hSplitBottom, SPLIT_HORZ, IDC_DBG_SPLIT_BOTTOM2);

			st->hDisasm      = DisasmPanelCreate(st->hSplitBody,   st->sys, IDC_DBG_DISASM);
			st->hStatus      = StatusPanelCreate(st->hSplitMid,    st->sys, IDC_DBG_STATUS_PANEL);
			st->hLabels      = CreatePlaceholder(st->hSplitMid,    IDC_DBG_LABELS,      TEXT("Labels"));
			st->hSettings    = CreatePlaceholder(st->hSplitRight,  IDC_DBG_SETTINGS,    TEXT("Disassembly settings"));
			st->hWatch       = CreatePlaceholder(st->hSplitBottom, IDC_DBG_WATCH,       TEXT("Watch"));
			st->hBreakpoints = BreakpointsPanelCreate(st->hSplitBottom2, st->sys, IDC_DBG_BREAKPOINTS);
			st->hCallstack   = CreatePlaceholder(st->hSplitBottom2,IDC_DBG_CALLSTACK,   TEXT("Call Stack"));

			SplitterSetChildren(st->hSplitBottom2, st->hBreakpoints, st->hCallstack);
			SplitterSetChildren(st->hSplitBottom,  st->hWatch, st->hSplitBottom2);
			SplitterSetChildren(st->hSplitMid,     st->hStatus, st->hLabels);
			SplitterSetChildren(st->hSplitRight,   st->hSplitMid, st->hSettings);
			SplitterSetChildren(st->hSplitBody,    st->hDisasm, st->hSplitRight);
			SplitterSetChildren(st->hSplitMain,    st->hSplitBody, st->hSplitBottom);

			SplitterSetRatio(st->hSplitMain, 0.78f);
			SplitterSetRatio(st->hSplitBody, 0.45f);
			SplitterSetRatio(st->hSplitRight, 0.65f);
			SplitterSetRatio(st->hSplitMid, 0.55f);
			SplitterSetRatio(st->hSplitBottom, 0.30f);
			SplitterSetRatio(st->hSplitBottom2, 0.50f);

			if (st->sys == DbgSystem::Snes)
				gDebugger.AttachSnes(hwnd);
			else
				gDebugger.AttachGb(hwnd);

			ResizeLayout(hwnd);
			DebuggerDlgRefresh(hwnd);
			return 0;
		}

		case WM_SIZE:
			ResizeLayout(hwnd);
			return 0;

		case WM_COMMAND:
		{
			DbgDlgState *st = GetState(hwnd);
			if (st) OnCommand(hwnd, st, wp);
			return 0;
		}

		case WM_USER_DEBUGGER_REFRESH:
			DebuggerDlgRefresh(hwnd);
			return 0;

		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
		{
			DbgDlgState *st = GetState(hwnd);
			if (st)
			{
				if (st->sys == DbgSystem::Snes) gDebugger.DetachSnes();
				else                            gDebugger.DetachGb();
				delete st;
				SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
			}
			return 0;
		}
	}

	return DefWindowProc(hwnd, msg, wp, lp);
}

static void RegisterDebuggerClass(HINSTANCE hInst, const TCHAR *name)
{
	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = DebuggerWndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = name;
	wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassEx(&wc);
}

void DebuggerDlgGlobalInit(HINSTANCE hInst)
{
	static bool initialised = false;
	if (initialised) return;
	initialised = true;

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
	InitCommonControlsEx(&icc);

	SplitterRegisterClass(hInst);
	DisasmPanelRegisterClass(hInst);
	StatusPanelRegisterClass(hInst);
	BreakpointsPanelRegisterClass(hInst);
	RegisterDebuggerClass(hInst, kSnesClassName);
	RegisterDebuggerClass(hInst, kGbClassName);

	ACCEL accels[] = {
		{ FVIRTKEY,                     (WORD)VK_F5,  (WORD)IDM_DBG_RUN              },
		{ FVIRTKEY | FSHIFT,            (WORD)VK_F5,  (WORD)IDM_DBG_PAUSE            },
		{ FVIRTKEY,                     (WORD)VK_F11, (WORD)IDM_DBG_STEP_IN          },
		{ FVIRTKEY,                     (WORD)VK_F10, (WORD)IDM_DBG_STEP_OVER        },
		{ FVIRTKEY | FSHIFT,            (WORD)VK_F11, (WORD)IDM_DBG_STEP_OUT         },
		{ FVIRTKEY,                     (WORD)VK_F7,  (WORD)IDM_DBG_RUN_ONE_SCANLINE },
		{ FVIRTKEY,                     (WORD)VK_F8,  (WORD)IDM_DBG_FRAME_STEP       },
		{ FVIRTKEY | FALT,              (WORD)'B',    (WORD)IDM_DBG_RUN_TO_SCANLINE  },
		{ FVIRTKEY | FCONTROL,          (WORD)'B',    (WORD)IDM_DBG_BREAK_IN         },
		{ FVIRTKEY | FCONTROL,          (WORD)'R',    (WORD)IDM_DBG_RESET            },
		{ FVIRTKEY | FCONTROL | FSHIFT, (WORD)'R',    (WORD)IDM_DBG_RELOAD_ROM       },
		{ FVIRTKEY | FCONTROL,          (WORD)'M',    (WORD)IDM_DBG_MEMORY_VIEWER    },
	};
	g_dbg_accel = CreateAcceleratorTable(accels, sizeof(accels)/sizeof(accels[0]));
}

HACCEL S9xDebuggerGetAcceleratorsForWindow(HWND wnd)
{
	if (!wnd || !g_dbg_accel) return NULL;
	HWND root = GetAncestor(wnd, GA_ROOT);
	if (root == gSnesDebuggerHWND || root == gGbDebuggerHWND)
		return g_dbg_accel;
	return NULL;
}

HWND DebuggerDlgCreate(DbgSystem sys)
{
	const TCHAR *cls   = (sys == DbgSystem::Snes) ? kSnesClassName : kGbClassName;
	const TCHAR *title = (sys == DbgSystem::Snes) ? TEXT("Debugger - SNES") : TEXT("Debugger - Game Boy");

	HMENU menu = BuildMenu();
	HWND hwnd = CreateWindowEx(0, cls, title,
	                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
	                           CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
	                           NULL, menu, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
	if (hwnd)
	{
		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
	}
	return hwnd;
}

void DebuggerDlgClose(HWND h)
{
	if (h && IsWindow(h))
		DestroyWindow(h);
}

static void UpdateRunPauseButtonStates(DbgDlgState *st, HWND h)
{
	const bool running = !Settings.Paused;

	if (st->hToolbar)
	{
		SendMessage(st->hToolbar, TB_ENABLEBUTTON, IDM_DBG_RUN,   MAKELONG(running ? 0 : 1, 0));
		SendMessage(st->hToolbar, TB_ENABLEBUTTON, IDM_DBG_PAUSE, MAKELONG(running ? 1 : 0, 0));
	}
	HMENU menu = GetMenu(h);
	if (menu)
	{
		EnableMenuItem(menu, IDM_DBG_RUN,   MF_BYCOMMAND | (running ? MF_GRAYED  : MF_ENABLED));
		EnableMenuItem(menu, IDM_DBG_PAUSE, MF_BYCOMMAND | (running ? MF_ENABLED : MF_GRAYED));
	}
}

void DebuggerDlgRefresh(HWND h)
{
	if (!h || !IsWindow(h)) return;
	DbgDlgState *st = GetState(h);
	if (!st || !st->hStatusbar) return;

	TCHAR buf[64];

	UpdateRunPauseButtonStates(st, h);

	if (Settings.Paused)
		SendMessage(st->hStatusbar, SB_SETTEXT, 0, (LPARAM)TEXT("Paused"));
	else if (st->sys == DbgSystem::Gb && gDebugger.IsGbBreakPending())
		SendMessage(st->hStatusbar, SB_SETTEXT, 0, (LPARAM)TEXT("Waiting for GB execution..."));
	else
		SendMessage(st->hStatusbar, SB_SETTEXT, 0, (LPARAM)TEXT("Running"));

	if (st->sys == DbgSystem::Snes)
	{
		SnesStatus ss = {};
		SnesBackend::GetStatus(&ss);
		_sntprintf(buf, 64, TEXT("%u cycles  V:%u  F:%u"), ss.cycles, ss.v_counter, ss.frame);
		buf[63] = 0;
		SendMessage(st->hStatusbar, SB_SETTEXT, 2, (LPARAM)buf);
	}
	else
	{
		GbStatus gs = {};
		GbBackend::GetStatus(&gs);
		_sntprintf(buf, 64, TEXT("PC:$%04X  T:%llu"), gs.pc, (unsigned long long)gs.t_cycles);
		buf[63] = 0;
		SendMessage(st->hStatusbar, SB_SETTEXT, 2, (LPARAM)buf);
	}

	if (st->hDisasm) DisasmPanelRefresh(st->hDisasm);
	if (st->hStatus) StatusPanelRefresh(st->hStatus);
	if (st->hBreakpoints) BreakpointsPanelRefresh(st->hBreakpoints);

	InvalidateRect(h, NULL, FALSE);
}
