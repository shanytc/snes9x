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
#include "../snes9x.h"

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
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RUN,        TEXT("&Run\tF5"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_PAUSE,      TEXT("&Pause\tF6"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_IN,    TEXT("Step &Into\tF11"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_OVER,  TEXT("Step &Over\tF10"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_STEP_OUT,   TEXT("Step Ou&t\tShift+F11"));
	AppendMenu(mDebug, MF_SEPARATOR, 0, NULL);
	AppendMenu(mDebug, MF_STRING, IDM_DBG_FRAME_STEP, TEXT("&Frame Advance\tF7"));
	AppendMenu(mDebug, MF_STRING, IDM_DBG_RESET,      TEXT("Re&set"));
	AppendMenu(menu, MF_POPUP, (UINT_PTR)mDebug, TEXT("&Debug"));

	HMENU mSearch = CreatePopupMenu();
	AppendMenu(mSearch, MF_STRING, IDM_DBG_GOTO, TEXT("&Go to address...\tCtrl+G"));
	AppendMenu(mSearch, MF_STRING, IDM_DBG_FIND, TEXT("&Find sequence..."));
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
			gDebugger.Pause();
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
		case IDM_DBG_RESET:
			gDebugger.ResetMachine(st->sys);
			DebuggerDlgRefresh(hwnd);
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
			st->hBreakpoints = CreatePlaceholder(st->hSplitBottom2,IDC_DBG_BREAKPOINTS, TEXT("Breakpoints"));
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
	RegisterDebuggerClass(hInst, kSnesClassName);
	RegisterDebuggerClass(hInst, kGbClassName);
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

void DebuggerDlgRefresh(HWND h)
{
	if (!h || !IsWindow(h)) return;
	DbgDlgState *st = GetState(h);
	if (!st || !st->hStatusbar) return;

	TCHAR buf[64];

	if (Settings.Paused)
		SendMessage(st->hStatusbar, SB_SETTEXT, 0, (LPARAM)TEXT("Paused"));
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

	InvalidateRect(h, NULL, FALSE);
}
