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
#include <shlobj.h>
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
#include "../sgb/acid_baseline.h"

extern HINSTANCE g_hInst;

namespace {

// Filter menu entries for the two fixed lists.
const char *kModelNames[]  = { "DMG", "CGB", "SGB" };
const char *kStatusNames[] = { "PASS", "FAIL", "INFO", "ERROR" };
// Slots 0-3 line up with Status; the rest are the runner's own states.
const char *kShowNames[]   = { "Passed", "Failed", "Informational", "Errors",
                               "Not run", "Differs from baseline",
                               "Missing from baseline" };
constexpr int kShowCount   = 7;
constexpr int kShowPending = 4;
constexpr int kShowDiffers = 5;
constexpr int kShowMissing = 6;

// Fixed list columns; one per baseline follows, then Detail last.
enum { kColNum, kColTest, kColModel, kColResult, kColBaseline };

struct AcidDlgState
{
	std::vector<AcidTests::Test> tests;
	// kPending / kRunning, else (int)AcidTests::Status
	std::vector<int>  status;
	// Verdict per test, including the captured frame. Meaningful once the
	// matching status is no longer pending/running.
	std::vector<AcidTests::Result> results;
	std::string       acid_dir;

	// Every folder under acid/baseline/, one list column each, and how each
	// test came out against each of them - indexed [test][baseline].
	std::vector<AcidTests::Baseline>           baselines;
	std::vector<std::vector<AcidTests::Match>> match;
	std::vector<std::vector<int>>              diff_px;
	int col_detail = kColBaseline;   // shifts right as baselines are found

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

bool HaveBaseline(const AcidDlgState *st)
{
	return !st->baselines.empty();
}

// Re-diff one test against every baseline.
void RecomputeMatch(AcidDlgState *st, int test)
{
	st->match[test].assign(st->baselines.size(), AcidTests::Match::None);
	st->diff_px[test].assign(st->baselines.size(), 0);
	for (size_t b = 0; b < st->baselines.size(); ++b)
		st->match[test][b] = AcidTests::CompareToBaseline(
			st->baselines[b], st->tests[test], st->results[test].shot,
			st->diff_px[test][b]);
}

// True when any baseline says so - what the Show filter asks.
bool MatchIsAny(const AcidDlgState *st, int test, AcidTests::Match want)
{
	for (AcidTests::Match m : st->match[test])
		if (m == want) return true;
	return false;
}

void BaselineText(const AcidDlgState *st, int test, size_t b, char *buf, size_t n)
{
	const AcidTests::Match m = b < st->match[test].size()
	                           ? st->match[test][b] : AcidTests::Match::None;
	switch (m)
	{
		case AcidTests::Match::Same:    snprintf(buf, n, "SAME");    break;
		case AcidTests::Match::NoImage: snprintf(buf, n, "MISSING"); break;
		case AcidTests::Match::NoFrame: snprintf(buf, n, "-");       break;
		case AcidTests::Match::Differs:
			snprintf(buf, n, "DIFF %d px", st->diff_px[test][b]);    break;
		default: buf[0] = 0;
	}
}

// Fill or clear every baseline cell on one row.
void SetBaselineCells(AcidDlgState *st, int row, int test, bool clear)
{
	char buf[64];
	for (size_t b = 0; b < st->baselines.size(); ++b)
	{
		if (clear) buf[0] = 0;
		else       BaselineText(st, test, b, buf, sizeof buf);
		SetItemText(st->hList, row, kColBaseline + (int)b, buf);
	}
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

// Checked entries are OR-ed, so Failed plus Differs shows both sets.
bool ShowStatus(const AcidDlgState *st, int test)
{
	if (!AnyOn(st->show_on)) return true;
	const int s = st->status[test];
	const int slot = (s == kPending || s == kRunning) ? kShowPending : s;
	if (st->show_on[slot]) return true;
	if (st->show_on[kShowDiffers] &&
	    MatchIsAny(st, test, AcidTests::Match::Differs)) return true;
	if (st->show_on[kShowMissing] &&
	    MatchIsAny(st, test, AcidTests::Match::NoImage)) return true;
	return false;
}

void InsertCol(AcidDlgState *st, int idx, const char *name, int width)
{
	Utf8ToWide wname(name);
	LVCOLUMN lvc = {};
	lvc.mask    = LVCF_TEXT | LVCF_WIDTH;
	lvc.pszText = (LPTSTR)(const TCHAR *)wname;
	lvc.cx      = width;
	ListView_InsertColumn(st->hList, idx, &lvc);
}

// Fixed columns, then one per baseline, then Detail. Rebuilt whenever the
// baseline folder is rescanned.
void RebuildColumns(AcidDlgState *st)
{
	while (ListView_DeleteColumn(st->hList, 0)) {}
	struct { const char *name; int width; } fixed[] = {
		{ "#",      34 },
		{ "Test",  330 },
		{ "Model",  44 },
		{ "Result", 50 },
	};
	int col = 0;
	for (const auto &f : fixed) InsertCol(st, col++, f.name, f.width);
	for (const AcidTests::Baseline &b : st->baselines)
		InsertCol(st, col++, b.name.c_str(), 88);
	st->col_detail = col;
	InsertCol(st, col, "Detail", 200);
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
		SetItemText(st->hList, (int)row, kColTest, t.name.c_str());
		SetItemText(st->hList, (int)row, kColModel, AcidTests::ModelName(t.model));

		const int s = st->status[i];
		SetItemText(st->hList, (int)row, kColResult,
		            s == kRunning ? "RUN" : s == kPending ? "" : kStatusNames[s]);
		SetBaselineCells(st, (int)row, i, false);
		buf[0] = 0;
		if (HasResult(st, i))
		{
			const AcidTests::Result &r = st->results[i];
			snprintf(buf, sizeof buf, "%d frames%s%s", r.frames,
			         r.detail.empty() ? "" : "; ", r.detail.c_str());
		}
		SetItemText(st->hList, (int)row, st->col_detail, buf);
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
std::vector<size_t> BaselinesWithFrame(const AcidDlgState *st, int test);
void RescanBaselines(AcidDlgState *st, bool quiet);

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
		int n = snprintf(buf, sizeof buf, "%s\r\n%s",
		                 st->tests[st->shown].name.c_str(),
		                 s == kRunning ? "running..." :
		                 s == kPending ? "not run yet" : kStatusNames[s]);
		// Frames are stacked in this order, so name them in it too.
		const std::vector<size_t> drawn = BaselinesWithFrame(st, st->shown);
		if (n > 0 && n < (int)sizeof buf && !drawn.empty())
			n += snprintf(buf + n, sizeof buf - n, "\r\nours on top");
		for (size_t b : drawn)
		{
			if (n <= 0 || n >= (int)sizeof buf) break;
			char verdict[64];
			BaselineText(st, st->shown, b, verdict, sizeof verdict);
			n += snprintf(buf + n, sizeof buf - n, "\r\n%s: %s",
			              st->baselines[b].name.c_str(), verdict);
		}
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

// One frame drawn into x,y,w,h. The core hands back R,G,B; a DIB wants
// B,G,R.
void BlitShot(HDC dc, const std::vector<uint8_t> &rgb, int x, int y, int w, int h)
{
	static std::vector<uint8_t> bgr;
	bgr = rgb;
	for (size_t i = 0; i + 2 < bgr.size(); i += 3)
		std::swap(bgr[i], bgr[i + 2]);

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth       = AcidTests::kShotWidth;
	bi.bmiHeader.biHeight      = -AcidTests::kShotHeight;   // top-down
	bi.bmiHeader.biPlanes      = 1;
	bi.bmiHeader.biBitCount    = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(dc, COLORONCOLOR);   // keep the pixels crisp
	StretchDIBits(dc, x, y, w, h,
	              0, 0, AcidTests::kShotWidth, AcidTests::kShotHeight,
	              bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// Baselines with a frame for this test, in column order. NoImage is the
// only state that means there is nothing on disk to show.
std::vector<size_t> BaselinesWithFrame(const AcidDlgState *st, int test)
{
	std::vector<size_t> out;
	if (test < 0 || test >= (int)st->tests.size()) return out;
	for (size_t b = 0; b < st->baselines.size(); ++b)
		if (b < st->match[test].size() &&
		    st->match[test][b] != AcidTests::Match::NoImage &&
		    st->match[test][b] != AcidTests::Match::None)
			out.push_back(b);
	return out;
}

// Our frame with each baseline's stacked under it, at the largest whole
// scale that fits - or shrunk to fit when there are too many for 1:1.
void PaintShot(AcidDlgState *st, DRAWITEMSTRUCT *dis)
{
	RECT rc = dis->rcItem;
	FillRect(dis->hDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

	std::vector<std::vector<uint8_t>> frames;
	if (const std::vector<uint8_t> *ours = ShotOf(st, st->shown))
		frames.push_back(*ours);
	for (size_t b : BaselinesWithFrame(st, st->shown))
	{
		std::vector<uint8_t> ref;
		if (AcidTests::LoadBaselineFrame(st->baselines[b], st->tests[st->shown], ref))
			frames.push_back(std::move(ref));
	}
	if (frames.empty())
	{
		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, RGB(140, 140, 140));
		DrawText(dis->hDC, TEXT("No screenshot"), -1, &rc,
		         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	const int gap = 4, n = (int)frames.size();
	const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	const int availh = ch - gap * (n - 1);
	int w, h;
	// Not std::min: windows.h has already made min a macro.
	int scale = cw / AcidTests::kShotWidth;
	const int vscale = availh / (AcidTests::kShotHeight * n);
	if (vscale < scale) scale = vscale;
	if (scale >= 1)
	{
		w = AcidTests::kShotWidth * scale;
		h = AcidTests::kShotHeight * scale;
	}
	else
	{
		// Too many for 1:1 - fit them rather than clip the last ones away.
		h = availh / n;
		if (h < 1) h = 1;
		w = AcidTests::kShotWidth * h / AcidTests::kShotHeight;
		if (w > cw)
		{
			w = cw;
			h = AcidTests::kShotHeight * w / AcidTests::kShotWidth;
		}
	}

	const int x = rc.left + (cw - w) / 2;
	int y = rc.top + (ch - (h * n + gap * (n - 1))) / 2;
	for (const std::vector<uint8_t> &f : frames)
	{
		BlitShot(dis->hDC, f, x, y, w, h);
		y += h + gap;
	}
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
	st->match[i].assign(st->baselines.size(), AcidTests::Match::None);
	const int row = st->row_of[i];
	if (row >= 0)
	{
		SetItemText(st->hList, row, kColResult, "RUN");
		SetBaselineCells(st, row, i, true);
		SetItemText(st->hList, row, st->col_detail, "");
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
	SetItemText(st->hList, st->row_of[i], st->col_detail, det);
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
	RecomputeMatch(st, i);   // fills the Baseline column live during a run

	const int row = st->row_of[i];
	if (row >= 0)
	{
		SetItemText(st->hList, row, kColResult, kStatusNames[(int)result.status]);
		SetBaselineCells(st, row, i, false);
		char det[288];
		snprintf(det, sizeof det, "%d frames%s%s", result.frames,
		         result.detail.empty() ? "" : "; ", result.detail.c_str());
		SetItemText(st->hList, row, st->col_detail, det);
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
	{
		AcidTests::ReportRow row;
		row.test    = &st->tests[i];
		row.result  = HasResult(st, i) ? &st->results[i] : nullptr;
		row.match   = st->match[i];
		row.diff_px = st->diff_px[i];
		rows.push_back(std::move(row));
	}
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
	if (HaveBaseline(st)) info.baselines = &st->baselines;
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
  Baselines
--------------------------------------------------------------------------*/

// Open the browser already sitting on `start`, so a new baseline lands
// beside the others by default.
int CALLBACK BrowseInitProc(HWND hwnd, UINT msg, LPARAM, LPARAM data)
{
	if (msg == BFFM_INITIALIZED && data)
		SendMessage(hwnd, BFFM_SETSELECTION, TRUE, data);
	return 0;
}

bool AskFolder(HWND hDlg, const TCHAR *title, const TCHAR *start,
               TCHAR *path, int len)
{
	path[0] = 0;
	TCHAR display[MAX_PATH] = { 0 };
	BROWSEINFO bi = {};
	bi.hwndOwner      = hDlg;
	bi.pszDisplayName = display;
	bi.lpszTitle      = title;
	bi.ulFlags        = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
	if (start && start[0])
	{
		bi.lpfn   = BrowseInitProc;
		bi.lParam = (LPARAM)start;
	}
	LPITEMIDLIST idl = SHBrowseForFolder(&bi);
	if (!idl) return false;
	const BOOL got = SHGetPathFromIDList(idl, path);
	LPMALLOC mal = NULL;
	if (SUCCEEDED(SHGetMalloc(&mal)) && mal) mal->Free(idl);
	if (!got) return false;
	path[len - 1] = 0;
	return path[0] != 0;
}

void SaveBaseline(AcidDlgState *st)
{
	// A folder dropped in here is picked up as a column on the next scan.
	Utf8ToWide wroot(AcidTests::BaselinePath(st->acid_dir, "").c_str());
	TCHAR dir[MAX_PATH];
	if (!AskFolder(st->hDlg,
	               TEXT("Save the captured frames as a baseline in a new folder under acid\\baseline:"),
	               wroot, dir, MAX_PATH))
		return;

	WideToUtf8 udir(dir);
	const std::vector<AcidTests::ReportRow> rows = ReportRows(st);

	// Saving into a folder that already holds frames replaces them, and the
	// index goes with them even for tests this save does not cover.
	const AcidTests::BaselineClash clash =
		AcidTests::CheckBaseline((const char *)udir, rows);
	if (clash.Any())
	{
		std::string msg = "Baseline already exists, override?\r\n\r\n";
		msg += (const char *)udir;
		msg += "\r\n";
		if (clash.images)
		{
			char n[64];
			snprintf(n, sizeof n, "Replaces %d frame%s. ", clash.images,
			         clash.images == 1 ? "" : "s");
			msg += n;
		}
		if (clash.index)
		{
			msg += "Replaces the existing index";
			std::string who = clash.title;
			if (!clash.created.empty())
				who += who.empty() ? clash.created : ", " + clash.created;
			if (!who.empty()) msg += " (" + who + ")";
			msg += ".";
		}
		Utf8ToWide wmsg(msg.c_str());
		if (MessageBox(st->hDlg, wmsg, TEXT("Save baseline"),
		               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			return;
	}

	std::string err;
	const int n = AcidTests::WriteBaseline((const char *)udir, rows,
	                                       MakeReportInfo(st), err);
	if (n < 0)
	{
		Utf8ToWide werr(err.c_str());
		MessageBox(st->hDlg, werr, TEXT("Save baseline"), MB_OK | MB_ICONERROR);
		return;
	}
	char buf[MAX_PATH + 64];
	snprintf(buf, sizeof buf, "Saved %d frame%s to %s", n, n == 1 ? "" : "s",
	         (const char *)udir);
	SetCtrlText(st->hStat, buf);
	MessageBox(st->hDlg, TEXT("Test baseline saved!"), TEXT("Save baseline"),
	           MB_OK | MB_ICONINFORMATION);
	// Saved under acid/baseline? Then it is a column now.
	RescanBaselines(st, true);
	SetCtrlText(st->hStat, buf);
}

void RecomputeAllMatches(AcidDlgState *st)
{
	for (size_t i = 0; i < st->tests.size(); ++i) RecomputeMatch(st, (int)i);
}

void RebuildColumns(AcidDlgState *st);

// Pick up whatever is in acid/baseline/ now, rebuild the columns and
// re-diff against the new set.
void RescanBaselines(AcidDlgState *st, bool quiet)
{
	st->baselines = AcidTests::DiscoverBaselines(st->acid_dir.c_str());
	if (!HaveBaseline(st))
	{
		// Those two only make sense while a baseline does; left set they
		// would go on hiding rows with nothing to explain why.
		st->show_on[kShowDiffers] = 0;
		st->show_on[kShowMissing] = 0;
	}
	RebuildColumns(st);
	RecomputeAllMatches(st);
	ApplyFilter(st);
	InvalidateRect(st->hShot, NULL, TRUE);
	UpdateShotCaption(st);
	if (quiet) return;

	std::string msg = "Baselines: ";
	if (!HaveBaseline(st)) msg += "none in " +
		AcidTests::BaselinePath(st->acid_dir, "");
	for (size_t b = 0; b < st->baselines.size(); ++b)
	{
		int same = 0, differs = 0, missing = 0;
		for (int i : st->rows)
		{
			if (b >= st->match[i].size()) continue;
			if      (st->match[i][b] == AcidTests::Match::Same)    ++same;
			else if (st->match[i][b] == AcidTests::Match::Differs) ++differs;
			else if (st->match[i][b] == AcidTests::Match::NoImage) ++missing;
		}
		char one[192];
		snprintf(one, sizeof one, "%s%s: %d same, %d differ, %d missing",
		         b ? "   " : "", st->baselines[b].name.c_str(), same, differs,
		         missing);
		msg += one;
	}
	SetCtrlText(st->hStat, msg.c_str());
}

void BaselineMenu(AcidDlgState *st)
{
	bool any_shot = false;
	for (int i : st->rows)
		if (!st->results[i].shot.empty()) { any_shot = true; break; }

	RECT rc;
	GetWindowRect(GetDlgItem(st->hDlg, IDC_ACID_BASELINE), &rc);
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	AppendMenu(menu, MF_STRING | (any_shot ? MF_ENABLED : MF_GRAYED), 1,
	           TEXT("&Save shown frames as a baseline..."));
	AppendMenu(menu, MF_SEPARATOR, 0, NULL);
	AppendMenu(menu, MF_STRING, 2, TEXT("&Rescan acid\\baseline"));
	const int cmd = (int)TrackPopupMenu(menu,
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN |
		TPM_LEFTBUTTON, rc.left, rc.top, 0, st->hDlg, NULL);
	DestroyMenu(menu);

	if      (cmd == 1) SaveBaseline(st);
	else if (cmd == 2) RescanBaselines(st, false);
}

/*--------------------------------------------------------------------------
  The run
--------------------------------------------------------------------------*/

void EnableFilterBar(AcidDlgState *st, BOOL on)
{
	const int ids[] = { IDC_ACID_SEARCH, IDC_ACID_SUITES, IDC_ACID_MODELS,
	                    IDC_ACID_SHOW, IDC_ACID_CLEAR, IDC_ACID_EXPORT,
	                    IDC_ACID_BASELINE };
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
		st->match[i].assign(st->baselines.size(), AcidTests::Match::None);
		SetItemText(st->hList, st->row_of[i], kColResult, "");
		SetBaselineCells(st, st->row_of[i], i, true);
		SetItemText(st->hList, st->row_of[i], st->col_detail, "");
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
			SetItemText(st->hList, st->row_of[i], kColResult, "");
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

	// A by-result or by-baseline filter now hides or reveals rows the run
	// just changed.
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

		st->acid_dir = ResolveAcidDir();
		std::string err;
		bool loaded = AcidTests::LoadManifest(st->acid_dir.c_str(), st->tests, err);
		st->status.assign(st->tests.size(), kPending);
		st->results.assign(st->tests.size(), AcidTests::Result());
		st->match.assign(st->tests.size(), std::vector<AcidTests::Match>());
		st->diff_px.assign(st->tests.size(), std::vector<int>());
		st->suites   = AcidTests::SuitesOf(st->tests);
		st->suite_on.assign(st->suites.size(), 0);
		st->model_on.assign(3, 0);
		st->show_on.assign(kShowCount, 0);

		// Builds the columns and applies the filter as a side effect.
		RescanBaselines(st, true);
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
				const std::vector<std::string> names(kShowNames,
					kShowNames + (HaveBaseline(st) ? kShowCount : kShowPending + 1));
				CheckMenu(st, IDC_ACID_SHOW, names, st->show_on, "All results");
			}
			return TRUE;
		}
		case IDC_ACID_BASELINE:
		{
			AcidDlgState *st = GetState(hDlg);
			if (st && !st->running) BaselineMenu(st);
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
