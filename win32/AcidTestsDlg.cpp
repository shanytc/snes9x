// Emulation > Acid Tests — GB Emulator Shootout runner (see sgb/acid.h).
// Modal dialog; the suite runs on the dialog thread and pumps messages from
// the progress callback so the UI stays live and Cancel works.

#include <windows.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "AcidTestsDlg.h"
#include "wsnes9x.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../sgb/acid.h"

extern HINSTANCE g_hInst;

namespace {

struct AcidDlgState
{
	std::vector<AcidTests::Test> tests;
	std::vector<int>  status;      // -1 pending, else (int)AcidTests::Status
	std::string       acid_dir;
	HWND hDlg = NULL, hList = NULL, hProg = NULL, hStat = NULL;
	bool running   = false;
	bool cancelled = false;
};

AcidDlgState *GetState(HWND hDlg)
{
	return (AcidDlgState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

// The acid folder ships at the repo root; try next to the exe, one level up
// (running from win32 build output), then the working directory.
std::string ResolveAcidDir()
{
	char exe[MAX_PATH];
	if (GetModuleFileNameA(NULL, exe, MAX_PATH))
	{
		std::string dir(exe);
		size_t slash = dir.find_last_of("\\/");
		if (slash != std::string::npos)
		{
			dir.resize(slash);
			const char *cands[] = { "\\acid", "\\..\\acid", "\\..\\..\\acid" };
			for (const char *c : cands)
			{
				std::string p = dir + c;
				DWORD a = GetFileAttributesA((p + "\\manifest.txt").c_str());
				if (a != INVALID_FILE_ATTRIBUTES) return p;
			}
		}
	}
	return "acid";
}

void SetItemText(HWND hList, int row, int col, const char *text)
{
	Utf8ToWide wtext(text);
	LVITEM lvi = {};
	lvi.iSubItem = col;
	lvi.pszText  = (LPTSTR)(const TCHAR *)wtext;
	SendMessage(hList, LVM_SETITEMTEXT, row, (LPARAM)&lvi);
}

void PopulateList(AcidDlgState *st)
{
	ListView_DeleteAllItems(st->hList);
	char buf[32];
	for (size_t i = 0; i < st->tests.size(); ++i)
	{
		const AcidTests::Test &t = st->tests[i];
		LVITEM lvi = {};
		lvi.mask    = LVIF_TEXT;
		lvi.iItem   = (int)i;
		snprintf(buf, sizeof buf, "%d", (int)i + 1);
		Utf8ToWide wnum(buf);
		lvi.pszText = (LPTSTR)(const TCHAR *)wnum;
		ListView_InsertItem(st->hList, &lvi);
		SetItemText(st->hList, (int)i, 1, t.name.c_str());
		SetItemText(st->hList, (int)i, 2,
			t.model == AcidTests::Model::CGB ? "CGB" :
			t.model == AcidTests::Model::SGB ? "SGB" : "DMG");
		SetItemText(st->hList, (int)i, 3, "");
		SetItemText(st->hList, (int)i, 4, "");
	}
}

void PumpMessages(AcidDlgState *st)
{
	MSG msg;
	while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
	{
		if (!IsDialogMessage(st->hDlg, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}

bool AcidProgress(void *user, int test_index, int test_count,
                  const AcidTests::Test &test, int frames_done, int frames_total)
{
	AcidDlgState *st = (AcidDlgState *)user;
	char buf[512];
	snprintf(buf, sizeof buf, "Running %d/%d: %s  (frame %d/%d)",
	         test_index + 1, test_count, test.name.c_str(),
	         frames_done, frames_total);
	Utf8ToWide wbuf(buf);
	SetWindowText(st->hStat, wbuf);
	SendMessage(st->hProg, PBM_SETPOS, test_index, 0);
	if (frames_done == 0)
		ListView_EnsureVisible(st->hList, test_index, FALSE);
	PumpMessages(st);
	return !st->cancelled;
}

void AcidResult(void *user, int test_index, const AcidTests::Test &test,
                const AcidTests::Result &result)
{
	AcidDlgState *st = (AcidDlgState *)user;
	if (test_index < 0 || test_index >= (int)st->status.size()) return;
	st->status[test_index] = (int)result.status;
	const char *txt = "?";
	switch (result.status)
	{
		case AcidTests::Status::Pass:  txt = "PASS";  break;
		case AcidTests::Status::Fail:  txt = "FAIL";  break;
		case AcidTests::Status::Info:  txt = "INFO";  break;
		case AcidTests::Status::Error: txt = "ERROR"; break;
	}
	SetItemText(st->hList, test_index, 3, txt);
	char det[256];
	snprintf(det, sizeof det, "%d frames%s%s", result.frames,
	         result.detail.empty() ? "" : "; ",
	         result.detail.empty() ? "" : result.detail.c_str());
	SetItemText(st->hList, test_index, 4, det);
	ListView_RedrawItems(st->hList, test_index, test_index);
}

void RunSuite(AcidDlgState *st)
{
	if (st->running) return;
	st->running   = true;
	st->cancelled = false;
	std::fill(st->status.begin(), st->status.end(), -1);
	PopulateList(st);
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), FALSE);
	SendMessage(st->hProg, PBM_SETRANGE32, 0, (LPARAM)st->tests.size());
	SendMessage(st->hProg, PBM_SETPOS, 0, 0);

	AcidTests::RunOptions opts;
	opts.acid_dir  = st->acid_dir.c_str();
	opts.progress  = AcidProgress;
	opts.on_result = AcidResult;
	opts.user      = st;
	AcidTests::Summary sum = AcidTests::Run(opts);

	char buf[256];
	if (sum.cancelled)
		snprintf(buf, sizeof buf, "Cancelled: %d/%d passed so far (%d failed, %d info, %d errors)",
		         sum.passed, sum.passed + sum.failed, sum.failed, sum.info, sum.errors);
	else
		snprintf(buf, sizeof buf, "Done: %d/%d passed (%d failed, %d info, %d errors) — results.txt written",
		         sum.passed, sum.passed + sum.failed, sum.failed, sum.info, sum.errors);
	Utf8ToWide wbuf(buf);
	SetWindowText(st->hStat, wbuf);
	SendMessage(st->hProg, PBM_SETPOS, (WPARAM)st->tests.size(), 0);
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), TRUE);
	st->running = false;
}

INT_PTR CALLBACK AcidDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		AcidDlgState *st = new AcidDlgState;
		SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);
		st->hDlg  = hDlg;
		st->hList = GetDlgItem(hDlg, IDC_ACID_LIST);
		st->hProg = GetDlgItem(hDlg, IDC_ACID_PROGRESS);
		st->hStat = GetDlgItem(hDlg, IDC_ACID_STATUS);

		ListView_SetExtendedListViewStyle(st->hList,
			LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		struct { const TCHAR *name; int width; } cols[] = {
			{ TEXT("#"),      34 },
			{ TEXT("Test"),  330 },
			{ TEXT("Model"),  44 },
			{ TEXT("Result"), 50 },
			{ TEXT("Detail"),200 },
		};
		for (int i = 0; i < 5; ++i)
		{
			LVCOLUMN lvc = {};
			lvc.mask     = LVCF_TEXT | LVCF_WIDTH;
			lvc.pszText  = (LPTSTR)cols[i].name;
			lvc.cx       = cols[i].width;
			ListView_InsertColumn(st->hList, i, &lvc);
		}

		st->acid_dir = ResolveAcidDir();
		std::string err;
		if (!AcidTests::LoadManifest(st->acid_dir.c_str(), st->tests, err))
		{
			Utf8ToWide werr(("Cannot load manifest: " + err).c_str());
			SetWindowText(st->hStat, werr);
			EnableWindow(GetDlgItem(hDlg, IDC_ACID_RUN), FALSE);
		}
		else
		{
			char buf[128];
			snprintf(buf, sizeof buf, "%d tests loaded from %s",
			         (int)st->tests.size(), st->acid_dir.c_str());
			Utf8ToWide wbuf(buf);
			SetWindowText(st->hStat, wbuf);
		}
		st->status.assign(st->tests.size(), -1);
		PopulateList(st);
		return TRUE;
	}

	case WM_NOTIFY:
	{
		NMHDR *nm = (NMHDR *)lParam;
		if (nm->idFrom == IDC_ACID_LIST && nm->code == NM_CUSTOMDRAW)
		{
			NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lParam;
			AcidDlgState *st = GetState(hDlg);
			switch (cd->nmcd.dwDrawStage)
			{
			case CDDS_PREPAINT:
				SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
				return TRUE;
			case CDDS_ITEMPREPAINT:
			{
				int row = (int)cd->nmcd.dwItemSpec;
				int s   = (st && row < (int)st->status.size())
				          ? st->status[row] : -1;
				if      (s == (int)AcidTests::Status::Pass)
					cd->clrText = RGB(0, 128, 0);
				else if (s == (int)AcidTests::Status::Fail ||
				         s == (int)AcidTests::Status::Error)
					cd->clrText = RGB(192, 0, 0);
				else if (s == (int)AcidTests::Status::Info)
					cd->clrText = RGB(0, 0, 192);
				SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
				return TRUE;
			}
			}
		}
		break;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_ACID_RUN:
			RunSuite(GetState(hDlg));
			return TRUE;
		case IDCANCEL:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st && st->running)
			{
				st->cancelled = true;   // progress callback ends the run
				return TRUE;
			}
			EndDialog(hDlg, 0);
			return TRUE;
		}
		}
		break;

	case WM_CLOSE:
	{
		AcidDlgState *st = GetState(hDlg);
		if (st && st->running)
		{
			st->cancelled = true;
			return TRUE;
		}
		EndDialog(hDlg, 0);
		return TRUE;
	}

	case WM_DESTROY:
	{
		AcidDlgState *st = GetState(hDlg);
		SetWindowLongPtr(hDlg, DWLP_USER, 0);
		delete st;
		return TRUE;
	}
	}
	return FALSE;
}

} // anonymous

void WinShowAcidTestsDialog()
{
	DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ACID_TESTS), GUI.hWnd, AcidDlgProc);
}
