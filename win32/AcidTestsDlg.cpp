// Emulation > Acid Tests — GB Emulator Shootout runner (see sgb/acid.h).
// Modal dialog. The tests are independent ROMs, so the runner keeps one
// emulator core busy per worker thread, each picking up the next test as it
// finishes; the Threads box picks how many and defaults to the machine's
// core count. Rows show RUN while a worker holds them. Start, result and
// progress callbacks all arrive on this thread, which pumps messages so the
// list stays live and Cancel works.
//
// The filter bar narrows the list the way the shootout's --test/--model
// flags do, plus a by-result filter for re-running failures; Run covers
// whatever is shown, and Export writes that same set as .txt/.json/.html.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
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
#include "../sgb/acid_report.h"

extern HINSTANCE g_hInst;

namespace {

// Filter menu entries for the two fixed lists.
const char *kModelNames[]  = { "DMG", "CGB", "SGB" };
const char *kStatusNames[] = { "PASS", "FAIL", "INFO", "ERROR" };
const char *kShowNames[]   = { "Passed", "Failed", "Informational", "Errors",
                               "Not run" };
constexpr int kShowPending = 4;   // the extra "Not run" row

struct AcidDlgState
{
	std::vector<AcidTests::Test> tests;
	// kPending / kRunning, else (int)AcidTests::Status
	std::vector<int>  status;
	// Verdict per test, including the captured frame. Meaningful once the
	// matching status is no longer pending/running.
	std::vector<AcidTests::Result> results;
	std::string       acid_dir;

	// Filter state. An all-clear vector means "no restriction".
	std::string              search;
	std::vector<std::string> suites;    // manifest order
	std::vector<char>        suite_on;
	std::vector<char>        model_on;  // DMG / CGB / SGB
	std::vector<char>        show_on;   // pass / fail / info / error / not run

	std::vector<int>  rows;     // list row -> test index
	std::vector<int>  row_of;   // test index -> list row, -1 when hidden
	std::vector<int>  run_map;  // index handed to the callbacks -> test index

	HWND hDlg = NULL, hList = NULL, hProg = NULL, hStat = NULL, hThreads = NULL;
	HWND hShot = NULL, hShotCap = NULL, hPause = NULL, hSearch = NULL;
	bool running   = false;
	bool cancelled = false;
	bool close_when_done = false;   // Close pressed mid-run
	std::atomic<bool> paused{false};
	int  threads   = 1;
	int  in_flight = 0;   // tests currently held by a worker
	int  shown     = -1;  // test the preview pane is showing
	DWORD started  = 0;   // GetTickCount at the start of the run
	double last_secs = 0.0;   // wall clock of the last finished run
};

constexpr int kPending = -1;
constexpr int kRunning = -2;

AcidDlgState *GetState(HWND hDlg)
{
	return (AcidDlgState *)GetWindowLongPtr(hDlg, DWLP_USER);
}

// The pack unpacks next to the exe; also accept one or two levels up so a
// build tree finds the copy at the repo root. Empty when it is not there.
std::string FindAcidDir()
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
	return std::string();
}

std::string ResolveAcidDir()
{
	const std::string found = FindAcidDir();
	return found.empty() ? std::string("acid") : found;
}

void SetItemText(HWND hList, int row, int col, const char *text)
{
	Utf8ToWide wtext(text);
	LVITEM lvi = {};
	lvi.iSubItem = col;
	lvi.pszText  = (LPTSTR)(const TCHAR *)wtext;
	SendMessage(hList, LVM_SETITEMTEXT, row, (LPARAM)&lvi);
}

void SetCtrlText(HWND ctrl, const char *text)
{
	Utf8ToWide wtext(text);
	SetWindowText(ctrl, wtext);
}

bool AnyOn(const std::vector<char> &v)
{
	return std::find(v.begin(), v.end(), 1) != v.end();
}

// True once a test has a verdict to show or export.
bool HasResult(const AcidDlgState *st, int test)
{
	return st->status[test] >= 0;
}

/*--------------------------------------------------------------------------
  Filtering
--------------------------------------------------------------------------*/

// Name/suite/model half of the filter, in the form the core and the report
// headers understand.
AcidTests::Filter CoreFilter(const AcidDlgState *st)
{
	AcidTests::Filter f;
	f.text = st->search;
	for (size_t i = 0; i < st->suites.size(); ++i)
		if (st->suite_on[i]) f.suites.push_back(st->suites[i]);
	for (int i = 0; i < 3; ++i)
		if (st->model_on[i]) f.models |= AcidTests::ModelBit((AcidTests::Model)i);
	return f;
}

std::string FilterDescription(const AcidDlgState *st)
{
	std::string d = CoreFilter(st).Describe();
	if (!AnyOn(st->show_on)) return d;
	std::string list;
	for (size_t i = 0; i < st->show_on.size(); ++i)
	{
		if (!st->show_on[i]) continue;
		if (!list.empty()) list += "+";
		list += kShowNames[i];
	}
	if (d == "all tests") return "showing " + list;
	return d + ", showing " + list;
}

bool ShowStatus(const AcidDlgState *st, int test)
{
	if (!AnyOn(st->show_on)) return true;
	const int s = st->status[test];
	const int slot = (s == kPending || s == kRunning) ? kShowPending : s;
	return st->show_on[slot] != 0;
}

void PopulateList(AcidDlgState *st)
{
	SetWindowRedraw(st->hList, FALSE);
	ListView_DeleteAllItems(st->hList);
	char buf[288];
	for (size_t row = 0; row < st->rows.size(); ++row)
	{
		const int i = st->rows[row];
		const AcidTests::Test &t = st->tests[i];
		LVITEM lvi = {};
		lvi.mask    = LVIF_TEXT;
		lvi.iItem   = (int)row;
		snprintf(buf, sizeof buf, "%d", i + 1);
		Utf8ToWide wnum(buf);
		lvi.pszText = (LPTSTR)(const TCHAR *)wnum;
		ListView_InsertItem(st->hList, &lvi);
		SetItemText(st->hList, (int)row, 1, t.name.c_str());
		SetItemText(st->hList, (int)row, 2, AcidTests::ModelName(t.model));

		const int s = st->status[i];
		SetItemText(st->hList, (int)row, 3,
		            s == kRunning ? "RUN" : s == kPending ? "" : kStatusNames[s]);
		buf[0] = 0;
		if (HasResult(st, i))
		{
			const AcidTests::Result &r = st->results[i];
			snprintf(buf, sizeof buf, "%d frames%s%s", r.frames,
			         r.detail.empty() ? "" : "; ", r.detail.c_str());
		}
		SetItemText(st->hList, (int)row, 4, buf);
	}
	SetWindowRedraw(st->hList, TRUE);
}

// Button captions say what is selected: the name when it is a single pick,
// otherwise a count.
void UpdateFilterButtons(AcidDlgState *st)
{
	struct { int id; const char *base; const std::vector<char> *on;
	         const char *const *names; } bars[] = {
		{ IDC_ACID_SUITES, "Suites", &st->suite_on, nullptr },
		{ IDC_ACID_MODELS, "Models", &st->model_on, kModelNames },
		{ IDC_ACID_SHOW,   "Show",   &st->show_on,  kShowNames  },
	};
	char buf[128];
	for (const auto &b : bars)
	{
		const int n = (int)std::count(b.on->begin(), b.on->end(), 1);
		if (n == 0)
			snprintf(buf, sizeof buf, "%s: all", b.base);
		else if (n == 1)
		{
			const size_t k = std::find(b.on->begin(), b.on->end(), 1) - b.on->begin();
			const char *name = b.names ? b.names[k] : st->suites[k].c_str();
			snprintf(buf, sizeof buf, "%s: %s", b.base, name);
		}
		else
			snprintf(buf, sizeof buf, "%s: %d", b.base, n);
		SetCtrlText(GetDlgItem(st->hDlg, b.id), buf);
	}
}

void UpdateShotCaption(AcidDlgState *st);
void SelectShot(AcidDlgState *st, int test);

// Recompute which tests are listed and rebuild the view around them.
void ApplyFilter(AcidDlgState *st)
{
	const AcidTests::Filter f = CoreFilter(st);
	st->rows.clear();
	st->row_of.assign(st->tests.size(), -1);
	for (size_t i = 0; i < st->tests.size(); ++i)
	{
		if (!f.Matches(st->tests[i]) || !ShowStatus(st, (int)i)) continue;
		st->row_of[i] = (int)st->rows.size();
		st->rows.push_back((int)i);
	}
	PopulateList(st);
	UpdateFilterButtons(st);

	const bool filtered = st->rows.size() != st->tests.size();
	SetCtrlText(GetDlgItem(st->hDlg, IDC_ACID_RUN),
	            filtered ? "&Run Shown" : "&Run All");
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), !st->rows.empty());

	if (!st->running)
	{
		char buf[256];
		if (filtered)
			snprintf(buf, sizeof buf, "%d of %d tests shown (%s)",
			         (int)st->rows.size(), (int)st->tests.size(),
			         FilterDescription(st).c_str());
		else
			snprintf(buf, sizeof buf, "%d tests loaded from %s",
			         (int)st->tests.size(), st->acid_dir.c_str());
		SetCtrlText(st->hStat, buf);
	}

	// Keep the preview on its test when it survived the filter.
	if (st->shown >= 0 && st->row_of[st->shown] >= 0)
		ListView_SetItemState(st->hList, st->row_of[st->shown],
		                      LVIS_SELECTED | LVIS_FOCUSED,
		                      LVIS_SELECTED | LVIS_FOCUSED);
}

// Popup check-list under `btn`, reopening after each pick so several boxes
// can be ticked in one go. The first item clears the lot.
void CheckMenu(AcidDlgState *st, int btn_id, const std::vector<std::string> &items,
               std::vector<char> &on, const char *all_text)
{
	RECT rc;
	GetWindowRect(GetDlgItem(st->hDlg, btn_id), &rc);
	for (;;)
	{
		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		AppendMenu(menu, MF_STRING | (AnyOn(on) ? MF_UNCHECKED : MF_CHECKED), 1,
		           Utf8ToWide(all_text));
		AppendMenu(menu, MF_SEPARATOR, 0, NULL);
		for (size_t i = 0; i < items.size(); ++i)
			AppendMenu(menu, MF_STRING | (on[i] ? MF_CHECKED : MF_UNCHECKED),
			           (UINT_PTR)(i + 2), Utf8ToWide(items[i].c_str()));
		const int cmd = (int)TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN |
			TPM_LEFTBUTTON, rc.left, rc.bottom, 0, st->hDlg, NULL);
		DestroyMenu(menu);
		if (cmd == 0) break;
		if (cmd == 1) std::fill(on.begin(), on.end(), (char)0);
		else          on[cmd - 2] = !on[cmd - 2];
		ApplyFilter(st);
	}
}

/*--------------------------------------------------------------------------
  Preview pane
--------------------------------------------------------------------------*/

const std::vector<uint8_t> *ShotOf(AcidDlgState *st, int test)
{
	if (test < 0 || test >= (int)st->results.size()) return nullptr;
	const std::vector<uint8_t> &s = st->results[test].shot;
	return s.empty() ? nullptr : &s;
}

void UpdateShotCaption(AcidDlgState *st)
{
	char buf[512];
	if (st->shown < 0 || st->shown >= (int)st->tests.size())
		buf[0] = 0;
	else
	{
		const int s = st->status[st->shown];
		snprintf(buf, sizeof buf, "%s\r\n%s", st->tests[st->shown].name.c_str(),
		         s == kRunning ? "running..." :
		         s == kPending ? "not run yet" : kStatusNames[s]);
	}
	SetCtrlText(st->hShotCap, buf);
}

void SelectShot(AcidDlgState *st, int test)
{
	if (test == st->shown) return;
	st->shown = test;
	UpdateShotCaption(st);
	InvalidateRect(st->hShot, NULL, TRUE);
}

// 160x144 scaled by the largest whole factor that fits, centred.
void PaintShot(AcidDlgState *st, DRAWITEMSTRUCT *dis)
{
	RECT rc = dis->rcItem;
	FillRect(dis->hDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

	const std::vector<uint8_t> *rgb = ShotOf(st, st->shown);
	if (!rgb)
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

	// The core hands back R,G,B; a DIB wants B,G,R.
	static std::vector<uint8_t> bgr;
	bgr = *rgb;
	for (size_t i = 0; i + 2 < bgr.size(); i += 3)
		std::swap(bgr[i], bgr[i + 2]);

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
	              bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

/*--------------------------------------------------------------------------
  Runner callbacks
--------------------------------------------------------------------------*/

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

// Callbacks index the set that was handed to the runner, not the manifest.
int TestOf(AcidDlgState *st, int run_index)
{
	return (run_index >= 0 && run_index < (int)st->run_map.size())
	       ? st->run_map[run_index] : -1;
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
	SetCtrlText(st->hStat, buf);
	SendMessage(st->hProg, PBM_SETPOS, done, 0);
	PumpMessages(st);
	return !st->cancelled;
}

// A worker just picked this test up: mark the row so all N in flight are
// visible at once.
void AcidStart(void *user, int run_index, const AcidTests::Test &)
{
	AcidDlgState *st = (AcidDlgState *)user;
	const int i = TestOf(st, run_index);
	if (i < 0) return;
	st->status[i] = kRunning;
	++st->in_flight;
	st->results[i] = AcidTests::Result();
	const int row = st->row_of[i];
	if (row >= 0)
	{
		SetItemText(st->hList, row, 3, "RUN");
		SetItemText(st->hList, row, 4, "");
		ListView_RedrawItems(st->hList, row, row);
	}
	if (i == st->shown) { UpdateShotCaption(st); InvalidateRect(st->hShot, NULL, TRUE); }
}

// Still going: show it advancing so a long ROM doesn't look wedged.
void AcidRunning(void *user, int run_index, int frames_done, int frames_total)
{
	AcidDlgState *st = (AcidDlgState *)user;
	const int i = TestOf(st, run_index);
	if (i < 0 || st->status[i] != kRunning || st->row_of[i] < 0) return;
	char det[64];
	snprintf(det, sizeof det, "frame %d/%d", frames_done, frames_total);
	SetItemText(st->hList, st->row_of[i], 4, det);
}

void AcidResult(void *user, int run_index, const AcidTests::Test &,
                const AcidTests::Result &result)
{
	AcidDlgState *st = (AcidDlgState *)user;
	const int i = TestOf(st, run_index);
	if (i < 0) return;
	if (st->status[i] == kRunning && st->in_flight > 0) --st->in_flight;
	st->status[i]  = (int)result.status;
	st->results[i] = result;

	const int row = st->row_of[i];
	if (row >= 0)
	{
		SetItemText(st->hList, row, 3, kStatusNames[(int)result.status]);
		char det[288];
		snprintf(det, sizeof det, "%d frames%s%s", result.frames,
		         result.detail.empty() ? "" : "; ", result.detail.c_str());
		SetItemText(st->hList, row, 4, det);
		ListView_RedrawItems(st->hList, row, row);
	}

	if (st->shown < 0) SelectShot(st, i);
	else if (i == st->shown)
	{
		UpdateShotCaption(st);
		InvalidateRect(st->hShot, NULL, TRUE);
	}
}

/*--------------------------------------------------------------------------
  Reports
--------------------------------------------------------------------------*/

// Every test currently shown, with its verdict when it has one.
std::vector<AcidTests::ReportRow> ReportRows(AcidDlgState *st)
{
	std::vector<AcidTests::ReportRow> rows;
	rows.reserve(st->rows.size());
	for (int i : st->rows)
		rows.push_back({ &st->tests[i], HasResult(st, i) ? &st->results[i] : nullptr });
	return rows;
}

AcidTests::ReportInfo MakeReportInfo(AcidDlgState *st)
{
	AcidTests::ReportInfo info;
	info.env     = AcidTests::EnvOverrides();
	info.filter  = FilterDescription(st);
	info.source  = st->acid_dir;
	info.threads = st->threads;
	info.seconds = st->last_secs;
	return info;
}

bool WriteReportTo(const TCHAR *path, AcidTests::Format fmt, AcidDlgState *st)
{
	const std::string doc =
		AcidTests::RenderReport(fmt, ReportRows(st), MakeReportInfo(st));
	FILE *f = _tfopen(path, TEXT("wb"));
	if (!f) return false;
	const bool ok = fwrite(doc.data(), 1, doc.size(), f) == doc.size();
	fclose(f);
	return ok;
}

// Save-As for the export. No lpstrDefExt: the extension comes from what the
// user typed, or from the filter they picked, so the format always matches.
bool AskExportPath(HWND hDlg, TCHAR *path, int len, AcidTests::Format &fmt)
{
	static const TCHAR kFilter[] =
		TEXT("HTML report (*.html)\0*.html\0")
		TEXT("JSON (*.json)\0*.json\0")
		TEXT("Text (*.txt)\0*.txt\0")
		TEXT("All files (*.*)\0*.*\0");
	_tcsncpy(path, TEXT("acid-results.html"), len - 1);
	path[len - 1] = 0;

	OPENFILENAME ofn = {};
	ofn.lStructSize  = sizeof ofn;
	ofn.hwndOwner    = hDlg;
	ofn.lpstrFilter  = kFilter;
	ofn.nFilterIndex = 1;
	ofn.lpstrFile    = path;
	ofn.nMaxFile     = len;
	ofn.Flags        = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
	if (!GetSaveFileName(&ofn)) return false;

	static const TCHAR *kExt[] = { TEXT(".html"), TEXT(".json"), TEXT(".txt"),
	                               TEXT(".html") };
	const TCHAR *dot = _tcsrchr(path, TEXT('.'));
	const bool known = dot && (!_tcsicmp(dot, TEXT(".html")) ||
	                           !_tcsicmp(dot, TEXT(".htm"))  ||
	                           !_tcsicmp(dot, TEXT(".json")) ||
	                           !_tcsicmp(dot, TEXT(".txt")));
	if (!known)
	{
		const int idx = (ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= 4)
		                ? (int)ofn.nFilterIndex - 1 : 0;
		if ((int)_tcslen(path) + 6 < len) _tcscat(path, kExt[idx]);
	}
	dot = _tcsrchr(path, TEXT('.'));
	fmt = (dot && !_tcsicmp(dot, TEXT(".json"))) ? AcidTests::Format::Json
	    : (dot && !_tcsicmp(dot, TEXT(".txt")))  ? AcidTests::Format::Text
	    : AcidTests::Format::Html;

	if (GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES &&
	    MessageBox(hDlg, TEXT("That file already exists. Overwrite it?"),
	               TEXT("Export results"), MB_YESNO | MB_ICONQUESTION) != IDYES)
		return false;
	return true;
}

void ExportResults(AcidDlgState *st)
{
	TCHAR path[MAX_PATH];
	AcidTests::Format fmt = AcidTests::Format::Html;
	if (!AskExportPath(st->hDlg, path, MAX_PATH, fmt)) return;

	if (WriteReportTo(path, fmt, st))
	{
		char buf[MAX_PATH + 64];
		WideToUtf8 upath(path);
		snprintf(buf, sizeof buf, "Exported %d test%s to %s",
		         (int)st->rows.size(), st->rows.size() == 1 ? "" : "s",
		         (const char *)upath);
		SetCtrlText(st->hStat, buf);
	}
	else
		MessageBox(st->hDlg, TEXT("Could not write that file."),
		           TEXT("Export results"), MB_OK | MB_ICONERROR);
}

/*--------------------------------------------------------------------------
  The run
--------------------------------------------------------------------------*/

void EnableFilterBar(AcidDlgState *st, BOOL on)
{
	const int ids[] = { IDC_ACID_SEARCH, IDC_ACID_SUITES, IDC_ACID_MODELS,
	                    IDC_ACID_SHOW, IDC_ACID_CLEAR, IDC_ACID_EXPORT };
	for (int id : ids) EnableWindow(GetDlgItem(st->hDlg, id), on);
}

void RunSuite(AcidDlgState *st)
{
	if (st->running || st->rows.empty()) return;

	// Only what is shown runs; verdicts outside the set are left alone so
	// re-running just the failures keeps the rest of the table.
	std::vector<AcidTests::Test> subset;
	st->run_map = st->rows;
	subset.reserve(st->run_map.size());
	for (int i : st->run_map)
	{
		subset.push_back(st->tests[i]);
		st->status[i]  = kPending;
		st->results[i] = AcidTests::Result();
		SetItemText(st->hList, st->row_of[i], 3, "");
		SetItemText(st->hList, st->row_of[i], 4, "");
	}

	st->running   = true;
	st->cancelled = false;
	st->in_flight = 0;
	st->shown     = -1;
	st->paused.store(false);
	SetWindowText(st->hPause, TEXT("&Pause"));
	EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), FALSE);
	EnableWindow(st->hThreads, FALSE);
	EnableWindow(st->hPause, TRUE);
	EnableFilterBar(st, FALSE);
	SendMessage(st->hProg, PBM_SETRANGE32, 0, (LPARAM)subset.size());
	SendMessage(st->hProg, PBM_SETPOS, 0, 0);
	UpdateShotCaption(st);
	InvalidateRect(st->hShot, NULL, TRUE);

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
	opts.report    = nullptr;   // written below, with the filter described
	// Failing frames land in <acid dir>\_failures so they can be diffed
	// against the reference rather than eyeballed in the preview pane.
	opts.dump_failures = true;
	AcidTests::Summary sum = AcidTests::RunTests(subset, opts);

	// Cancelling drops whatever the workers were holding, so those rows
	// never get a verdict — put them back to pending.
	for (int i : st->run_map)
	{
		if (st->status[i] != kRunning) continue;
		st->status[i] = kPending;
		if (st->row_of[i] >= 0)
		{
			SetItemText(st->hList, st->row_of[i], 3, "");
			ListView_RedrawItems(st->hList, st->row_of[i], st->row_of[i]);
		}
	}
	st->in_flight = 0;
	st->last_secs = (GetTickCount() - st->started) / 1000.0;
	st->running   = false;

	const std::string report = st->acid_dir + "\\results.txt";
	std::string err;
	const bool wrote = AcidTests::WriteReport(report.c_str(),
		AcidTests::Format::Text, ReportRows(st), MakeReportInfo(st), err);

	char buf[256];
	// Informational tests have no reference image to match, so they count
	// as passing: a clean run is every test in the manifest, 264/264.
	const int ok = sum.passed + sum.info;
	if (sum.cancelled)
		snprintf(buf, sizeof buf, "Cancelled: %d/%d passed so far (%d failed, %d info, %d errors)",
		         ok, sum.total, sum.failed, sum.info, sum.errors);
	else
		snprintf(buf, sizeof buf,
		         "Done in %.1fs on %d thread%s: %d/%d passed (%d failed, %d info, %d errors)%s",
		         st->last_secs, st->threads, st->threads == 1 ? "" : "s",
		         ok, sum.total, sum.failed, sum.info, sum.errors,
		         wrote ? " — results.txt written" : "");
	SetCtrlText(st->hStat, buf);
	SendMessage(st->hProg, PBM_SETPOS, (WPARAM)subset.size(), 0);
	EnableWindow(st->hThreads, TRUE);
	EnableWindow(st->hPause, FALSE);
	SetWindowText(st->hPause, TEXT("&Pause"));
	EnableFilterBar(st, TRUE);
	UpdateShotCaption(st);

	// A by-result filter now hides or reveals rows the run just changed.
	if (AnyOn(st->show_on) && !st->close_when_done)
	{
		const std::string status = buf;
		ApplyFilter(st);
		SetCtrlText(st->hStat, status.c_str());
	}
	else
		EnableWindow(GetDlgItem(st->hDlg, IDC_ACID_RUN), TRUE);

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
		st->hSearch  = GetDlgItem(hDlg, IDC_ACID_SEARCH);
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
		bool loaded = AcidTests::LoadManifest(st->acid_dir.c_str(), st->tests, err);
		st->status.assign(st->tests.size(), kPending);
		st->results.assign(st->tests.size(), AcidTests::Result());
		st->suites   = AcidTests::SuitesOf(st->tests);
		st->suite_on.assign(st->suites.size(), 0);
		st->model_on.assign(3, 0);
		st->show_on.assign(5, 0);

		ApplyFilter(st);
		if (!loaded)
		{
			SetCtrlText(st->hStat, ("Cannot load manifest: " + err).c_str());
			EnableWindow(GetDlgItem(hDlg, IDC_ACID_RUN), FALSE);
			EnableFilterBar(st, FALSE);
		}
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
			if (st && (lv->uNewState & LVIS_SELECTED) &&
			    lv->iItem >= 0 && lv->iItem < (int)st->rows.size())
				SelectShot(st, st->rows[lv->iItem]);
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
				const int row = (int)cd->nmcd.dwItemSpec;
				const int s = (st && row < (int)st->rows.size())
				              ? st->status[st->rows[row]] : kPending;
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
		case IDC_ACID_SEARCH:
		{
			AcidDlgState *st = GetState(hDlg);
			if (!st || HIWORD(wParam) != EN_CHANGE || st->running) return TRUE;
			TCHAR text[128];
			GetWindowText(st->hSearch, text, 128);
			WideToUtf8 utf8(text);
			st->search = (const char *)utf8;
			ApplyFilter(st);
			return TRUE;
		}
		case IDC_ACID_SUITES:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st) CheckMenu(st, IDC_ACID_SUITES, st->suites, st->suite_on,
			                  "All suites");
			return TRUE;
		}
		case IDC_ACID_MODELS:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st)
			{
				const std::vector<std::string> names(kModelNames, kModelNames + 3);
				CheckMenu(st, IDC_ACID_MODELS, names, st->model_on, "All models");
			}
			return TRUE;
		}
		case IDC_ACID_SHOW:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st)
			{
				const std::vector<std::string> names(kShowNames, kShowNames + 5);
				CheckMenu(st, IDC_ACID_SHOW, names, st->show_on, "All results");
			}
			return TRUE;
		}
		case IDC_ACID_CLEAR:
		{
			AcidDlgState *st = GetState(hDlg);
			if (!st || st->running) return TRUE;
			st->search.clear();
			std::fill(st->suite_on.begin(), st->suite_on.end(), (char)0);
			std::fill(st->model_on.begin(), st->model_on.end(), (char)0);
			std::fill(st->show_on.begin(), st->show_on.end(), (char)0);
			SetWindowText(st->hSearch, TEXT(""));   // re-enters as EN_CHANGE
			ApplyFilter(st);
			return TRUE;
		}
		case IDC_ACID_EXPORT:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st && !st->running) ExportResults(st);
			return TRUE;
		}
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

bool WinAcidTestsAvailable()
{
	return !FindAcidDir().empty();
}

void WinShowAcidTestsDialog()
{
	DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ACID_TESTS), GUI.hWnd, AcidDlgProc);
}
