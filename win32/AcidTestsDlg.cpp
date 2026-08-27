// Emulation > Acid Tests — GB Emulator Shootout runner (see sgb/acid.h).
// Modal dialog. The tests are independent ROMs, so the runner keeps one
// emulator core busy per worker thread, each picking up the next test as it
// finishes; the Threads box picks how many and defaults to the machine's
// core count. Rows show RUN while a worker holds them. Start, result and
// progress callbacks all arrive on this thread, which pumps messages so the
// list stays live and Cancel works.

#include <windows.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
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
	// kPending / kRunning, else (int)AcidTests::Status
	std::vector<int>  status;
	std::string       acid_dir;
	// One captured frame per test, BGR triplets ready for StretchDIBits.
	// Empty until that test has produced one.
	std::vector<std::vector<uint8_t>> shots;
	HWND hDlg = NULL, hList = NULL, hProg = NULL, hStat = NULL, hThreads = NULL;
	HWND hShot = NULL, hShotCap = NULL, hPause = NULL;
	bool running   = false;
	bool cancelled = false;
	bool close_when_done = false;   // Close pressed mid-run
	std::atomic<bool> paused{false};
	int  threads   = 1;
	int  in_flight = 0;   // tests currently held by a worker
	int  shown     = -1;  // row the preview pane is showing
	DWORD started  = 0;   // GetTickCount at the start of the run
};

constexpr int kPending = -1;
constexpr int kRunning = -2;

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

// Show whichever row is selected in the preview pane.
void UpdateShotCaption(AcidDlgState *st)
{
	char buf[512];
	if (st->shown < 0 || st->shown >= (int)st->tests.size())
		buf[0] = 0;
	else
	{
		static const char *kName[] = { "PASS", "FAIL", "INFO", "ERROR" };
		const int s = st->status[st->shown];
		snprintf(buf, sizeof buf, "%s\r\n%s", st->tests[st->shown].name.c_str(),
		         s == kRunning ? "running..." :
		         s == kPending ? "not run yet" : kName[s]);
	}
	Utf8ToWide wbuf(buf);
	SetWindowText(st->hShotCap, wbuf);
}

void SelectShot(AcidDlgState *st, int row)
{
	if (row == st->shown) return;
	st->shown = row;
	UpdateShotCaption(st);
	InvalidateRect(st->hShot, NULL, TRUE);
}

// 160x144 scaled by the largest whole factor that fits, centred.
void PaintShot(AcidDlgState *st, DRAWITEMSTRUCT *dis)
{
	RECT rc = dis->rcItem;
	FillRect(dis->hDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

	const bool have = st->shown >= 0 && st->shown < (int)st->shots.size() &&
	                  !st->shots[st->shown].empty();
	if (!have)
	{
		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, RGB(140, 140, 140));
		DrawText(dis->hDC, TEXT("No screenshot"), -1, &rc,
		         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	int scale = cw / AcidTests::kShotWidth;
	const int vscale = ch / AcidTests::kShotHeight;
	if (vscale < scale) scale = vscale;
	if (scale < 1) scale = 1;
	const int w = AcidTests::kShotWidth * scale, h = AcidTests::kShotHeight * scale;
	const int x = rc.left + (cw - w) / 2, y = rc.top + (ch - h) / 2;

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth       = AcidTests::kShotWidth;
	bi.bmiHeader.biHeight      = -AcidTests::kShotHeight;   // top-down
	bi.bmiHeader.biPlanes      = 1;
	bi.bmiHeader.biBitCount    = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(dis->hDC, COLORONCOLOR);   // keep the pixels crisp
	StretchDIBits(dis->hDC, x, y, w, h,
	              0, 0, AcidTests::kShotWidth, AcidTests::kShotHeight,
	              st->shots[st->shown].data(), &bi, DIB_RGB_COLORS, SRCCOPY);
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

bool AcidProgress(void *user, int done, int test_count,
                  const AcidTests::Test &, int, int)
{
	AcidDlgState *st = (AcidDlgState *)user;
	if (st->close_when_done) { PumpMessages(st); return false; }
	const double secs = (GetTickCount() - st->started) / 1000.0;
	char buf[256];
	snprintf(buf, sizeof buf, "%d/%d done, %d running on %d thread%s — %.1fs%s",
	         done, test_count, st->in_flight, st->threads,
	         st->threads == 1 ? "" : "s", secs,
	         st->paused.load() ? " — PAUSED" : "");
	Utf8ToWide wbuf(buf);
	SetWindowText(st->hStat, wbuf);
	SendMessage(st->hProg, PBM_SETPOS, done, 0);
	PumpMessages(st);
	return !st->cancelled;
}

// A worker just picked this test up: mark the row so all N in flight are
// visible at once.
void AcidStart(void *user, int test_index, const AcidTests::Test &)
{
	AcidDlgState *st = (AcidDlgState *)user;
	if (test_index < 0 || test_index >= (int)st->status.size()) return;
	st->status[test_index] = kRunning;
	++st->in_flight;
	SetItemText(st->hList, test_index, 3, "RUN");
	SetItemText(st->hList, test_index, 4, "");
	st->shots[test_index].clear();
	ListView_RedrawItems(st->hList, test_index, test_index);
	if (test_index == st->shown) { UpdateShotCaption(st); InvalidateRect(st->hShot, NULL, TRUE); }
}

// Still going: show it advancing so a long ROM doesn't look wedged.
void AcidRunning(void *user, int test_index, int frames_done, int frames_total)
{
	AcidDlgState *st = (AcidDlgState *)user;
	if (test_index < 0 || test_index >= (int)st->status.size()) return;
	if (st->status[test_index] != kRunning) return;
	char det[64];
	snprintf(det, sizeof det, "frame %d/%d", frames_done, frames_total);
	SetItemText(st->hList, test_index, 4, det);
}

void AcidResult(void *user, int test_index, const AcidTests::Test &test,
                const AcidTests::Result &result)
{
	AcidDlgState *st = (AcidDlgState *)user;
	if (test_index < 0 || test_index >= (int)st->status.size()) return;
	if (st->status[test_index] == kRunning && st->in_flight > 0) --st->in_flight;
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

	if (test_index < (int)st->shots.size())
	{
		// R,G,B from the core; a DIB wants B,G,R.
		std::vector<uint8_t> bgr = result.shot;
		for (size_t i = 0; i + 2 < bgr.size(); i += 3)
			std::swap(bgr[i], bgr[i + 2]);
		st->shots[test_index] = std::move(bgr);
	}
	if (st->shown < 0) SelectShot(st, test_index);
	else if (test_index == st->shown)
	{
		UpdateShotCaption(st);
		InvalidateRect(st->hShot, NULL, TRUE);
	}
}

void RunSuite(AcidDlgState *st)
{
	if (st->running) return;
	st->running   = true;
	st->cancelled = false;
	std::fill(st->status.begin(), st->status.end(), kPending);
	st->shots.assign(st->tests.size(), std::vector<uint8_t>());
	st->in_flight = 0;
	st->shown     = -1;
	st->paused.store(false);
	SetWindowText(st->hPause, TEXT("&Pause"));
	PopulateList(st);
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), FALSE);
	EnableWindow(st->hThreads, FALSE);
	EnableWindow(st->hPause, TRUE);
	SendMessage(st->hProg, PBM_SETRANGE32, 0, (LPARAM)st->tests.size());
	SendMessage(st->hProg, PBM_SETPOS, 0, 0);

	const int sel = (int)SendMessage(st->hThreads, CB_GETCURSEL, 0, 0);
	st->threads = (sel == CB_ERR) ? 1 : sel + 1;
	st->started = GetTickCount();

	AcidTests::RunOptions opts;
	opts.acid_dir  = st->acid_dir.c_str();
	opts.progress  = AcidProgress;
	opts.on_result = AcidResult;
	opts.on_start  = AcidStart;
	opts.on_running = AcidRunning;
	opts.user      = st;
	opts.threads   = st->threads;
	opts.pause     = &st->paused;
	// Failing frames land in <acid dir>\_failures so they can be diffed
	// against the reference rather than eyeballed in the preview pane.
	opts.dump_failures = true;
	AcidTests::Summary sum = AcidTests::Run(opts);

	// Cancelling drops whatever the workers were holding, so those rows
	// never get a verdict — put them back to pending.
	for (size_t i = 0; i < st->status.size(); ++i)
	{
		if (st->status[i] != kRunning) continue;
		st->status[i] = kPending;
		SetItemText(st->hList, (int)i, 3, "");
		ListView_RedrawItems(st->hList, (int)i, (int)i);
	}
	st->in_flight = 0;

	const double secs = (GetTickCount() - st->started) / 1000.0;
	char buf[256];
	if (sum.cancelled)
		snprintf(buf, sizeof buf, "Cancelled: %d/%d passed so far (%d failed, %d info, %d errors)",
		         sum.passed, sum.passed + sum.failed, sum.failed, sum.info, sum.errors);
	else
		snprintf(buf, sizeof buf,
		         "Done in %.1fs on %d thread%s: %d/%d passed (%d failed, %d info, %d errors) — results.txt written",
		         secs, st->threads, st->threads == 1 ? "" : "s",
		         sum.passed, sum.passed + sum.failed, sum.failed, sum.info, sum.errors);
	Utf8ToWide wbuf(buf);
	SetWindowText(st->hStat, wbuf);
	SendMessage(st->hProg, PBM_SETPOS, (WPARAM)st->tests.size(), 0);
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), TRUE);
	EnableWindow(st->hThreads, TRUE);
	EnableWindow(st->hPause, FALSE);
	SetWindowText(st->hPause, TEXT("&Pause"));
	st->running = false;
	UpdateShotCaption(st);

	// Close pressed mid-run: the workers are joined now, so it is safe.
	if (st->close_when_done)
		EndDialog(st->hDlg, 0);
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
		st->hThreads = GetDlgItem(hDlg, IDC_ACID_THREADS);
		st->hShot    = GetDlgItem(hDlg, IDC_ACID_SHOT);
		st->hShotCap = GetDlgItem(hDlg, IDC_ACID_SHOTCAP);
		st->hPause   = GetDlgItem(hDlg, IDC_ACID_PAUSE);
		EnableWindow(st->hPause, FALSE);

		// One core per hardware thread by default: the ROMs are
		// independent, so this is roughly a 5x saving on a 16-thread box.
		const int cores = AcidTests::DefaultThreadCount();
		for (int i = 1; i <= cores; ++i)
		{
			TCHAR num[16];
			_sntprintf(num, 16, TEXT("%d"), i);
			SendMessage(st->hThreads, CB_ADDSTRING, 0, (LPARAM)num);
		}
		SendMessage(st->hThreads, CB_SETCURSEL, cores - 1, 0);
		st->threads = cores;

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
		st->status.assign(st->tests.size(), kPending);
		st->shots.assign(st->tests.size(), std::vector<uint8_t>());
		PopulateList(st);
		return TRUE;
	}

	case WM_DRAWITEM:
	{
		DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
		AcidDlgState *st = GetState(hDlg);
		if (st && dis->CtlID == IDC_ACID_SHOT)
		{
			PaintShot(st, dis);
			return TRUE;
		}
		break;
	}

	case WM_NOTIFY:
	{
		NMHDR *nm = (NMHDR *)lParam;
		if (nm->idFrom == IDC_ACID_LIST && nm->code == LVN_ITEMCHANGED)
		{
			NMLISTVIEW *lv = (NMLISTVIEW *)lParam;
			AcidDlgState *st = GetState(hDlg);
			if (st && (lv->uNewState & LVIS_SELECTED))
				SelectShot(st, lv->iItem);
			break;
		}
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
				          ? st->status[row] : kPending;
				if      (s == kRunning)
					cd->clrText = RGB(176, 96, 0);
				else if (s == (int)AcidTests::Status::Pass)
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
		case IDC_ACID_PAUSE:
		{
			AcidDlgState *st = GetState(hDlg);
			if (!st || !st->running) return TRUE;
			const bool now = !st->paused.load();
			st->paused.store(now);
			SetWindowText(st->hPause, now ? TEXT("&Resume") : TEXT("&Pause"));
			return TRUE;
		}
		case IDCANCEL:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st && st->running)
			{
				// Stop the pool first — closing out from under the
				// workers would leave them writing to a freed dialog.
				st->close_when_done = true;
				st->cancelled = true;
				st->paused.store(false);   // let paused workers see it
				SetWindowText(st->hStat, TEXT("Stopping..."));
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
			st->close_when_done = true;
			st->cancelled = true;
			st->paused.store(false);
			SetWindowText(st->hStat, TEXT("Stopping..."));
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
