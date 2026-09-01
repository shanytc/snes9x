// Tests > Audit Tests - regression audit runner (see sgb/audit.h).
// Same shape as the Acid Tests dialog: a list the workers fill in live, a
// preview pane that flips ours against the baseline, threads picked in a
// combo box, and Save Baseline to pin the current captures as the new
// reference. One row per ROM; one column per boot combination, each cell
// PASS / DIFF n px / SLIP +n / skip, and a Verdict column that only reads
// PASS when every applicable combination matched the baseline.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "AuditTestsDlg.h"
#include "wsnes9x.h"
#include "win32_display.h"
#include "win32_sound.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../sgb/audit.h"

extern HINSTANCE g_hInst;

namespace {

using namespace AuditTests;

// Modeless dialog handle - one instance, NULL when closed.
HWND s_hAuditDlg = NULL;

// The status entries are OR-ed together; "SGB enhanced" is an attribute
// gate that ANDs with them (filter to SGB carts, in any state).
const char *kShowNames[] = { "Passed", "Failed", "Errors", "Not run",
                             "Missing from baseline", "SGB enhanced" };
constexpr int kShowCount  = 6;
constexpr int kShowPass = 0, kShowFail = 1, kShowError = 2, kShowPending = 3,
              kShowMissing = 4, kShowSgb = 5;

// Fixed columns; the combo cells follow, then Verdict and Detail.
enum { kColNum, kColRom, kColType, kColCart, kColSgb, kColBoot, kColLink, kColCombo };
constexpr int kColVerdict = kColCombo + kComboCount;
constexpr int kColDetail  = kColVerdict + 1;

constexpr int kPending = -1;
constexpr int kRunning = -2;
constexpr int kDone    = 0;

// Posted by the ROM scan thread: progress, then the finished list.
constexpr UINT WM_AUDIT_SCANPROG = WM_APP + 30;
constexpr UINT WM_AUDIT_SCANDONE = WM_APP + 31;

struct AuditDlgState
{
	std::string audit_dir, roms_dir;
	std::vector<Rom>       roms;
	std::vector<RomResult> results;
	std::vector<int>       status;    // kPending / kRunning / kDone
	std::map<std::string, std::string> linkmeta;

	std::vector<Baseline> baselines;
	int  base_sel = -1;               // active baseline; -1 = none (capture)

	std::string       search;
	std::vector<std::string> folders;
	std::vector<char> folder_on;
	std::vector<std::string> carttypes;
	std::vector<char> carttype_on;
	std::vector<char> show_on;

	std::vector<int> rows;      // list row -> rom index
	std::vector<int> row_of;    // rom index -> row, -1 hidden
	std::vector<int> run_map;   // run index -> rom index

	HWND hDlg = NULL, hList = NULL, hProg = NULL, hStat = NULL, hThreads = NULL;
	HWND hShot = NULL, hShotCap = NULL, hPause = NULL, hSearch = NULL;
	bool running = false, cancelled = false, close_when_done = false;
	bool rescan_after_run = false;   // a mid-run save wants the new column
	std::atomic<bool> paused{false};
	// Live thread dial - the combo box stays enabled mid-run and this is
	// what the workers follow.
	std::atomic<int>  threads_live{1};

	// The ROM scan - the dialog is up and usable the moment it opens, the
	// list arrives when the folder walk is done (like the acid baseline scan).
	std::thread       scan_thread;
	std::atomic<bool> scan_abort{false};
	bool              scanning = false;

	int   threads   = 1;
	int   shown     = -1;    // rom the preview is on
	int   shown_combo = -1;  // combo the preview is on; -1 = every combo
	int   shot_src  = 0;     // index into ShotSources
	// Sync Position: keep the preview on the same column (and ours/base
	// side) across ROM hops; a ROM without it falls back to the first.
	bool  sync_pos   = false;
	int   sync_combo = -1;
	bool  sync_ours  = true;
	DWORD started   = 0;
	// Wall time spent paused, kept out of the ETA math.
	DWORD pause_accum = 0, pause_since = 0;
};

AuditDlgState *GetState(HWND hDlg)
{
	return (AuditDlgState *)GetWindowLongPtr(hDlg, DWLP_USER);
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

// Both packs unpack next to the exe; accept a level or two up for a build
// tree. Empty when not found.
std::string FindDirNearExe(const char *name)
{
	char exe[MAX_PATH];
	if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return std::string();
	std::string dir(exe);
	const size_t slash = dir.find_last_of("\\/");
	if (slash == std::string::npos) return std::string();
	dir.resize(slash);
	const char *ups[] = { "\\", "\\..\\", "\\..\\..\\" };
	for (const char *u : ups)
	{
		const std::string p = dir + u + name;
		const DWORD a = GetFileAttributesA(p.c_str());
		if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
			return p;
	}
	return std::string();
}

const Baseline *ActiveBaseline(const AuditDlgState *st)
{
	return (st->base_sel >= 0 && st->base_sel < (int)st->baselines.size())
	       ? &st->baselines[st->base_sel] : nullptr;
}

/*--------------------------------------------------------------------------
  Cells
--------------------------------------------------------------------------*/

void CellText(const AuditDlgState *st, int rom, int combo, char *buf, size_t n)
{
	buf[0] = 0;
	if (!ComboApplies((Combo)combo, st->roms[rom])) { snprintf(buf, n, "-"); return; }
	if (st->status[rom] != kDone) return;
	const ComboResult &cr = st->results[rom].combos[combo];
	switch (cr.Cell())
	{
		case Match::Same:    snprintf(buf, n, "PASS"); break;
		case Match::Slip:    snprintf(buf, n, "SLIP %+d", cr.CellSlip()); break;
		case Match::Differs: snprintf(buf, n, "DIFF %d px", cr.CellDiff()); break;
		case Match::NoBase:  snprintf(buf, n, "no base"); break;
		case Match::Skip:    snprintf(buf, n, "skip"); break;
		case Match::Error:   snprintf(buf, n, "ERROR"); break;
		default:
			// Baseline-less capture run: it ran and holds shots.
			if (cr.ran) snprintf(buf, n, "captured");
			break;
	}
}

void VerdictText(const AuditDlgState *st, int rom, char *buf, size_t n)
{
	buf[0] = 0;
	if (st->status[rom] == kRunning) { snprintf(buf, n, "RUN"); return; }
	if (st->status[rom] != kDone) return;
	for (int c = 0; c < kComboCount; ++c)
		if (ComboApplies((Combo)c, st->roms[rom]) &&
		    st->results[rom].combos[c].Cell() == Match::Error)
		{ snprintf(buf, n, "ERROR"); return; }
	if (!ActiveBaseline(st)) { snprintf(buf, n, "captured"); return; }
	snprintf(buf, n, "%s", st->results[rom].AllPassed(st->roms[rom])
	                       ? "PASS" : "FAIL");
}

void DetailText(const AuditDlgState *st, int rom, char *buf, size_t n)
{
	buf[0] = 0;
	if (st->status[rom] != kDone) return;
	for (int c = 0; c < kComboCount; ++c)
	{
		const std::string &e = st->results[rom].combos[c].error;
		if (e.empty()) continue;
		snprintf(buf, n, "%s: %s", ComboId((Combo)c), e.c_str());
		return;
	}
}

void RefreshRow(AuditDlgState *st, int rom)
{
	const int row = st->row_of[rom];
	if (row < 0) return;
	char buf[192];
	for (int c = 0; c < kComboCount; ++c)
	{
		CellText(st, rom, c, buf, sizeof buf);
		SetItemText(st->hList, row, kColCombo + c, buf);
	}
	VerdictText(st, rom, buf, sizeof buf);
	SetItemText(st->hList, row, kColVerdict, buf);
	DetailText(st, rom, buf, sizeof buf);
	SetItemText(st->hList, row, kColDetail, buf);
	ListView_RedrawItems(st->hList, row, row);
}

// Right-click > Copy Info: every column of every selected row, tab-separated
// under a header line, so it pastes straight into a sheet or a bug report.
void CopySelectedRows(AuditDlgState *st)
{
	std::string text = "#\tROM\tType\tCart\tSGB\tBoot\tLink";
	for (int c = 0; c < kComboCount; ++c)
	{
		text += '\t';
		text += ComboName((Combo)c);
	}
	text += "\tVerdict\tDetail\r\n";

	char buf[192];
	int copied = 0;
	for (int row = ListView_GetNextItem(st->hList, -1, LVNI_SELECTED);
	     row >= 0; row = ListView_GetNextItem(st->hList, row, LVNI_SELECTED))
	{
		if (row >= (int)st->rows.size()) continue;
		const int i = st->rows[row];
		const Rom &r = st->roms[i];
		snprintf(buf, sizeof buf, "%d\t", i + 1);
		text += buf;
		text += r.name;
		text += r.gbc_file ? "\tGBC" : "\tGB";
		text += '\t'; text += r.cart_type;
		text += RomSgbEnhanced(r) ? "\tyes" : "\t-";
		text += '\t'; text += RomBootClass(r);
		text += '\t'; text += LinkLabelFor(st->linkmeta, r);
		for (int c = 0; c < kComboCount; ++c)
		{
			CellText(st, i, c, buf, sizeof buf);
			text += '\t'; text += buf;
		}
		VerdictText(st, i, buf, sizeof buf);
		text += '\t'; text += buf;
		DetailText(st, i, buf, sizeof buf);
		text += '\t'; text += buf;
		text += "\r\n";
		++copied;
	}
	if (!copied) return;

	Utf8ToWide wide(text.c_str());
	const wchar_t *w = wide;
	const size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
	if (!OpenClipboard(st->hDlg)) return;
	EmptyClipboard();
	if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
	{
		memcpy(GlobalLock(mem), w, bytes);
		GlobalUnlock(mem);
		SetClipboardData(CF_UNICODETEXT, mem);
	}
	CloseClipboard();

	snprintf(buf, sizeof buf, "Copied %d row%s to the clipboard.",
	         copied, copied == 1 ? "" : "s");
	SetCtrlText(st->hStat, buf);
}

/*--------------------------------------------------------------------------
  Filtering / list
--------------------------------------------------------------------------*/

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool ContainsFold(const std::string &hay, const std::string &needle)
{
	if (needle.empty()) return true;
	std::string h = hay, n = needle;
	for (char &c : h) c = Lower(c);
	for (char &c : n) c = Lower(c);
	return h.find(n) != std::string::npos;
}

// The active baseline holds nothing (or only part) of this ROM - the rows
// a resumed baseline build still owes.
bool MissingFromBaseline(const AuditDlgState *st, int i)
{
	const Baseline *b = ActiveBaseline(st);
	if (!b) return false;
	const Rom &r = st->roms[i];
	for (int c = 0; c < kComboCount; ++c)
	{
		if (!ComboApplies((Combo)c, r)) continue;
		if (!b->Find(r, (Combo)c)) return true;
	}
	return false;
}

bool ShowRom(const AuditDlgState *st, int i)
{
	const Rom &r = st->roms[i];
	if (!ContainsFold(r.name, st->search)) return false;
	if (AnyOn(st->folder_on))
	{
		bool on = false;
		for (size_t f = 0; f < st->folders.size(); ++f)
			if (st->folder_on[f] && st->folders[f] == r.folder) on = true;
		if (!on) return false;
	}
	if (AnyOn(st->carttype_on))
	{
		bool on = false;
		for (size_t t = 0; t < st->carttypes.size(); ++t)
			if (st->carttype_on[t] && st->carttypes[t] == r.cart_type) on = true;
		if (!on) return false;
	}
	if (st->show_on[kShowSgb] && !RomSgbEnhanced(r)) return false;
	bool any_status = false;
	for (int k = 0; k < kShowSgb; ++k)
		if (st->show_on[k]) any_status = true;
	if (!any_status) return true;

	// Checked status entries are OR-ed together.
	bool ok;
	if (st->status[i] != kDone)
		ok = st->show_on[kShowPending] != 0;
	else
	{
		bool err = false;
		for (int c = 0; c < kComboCount; ++c)
			if (st->results[i].combos[c].Cell() == Match::Error) err = true;
		if (err)                          ok = st->show_on[kShowError] != 0;
		else if (!ActiveBaseline(st))     ok = st->show_on[kShowPass] != 0;
		else ok = st->results[i].AllPassed(r) ? st->show_on[kShowPass] != 0
		                                      : st->show_on[kShowFail] != 0;
	}
	if (!ok && st->show_on[kShowMissing] && MissingFromBaseline(st, i))
		ok = true;
	return ok;
}

void InsertCol(AuditDlgState *st, int idx, const char *name, int width)
{
	Utf8ToWide wname(name);
	LVCOLUMN lvc = {};
	lvc.mask    = LVCF_TEXT | LVCF_WIDTH;
	lvc.pszText = (LPTSTR)(const TCHAR *)wname;
	lvc.cx      = width;
	ListView_InsertColumn(st->hList, idx, &lvc);
}

void RebuildColumns(AuditDlgState *st)
{
	while (ListView_DeleteColumn(st->hList, 0)) {}
	struct { const char *name; int width; } fixed[] = {
		{ "#",     34 },
		{ "ROM",  240 },
		{ "Type",  38 },
		{ "Cart",  60 },
		{ "SGB",   34 },
		{ "Boot",  56 },
		{ "Link",  64 },
	};
	int col = 0;
	for (const auto &f : fixed) InsertCol(st, col++, f.name, f.width);
	for (int c = 0; c < kComboCount; ++c)
		InsertCol(st, col++, ComboName((Combo)c), 62);
	InsertCol(st, col++, "Verdict", 52);
	InsertCol(st, col, "Detail", 180);
}

void PopulateList(AuditDlgState *st)
{
	SetWindowRedraw(st->hList, FALSE);
	ListView_DeleteAllItems(st->hList);
	char buf[192];
	for (size_t row = 0; row < st->rows.size(); ++row)
	{
		const int i = st->rows[row];
		const Rom &r = st->roms[i];
		LVITEM lvi = {};
		lvi.mask  = LVIF_TEXT;
		lvi.iItem = (int)row;
		snprintf(buf, sizeof buf, "%d", i + 1);
		Utf8ToWide wnum(buf);
		lvi.pszText = (LPTSTR)(const TCHAR *)wnum;
		ListView_InsertItem(st->hList, &lvi);
		SetItemText(st->hList, (int)row, kColRom, r.name.c_str());
		SetItemText(st->hList, (int)row, kColType, r.gbc_file ? "GBC" : "GB");
		SetItemText(st->hList, (int)row, kColCart, r.cart_type.c_str());
		SetItemText(st->hList, (int)row, kColSgb, RomSgbEnhanced(r) ? "yes" : "-");
		SetItemText(st->hList, (int)row, kColBoot, RomBootClass(r).c_str());
		SetItemText(st->hList, (int)row, kColLink,
		            LinkLabelFor(st->linkmeta, r).c_str());
		if (st->status[i] == kRunning)
			SetItemText(st->hList, (int)row, kColVerdict, "RUN");
	}
	SetWindowRedraw(st->hList, TRUE);
	for (size_t row = 0; row < st->rows.size(); ++row)
		RefreshRow(st, st->rows[row]);
}

void UpdateFilterButtons(AuditDlgState *st)
{
	char buf[128];
	const int nf = (int)std::count(st->folder_on.begin(), st->folder_on.end(), 1);
	if (nf == 0) snprintf(buf, sizeof buf, "Folders: all");
	else if (nf == 1)
	{
		const size_t k = std::find(st->folder_on.begin(), st->folder_on.end(),
		                           (char)1) - st->folder_on.begin();
		snprintf(buf, sizeof buf, "Folders: %s", st->folders[k].c_str());
	}
	else snprintf(buf, sizeof buf, "Folders: %d", nf);
	SetCtrlText(GetDlgItem(st->hDlg, IDC_AUDIT_FOLDERS), buf);

	const int nc = (int)std::count(st->carttype_on.begin(),
	                               st->carttype_on.end(), 1);
	if (nc == 0) snprintf(buf, sizeof buf, "Cart: all");
	else if (nc == 1)
	{
		const size_t k = std::find(st->carttype_on.begin(),
		                           st->carttype_on.end(),
		                           (char)1) - st->carttype_on.begin();
		snprintf(buf, sizeof buf, "Cart: %s", st->carttypes[k].c_str());
	}
	else snprintf(buf, sizeof buf, "Cart: %d", nc);
	SetCtrlText(GetDlgItem(st->hDlg, IDC_AUDIT_CARTTYPE), buf);

	const int ns = (int)std::count(st->show_on.begin(), st->show_on.end(), 1);
	if (ns == 0) snprintf(buf, sizeof buf, "Show: all");
	else if (ns == 1)
	{
		const size_t k = std::find(st->show_on.begin(), st->show_on.end(),
		                           (char)1) - st->show_on.begin();
		snprintf(buf, sizeof buf, "Show: %s", kShowNames[k]);
	}
	else snprintf(buf, sizeof buf, "Show: %d", ns);
	SetCtrlText(GetDlgItem(st->hDlg, IDC_AUDIT_SHOW), buf);

	const Baseline *b = ActiveBaseline(st);
	snprintf(buf, sizeof buf, "Baseline: %s", b ? b->name.c_str() : "(none)");
	SetCtrlText(GetDlgItem(st->hDlg, IDC_AUDIT_BASEPICK), buf);
}

void UpdateShotCaption(AuditDlgState *st);

void ApplyFilter(AuditDlgState *st)
{
	st->rows.clear();
	st->row_of.assign(st->roms.size(), -1);
	for (size_t i = 0; i < st->roms.size(); ++i)
	{
		if (!ShowRom(st, (int)i)) continue;
		st->row_of[i] = (int)st->rows.size();
		st->rows.push_back((int)i);
	}
	PopulateList(st);
	UpdateFilterButtons(st);

	const bool filtered = st->rows.size() != st->roms.size();
	SetCtrlText(GetDlgItem(st->hDlg, IDC_AUDIT_RUN),
	            filtered ? "&Run Shown" : "&Run All");
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUN), !st->rows.empty());
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUNSEL), !st->rows.empty());

	if (!st->running)
	{
		char buf[256];
		if (filtered)
			snprintf(buf, sizeof buf, "%d of %d ROMs shown",
			         (int)st->rows.size(), (int)st->roms.size());
		else
			snprintf(buf, sizeof buf, "%d ROMs from %s — %d baseline%s in %s",
			         (int)st->roms.size(), st->roms_dir.c_str(),
			         (int)st->baselines.size(),
			         st->baselines.size() == 1 ? "" : "s",
			         st->audit_dir.c_str());
		SetCtrlText(st->hStat, buf);
	}
	if (st->shown >= 0 && st->shown < (int)st->row_of.size() &&
	    st->row_of[st->shown] >= 0)
		ListView_SetItemState(st->hList, st->row_of[st->shown],
		                      LVIS_SELECTED | LVIS_FOCUSED,
		                      LVIS_SELECTED | LVIS_FOCUSED);
}

void CheckMenu(AuditDlgState *st, int btn_id,
               const std::vector<std::string> &items, std::vector<char> &on,
               const char *all_text)
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

// The baseline button picks which column of truth a run compares against;
// "(none)" runs capture-only, which is what a fresh baseline is saved from.
void PickBaseline(AuditDlgState *st)
{
	RECT rc;
	GetWindowRect(GetDlgItem(st->hDlg, IDC_AUDIT_BASEPICK), &rc);
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	AppendMenu(menu, MF_STRING | (st->base_sel < 0 ? MF_CHECKED : MF_UNCHECKED),
	           1, TEXT("(none - capture only)"));
	for (size_t i = 0; i < st->baselines.size(); ++i)
		AppendMenu(menu, MF_STRING |
		           ((int)i == st->base_sel ? MF_CHECKED : MF_UNCHECKED),
		           (UINT_PTR)(i + 2),
		           Utf8ToWide(st->baselines[i].name.c_str()));
	const int cmd = (int)TrackPopupMenu(menu,
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN |
		TPM_LEFTBUTTON, rc.left, rc.bottom, 0, st->hDlg, NULL);
	DestroyMenu(menu);
	if (cmd == 0) return;
	st->base_sel = cmd == 1 ? -1 : cmd - 2;
	ApplyFilter(st);
	UpdateShotCaption(st);
	InvalidateRect(st->hShot, NULL, TRUE);
}

/*--------------------------------------------------------------------------
  Preview pane - ours and the baseline, per shot, flipped in place
--------------------------------------------------------------------------*/

struct ShotSource
{
	int  combo;
	bool ours;                 // false = baseline image
	std::string label;
};

std::vector<ShotSource> ShotSources(const AuditDlgState *st, int rom)
{
	std::vector<ShotSource> out;
	if (rom < 0 || rom >= (int)st->roms.size()) return out;
	const Baseline *base = ActiveBaseline(st);
	for (int c = 0; c < kComboCount; ++c)
	{
		if (!ComboApplies((Combo)c, st->roms[rom])) continue;
		// Whatever has landed so far - mid-run the list grows as each
		// column finishes.
		const Capture &cap = st->results[rom].combos[c].shot;
		const BaselineEntry *be = base
			? base->Find(st->roms[rom], (Combo)c) : nullptr;
		char label[96];
		if (!cap.Empty())
		{
			snprintf(label, sizeof label, "%s (ours, f%d)",
			         ComboName((Combo)c), cap.frame);
			out.push_back({ c, true, label });
		}
		if (be)
		{
			snprintf(label, sizeof label, "%s (base, f%d)",
			         ComboName((Combo)c), be->frame);
			out.push_back({ c, false, label });
		}
	}
	return out;
}

bool LoadShotFrame(const AuditDlgState *st, int rom, const ShotSource &src,
                   std::vector<uint8_t> &rgb, int &w, int &h)
{
	if (src.ours)
	{
		const Capture &cap = st->results[rom].combos[src.combo].shot;
		if (!cap.Decode(rgb)) return false;
		w = cap.w; h = cap.h;
		return true;
	}
	const Baseline *base = ActiveBaseline(st);
	if (!base) return false;
	const BaselineEntry *be = base->Find(st->roms[rom], (Combo)src.combo);
	return be && LoadBaselineShot(*base, *be, rgb, w, h);
}

void UpdateShotNav(AuditDlgState *st)
{
	const BOOL on = ShotSources(st, st->shown).size() > 1;
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_SHOTPREV), on);
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_SHOTNEXT), on);
}

void UpdateShotCaption(AuditDlgState *st)
{
	char buf[512];
	if (st->shown < 0 || st->shown >= (int)st->roms.size())
		buf[0] = 0;
	else
	{
		const Rom &r = st->roms[st->shown];
		int n = snprintf(buf, sizeof buf, "%s", r.name.c_str());
		const std::vector<ShotSource> src = ShotSources(st, st->shown);
		n += snprintf(buf + n, sizeof buf - n, "\r\n%d capture%s",
		              (int)src.size(), src.size() == 1 ? "" : "s");
		// Verdict of whichever capture the pane is on right now.
		if (!src.empty())
		{
			const ShotSource &cur = src[st->shot_src % src.size()];
			const ComboResult &cr = st->results[st->shown].combos[cur.combo];
			char cell[64];
			CellText(st, st->shown, cur.combo, cell, sizeof cell);
			n += snprintf(buf + n, sizeof buf - n, "\r\n%s: %s",
			              ComboName((Combo)cur.combo), cell);
			const ShotVerdict &v = cr.verdict;
			if (v.m == Match::Differs)
				n += snprintf(buf + n, sizeof buf - n, "\r\n%d px moved",
				              v.diff_px);
			if (v.m == Match::Slip)
				n += snprintf(buf + n, sizeof buf - n, "\r\nslipped %+d frames",
				              v.slip);
		}
	}
	SetCtrlText(st->hShotCap, buf);
	UpdateShotNav(st);
}

// Sync Position: the remembered column (and ours/base side) in this ROM's
// capture list; the same column on the other side beats falling back.
int SyncShotIndex(const std::vector<ShotSource> &src, int combo, bool ours)
{
	for (int i = 0; i < (int)src.size(); ++i)
		if (src[i].combo == combo && src[i].ours == ours) return i;
	for (int i = 0; i < (int)src.size(); ++i)
		if (src[i].combo == combo) return i;
	return 0;
}

void RememberShotPos(AuditDlgState *st)
{
	const std::vector<ShotSource> src = ShotSources(st, st->shown);
	if (src.empty()) return;
	const ShotSource &cur = src[st->shot_src % (int)src.size()];
	st->sync_combo = cur.combo;
	st->sync_ours  = cur.ours;
}

void SelectShot(AuditDlgState *st, int rom, int combo)
{
	if (rom == st->shown && combo == st->shown_combo) return;
	st->shown = rom;
	st->shown_combo = combo;
	st->shot_src = 0;
	if (st->sync_pos && st->sync_combo >= 0)
	{
		const std::vector<ShotSource> src = ShotSources(st, rom);
		if (!src.empty())
			st->shot_src = SyncShotIndex(src, st->sync_combo, st->sync_ours);
	}
	UpdateShotCaption(st);
	InvalidateRect(st->hShot, NULL, TRUE);
}

void StepShot(AuditDlgState *st, int delta)
{
	const std::vector<ShotSource> src = ShotSources(st, st->shown);
	if (src.size() < 2) return;
	st->shot_src = (int)((st->shot_src + src.size() + delta) % src.size());
	RememberShotPos(st);
	InvalidateRect(st->hShot, NULL, TRUE);
	UpdateShotCaption(st);
}

void BlitShot(HDC dc, const std::vector<uint8_t> &rgb, int iw, int ih,
              int x, int y, int w, int h)
{
	static std::vector<uint8_t> bgr;
	bgr = rgb;
	for (size_t i = 0; i + 2 < bgr.size(); i += 3)
		std::swap(bgr[i], bgr[i + 2]);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth       = iw;
	bi.bmiHeader.biHeight      = -ih;
	bi.bmiHeader.biPlanes      = 1;
	bi.bmiHeader.biBitCount    = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(dc, COLORONCOLOR);
	StretchDIBits(dc, x, y, w, h, 0, 0, iw, ih,
	              bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void PaintShot(AuditDlgState *st, DRAWITEMSTRUCT *dis)
{
	RECT rc = dis->rcItem;
	FillRect(dis->hDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
	SetBkMode(dis->hDC, TRANSPARENT);

	const std::vector<ShotSource> src = ShotSources(st, st->shown);
	std::vector<uint8_t> frame;
	int iw = 0, ih = 0;
	const int pos = src.empty() ? 0 : st->shot_src % (int)src.size();
	if (src.empty() || !LoadShotFrame(st, st->shown, src[pos], frame, iw, ih))
	{
		SetTextColor(dis->hDC, RGB(140, 140, 140));
		DrawText(dis->hDC, TEXT("No capture"), -1, &rc,
		         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	HFONT font = (HFONT)SendMessage(st->hDlg, WM_GETFONT, 0, 0);
	HGDIOBJ old = font ? SelectObject(dis->hDC, font) : NULL;
	const int line = 14;
	RECT hr = { rc.left + 2, rc.top + 2, rc.right - 2, rc.top + 2 + line };
	SetTextColor(dis->hDC, RGB(230, 230, 230));
	Utf8ToWide whead(src[pos].label.c_str());
	DrawText(dis->hDC, whead, -1, &hr, DT_CENTER | DT_SINGLELINE);
	if (src.size() > 1)
	{
		char pos_text[32];
		snprintf(pos_text, sizeof pos_text, "%d / %d", pos + 1, (int)src.size());
		RECT pr = { rc.left + 2, rc.bottom - 2 - line, rc.right - 2, rc.bottom - 2 };
		SetTextColor(dis->hDC, RGB(150, 150, 150));
		Utf8ToWide wpos(pos_text);
		DrawText(dis->hDC, wpos, -1, &pr, DT_CENTER | DT_SINGLELINE);
	}
	if (old) SelectObject(dis->hDC, old);

	const int top = rc.top + line + 4, bot = rc.bottom - line - 4;
	const int cw = rc.right - rc.left, ch = bot - top;
	int scale_n = cw, scale_d = iw;   // fit, keeping aspect
	if (ch * iw < cw * ih) { scale_n = ch; scale_d = ih; }
	int w = iw * scale_n / scale_d, h = ih * scale_n / scale_d;
	if (w < 1) w = iw;
	if (h < 1) h = ih;
	BlitShot(dis->hDC, frame, iw, ih, rc.left + (cw - w) / 2,
	         top + (ch - h) / 2, w, h);
}

/*--------------------------------------------------------------------------
  Runner callbacks - all arrive on the dialog thread
--------------------------------------------------------------------------*/

void PumpMessages(AuditDlgState *st)
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

int RomOf(AuditDlgState *st, int run_index)
{
	return (run_index >= 0 && run_index < (int)st->run_map.size())
	       ? st->run_map[run_index] : -1;
}

bool AuditProgress(void *user, int done, int total)
{
	AuditDlgState *st = (AuditDlgState *)user;
	if (st->close_when_done) { PumpMessages(st); return false; }
	DWORD paused_ms = st->pause_accum;
	if (st->paused.load()) paused_ms += GetTickCount() - st->pause_since;
	const double secs = (GetTickCount() - st->started - paused_ms) / 1000.0;
	const int live = st->threads_live.load();
	const double pct = total > 0 ? done * 100.0 / total : 0.0;
	// ETA from the run's own average pace; meaningless until a few finish.
	char eta[48] = "";
	if (done >= 3 && done < total && secs > 1.0)
	{
		const int left = (int)(secs / done * (total - done) + 0.5);
		if (left >= 3600)
			snprintf(eta, sizeof eta, " — ETA %d:%02d:%02d",
			         left / 3600, (left / 60) % 60, left % 60);
		else
			snprintf(eta, sizeof eta, " — ETA %d:%02d", left / 60, left % 60);
	}
	char buf[256];
	snprintf(buf, sizeof buf, "%d/%d ROMs (%.1f%%) on %d thread%s — %.1fs%s%s",
	         done, total, pct, live, live == 1 ? "" : "s", secs, eta,
	         st->paused.load() ? " — PAUSED" : "");
	SetCtrlText(st->hStat, buf);
	SendMessage(st->hProg, PBM_SETPOS, done, 0);
	PumpMessages(st);
	return !st->cancelled;
}

void AuditStart(void *user, int run_index)
{
	AuditDlgState *st = (AuditDlgState *)user;
	const int i = RomOf(st, run_index);
	if (i < 0) return;
	st->status[i] = kRunning;
	if (st->row_of[i] >= 0)
		SetItemText(st->hList, st->row_of[i], kColVerdict, "RUN");
}

void AuditCell(void *user, int run_index, Combo c, const ComboResult &res)
{
	AuditDlgState *st = (AuditDlgState *)user;
	const int i = RomOf(st, run_index);
	if (i < 0) return;
	st->results[i].combos[(int)c] = res;
	const int row = st->row_of[i];
	if (row < 0) return;
	// The row is still kRunning; show the cell as it lands anyway.
	char buf[64];
	const int keep = st->status[i];
	st->status[i] = kDone;
	CellText(st, i, (int)c, buf, sizeof buf);
	st->status[i] = keep;
	SetItemText(st->hList, row, kColCombo + (int)c, buf);
	// New captures for the game on show: let the pane grow live.
	if (i == st->shown)
	{
		UpdateShotCaption(st);
		InvalidateRect(st->hShot, NULL, TRUE);
	}
}

void AuditResult(void *user, int run_index, const RomResult &res)
{
	AuditDlgState *st = (AuditDlgState *)user;
	const int i = RomOf(st, run_index);
	if (i < 0) return;
	st->results[i] = res;
	st->status[i]  = kDone;
	RefreshRow(st, i);
	if (st->shown < 0) SelectShot(st, i, -1);
	else if (i == st->shown)
	{
		UpdateShotCaption(st);
		InvalidateRect(st->hShot, NULL, TRUE);
	}
}

/*--------------------------------------------------------------------------
  Save baseline
--------------------------------------------------------------------------*/

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

void RescanBaselines(AuditDlgState *st)
{
	const std::string keep = ActiveBaseline(st) ? ActiveBaseline(st)->name : "";
	st->baselines = DiscoverBaselines(st->audit_dir.c_str());
	st->base_sel = -1;
	for (size_t i = 0; i < st->baselines.size(); ++i)
		if (st->baselines[i].name == keep) st->base_sel = (int)i;
	if (st->base_sel < 0 && !st->baselines.empty()) st->base_sel = 0;
	ApplyFilter(st);
}

void SaveBaselineUI(AuditDlgState *st)
{
	// Only shots captured this session can be saved.
	std::vector<Rom>       roms;
	std::vector<RomResult> results;
	for (int i : st->rows)
	{
		if (st->status[i] != kDone) continue;
		roms.push_back(st->roms[i]);
		results.push_back(st->results[i]);
	}
	if (roms.empty())
	{
		MessageBox(st->hDlg, TEXT("Run the audit first - a baseline is saved from captured frames."),
		           TEXT("Save baseline"), MB_OK | MB_ICONINFORMATION);
		return;
	}
	std::string root = st->audit_dir + "\\" + kBaselineDir;
	CreateDirectoryA(root.c_str(), NULL);
	Utf8ToWide wroot(root.c_str());
	TCHAR dir[MAX_PATH];
	if (!AskFolder(st->hDlg,
	               TEXT("Save the captured frames as a baseline in a folder under audit\\baseline:"),
	               wroot, dir, MAX_PATH))
		return;
	WideToUtf8 udir(dir);

	// Mid-run, the workers are reading the active baseline's PNGs: writing
	// over that same folder would race them. Any other folder is safe.
	if (st->running && ActiveBaseline(st))
	{
		auto norm = [](std::string p) {
			for (char &c : p)
			{
				if (c == '\\') c = '/';
				if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			}
			while (!p.empty() && p.back() == '/') p.pop_back();
			return p;
		};
		if (norm((const char *)udir) == norm(ActiveBaseline(st)->dir))
		{
			MessageBox(st->hDlg,
			           TEXT("That folder is the baseline this run is comparing against.\r\n")
			           TEXT("Pick another folder, or save after the run finishes."),
			           TEXT("Save baseline"), MB_OK | MB_ICONWARNING);
			return;
		}
	}

	std::string err;
	const int n = WriteBaseline((const char *)udir, roms, results,
	                            "SuperSnes9x audit", err);
	if (n < 0)
	{
		Utf8ToWide werr(err.c_str());
		MessageBox(st->hDlg, werr, TEXT("Save baseline"), MB_OK | MB_ICONERROR);
		return;
	}
	char buf[MAX_PATH + 96];
	snprintf(buf, sizeof buf, "Saved %d shot%s (%d ROM%s) to %s%s",
	         n, n == 1 ? "" : "s", (int)roms.size(),
	         roms.size() == 1 ? "" : "s", (const char *)udir,
	         st->running ? " — checkpoint; run continues" : "");
	SetCtrlText(st->hStat, buf);
	// A rescan swaps the baseline tables the running comparison points at,
	// so mid-run it waits for the finish line.
	if (st->running) st->rescan_after_run = true;
	else             RescanBaselines(st);
}

/*--------------------------------------------------------------------------
  Run state - stop a long run today, continue it tomorrow
--------------------------------------------------------------------------*/

std::string RunStateDir(const AuditDlgState *st)
{
	return st->audit_dir + "\\runstate";
}

// Snapshot every finished ROM - captures included - into audit\runstate.
// Restored automatically the next time the dialog opens.
void SaveRunUI(AuditDlgState *st)
{
	std::vector<Rom>       roms;
	std::vector<RomResult> results;
	for (size_t i = 0; i < st->roms.size(); ++i)
	{
		if (st->status[i] != kDone) continue;
		roms.push_back(st->roms[i]);
		results.push_back(st->results[i]);
	}
	if (roms.empty())
	{
		SetCtrlText(st->hStat, "Nothing finished yet - there is no run to save.");
		return;
	}
	std::string err;
	const int n = WriteBaseline(RunStateDir(st).c_str(), roms, results,
	                            "saved run", err);
	if (n < 0)
	{
		Utf8ToWide werr(err.c_str());
		MessageBox(st->hDlg, werr, TEXT("Save run"), MB_OK | MB_ICONERROR);
		return;
	}
	char buf[256];
	snprintf(buf, sizeof buf,
	         "Run saved: %d ROM%s. Reopen this dialog any time to continue "
	         "from here.%s",
	         (int)roms.size(), roms.size() == 1 ? "" : "s",
	         st->running ? " Run continues." : "");
	SetCtrlText(st->hStat, buf);
}

// Bring a saved run back: finished ROMs return as done with their captures,
// so the run continues with Show: Not run + Run Shown.
int LoadRunState(AuditDlgState *st)
{
	Baseline state;
	std::string err;
	if (!LoadBaseline(RunStateDir(st).c_str(), state, err) || state.Empty())
		return 0;

	int restored = 0;
	for (size_t i = 0; i < st->roms.size(); ++i)
	{
		bool any = false;
		for (int c = 0; c < kComboCount; ++c)
		{
			ComboResult &cr = st->results[i].combos[c];
			const BaselineEntry *be = state.Find(st->roms[i], (Combo)c);
			if (!be) continue;
			if (LoadCapturePng(state, *be, cr.shot))
			{
				cr.ran = true;
				any = true;
			}
		}
		if (any)
		{
			st->status[i] = kDone;
			++restored;
		}
	}
	return restored;
}

/*--------------------------------------------------------------------------
  The run
--------------------------------------------------------------------------*/

void EnableIdleUI(AuditDlgState *st, BOOL on)
{
	const int ids[] = { IDC_AUDIT_SEARCH, IDC_AUDIT_FOLDERS, IDC_AUDIT_SHOW,
	                    IDC_AUDIT_CLEAR, IDC_AUDIT_BASEPICK,
	                    IDC_AUDIT_SAVEBASE, IDC_AUDIT_RESCAN,
	                    IDC_AUDIT_SAVERUN };
	for (int id : ids) EnableWindow(GetDlgItem(st->hDlg, id), on);
}

// On the scan thread: no UI calls, only posted messages.
bool ScanProgressCB(void *user, int done, int total)
{
	AuditDlgState *st = (AuditDlgState *)user;
	PostMessage(st->hDlg, WM_AUDIT_SCANPROG, (WPARAM)done, (LPARAM)total);
	return !st->scan_abort.load(std::memory_order_relaxed);
}

void ScanThread(AuditDlgState *st)
{
	std::vector<Rom> *out = new std::vector<Rom>(
		ScanRoms(st->roms_dir.c_str(), st->audit_dir.c_str(), ScanProgressCB, st));
	if (st->scan_abort.load()) { delete out; return; }
	PostMessage(st->hDlg, WM_AUDIT_SCANDONE, 0, (LPARAM)out);
}

void StopScan(AuditDlgState *st)
{
	if (!st->scan_thread.joinable()) return;
	st->scan_abort.store(true);
	st->scan_thread.join();
	MSG msg;
	while (PeekMessage(&msg, st->hDlg, WM_AUDIT_SCANDONE, WM_AUDIT_SCANDONE,
	                   PM_REMOVE))
		delete (std::vector<Rom> *)msg.lParam;
}

// The finished list, adopted on the dialog thread.
void ScanDone(AuditDlgState *st, std::vector<Rom> *roms)
{
	st->roms.swap(*roms);
	delete roms;
	if (st->scan_thread.joinable()) st->scan_thread.join();
	st->scanning = false;

	st->results.assign(st->roms.size(), RomResult());
	st->status.assign(st->roms.size(), kPending);
	st->folders.clear();
	for (const Rom &r : st->roms)
		if (std::find(st->folders.begin(), st->folders.end(), r.folder)
		    == st->folders.end())
			st->folders.push_back(r.folder);
	st->folder_on.assign(st->folders.size(), 0);

	st->carttypes.clear();
	for (const Rom &r : st->roms)
		if (std::find(st->carttypes.begin(), st->carttypes.end(), r.cart_type)
		    == st->carttypes.end())
			st->carttypes.push_back(r.cart_type);
	std::sort(st->carttypes.begin(), st->carttypes.end());
	// "Official" leads; the exotic families follow alphabetically.
	auto of = std::find(st->carttypes.begin(), st->carttypes.end(), "Official");
	if (of != st->carttypes.end()) std::rotate(st->carttypes.begin(), of, of + 1);
	st->carttype_on.assign(st->carttypes.size(), 0);

	st->baselines = DiscoverBaselines(st->audit_dir.c_str());
	if (!st->baselines.empty()) st->base_sel = 0;

	const int restored = LoadRunState(st);

	SendMessage(st->hProg, PBM_SETPOS, 0, 0);
	EnableIdleUI(st, TRUE);
	ApplyFilter(st);
	if (restored > 0)
	{
		char buf[256];
		snprintf(buf, sizeof buf,
		         "Resumed saved run: %d of %d ROMs already done - "
		         "Show: Not run, then Run Shown continues it.",
		         restored, (int)st->roms.size());
		SetCtrlText(st->hStat, buf);
	}
}

void RunAuditUI(AuditDlgState *st, bool selected_only)
{
	if (st->running || st->scanning || st->rows.empty()) return;

	// Everything shown, or just the highlighted rows; results outside the
	// set are left alone, so a re-run of a few games keeps the rest.
	std::vector<int> picked;
	if (selected_only)
	{
		int row = -1;
		while ((row = ListView_GetNextItem(st->hList, row, LVNI_SELECTED)) >= 0)
			if (row < (int)st->rows.size()) picked.push_back(st->rows[row]);
		if (picked.empty())
		{
			SetCtrlText(st->hStat, "Select one or more rows first.");
			return;
		}
	}
	else
		picked = st->rows;

	std::vector<Rom> subset;
	st->run_map = picked;
	subset.reserve(st->run_map.size());
	for (int i : st->run_map)
	{
		subset.push_back(st->roms[i]);
		st->status[i]  = kPending;
		st->results[i] = RomResult();
		RefreshRow(st, i);
	}

	st->running   = true;
	st->cancelled = false;
	st->paused.store(false);
	SetWindowText(st->hPause, TEXT("&Pause"));
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUN), FALSE);
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUNSEL), FALSE);
	// The Threads box stays live: workers follow it mid-run, paused or not.
	EnableWindow(st->hPause, TRUE);
	EnableIdleUI(st, FALSE);
	// Save Baseline and Save Run stay usable as mid-run checkpoints.
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_SAVEBASE), TRUE);
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_SAVERUN), TRUE);
	SendMessage(st->hProg, PBM_SETRANGE32, 0, (LPARAM)subset.size());
	SendMessage(st->hProg, PBM_SETPOS, 0, 0);

	const int sel = (int)SendMessage(st->hThreads, CB_GETCURSEL, 0, 0);
	st->threads = (sel == CB_ERR) ? 1 : sel + 1;
	st->started = GetTickCount();
	st->pause_accum = 0;
	st->pause_since = 0;

	RunOptions opts;
	opts.audit_dir = st->audit_dir.c_str();
	opts.baseline  = ActiveBaseline(st);
	opts.progress  = AuditProgress;
	opts.on_start  = AuditStart;
	opts.on_cell   = AuditCell;
	opts.on_result = AuditResult;
	opts.user      = st;
	st->threads_live.store(st->threads);
	opts.threads      = st->threads;
	opts.threads_live = &st->threads_live;
	opts.pause     = &st->paused;
	// The audit cores share Settings with the live session, so the modal
	// launcher's old dialog-lifetime overrides now scope to the run:
	// BIOS-less silent boots while workers are up, sound device released.
	CloseSoundDevice();
	const bool8 saved_bios_active = Settings.SGB_BIOSModeActive;
	const bool8 saved_mute        = Settings.Mute;
	Settings.SGB_BIOSModeActive = FALSE;
	Settings.Mute = TRUE;

	Summary sum = RunAudit(subset, opts, nullptr);

	Settings.SGB_BIOSModeActive = saved_bios_active;
	Settings.Mute = saved_mute;
	ReInitSound();

	for (int i : st->run_map)
		if (st->status[i] == kRunning)
		{
			st->status[i] = kPending;
			RefreshRow(st, i);
		}
	st->running = false;
	const double secs = (GetTickCount() - st->started) / 1000.0;

	char buf[256];
	if (sum.cancelled)
		snprintf(buf, sizeof buf, "Cancelled: %d/%d done so far (%d failed, %d errors)",
		         sum.passed, sum.total, sum.failed, sum.errors);
	else if (ActiveBaseline(st))
		snprintf(buf, sizeof buf,
		         "Done in %.1fs on %d thread%s: %d/%d passed (%d failed, %d errors)",
		         secs, st->threads, st->threads == 1 ? "" : "s",
		         sum.passed, sum.total, sum.failed, sum.errors);
	else
		snprintf(buf, sizeof buf,
		         "Captured %d ROM%s in %.1fs — pick Save Baseline to pin them",
		         sum.total, sum.total == 1 ? "" : "s", secs);
	SetCtrlText(st->hStat, buf);
	SendMessage(st->hProg, PBM_SETPOS, (WPARAM)subset.size(), 0);
	EnableWindow(st->hThreads, TRUE);
	EnableWindow(st->hPause, FALSE);
	SetWindowText(st->hPause, TEXT("&Pause"));
	EnableIdleUI(st, TRUE);
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUN), TRUE);
	EnableWindow(GetDlgItem(st->hDlg, IDC_AUDIT_RUNSEL), TRUE);
	UpdateShotCaption(st);

	// A checkpoint saved mid-run becomes a pickable baseline now.
	if (st->rescan_after_run && !st->close_when_done)
	{
		st->rescan_after_run = false;
		const std::string keep = buf;
		RescanBaselines(st);
		SetCtrlText(st->hStat, keep.c_str());
	}

	if (st->close_when_done)
	{
		DestroyWindow(st->hDlg);   // frees st - nothing may touch it after
		RestoreSNESDisplay();
	}
}

INT_PTR CALLBACK AuditDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		AuditDlgState *st = new AuditDlgState;
		SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)st);
		st->hDlg  = hDlg;
		st->hList = GetDlgItem(hDlg, IDC_AUDIT_LIST);
		st->hProg = GetDlgItem(hDlg, IDC_AUDIT_PROGRESS);
		st->hStat = GetDlgItem(hDlg, IDC_AUDIT_STATUS);
		st->hThreads = GetDlgItem(hDlg, IDC_AUDIT_THREADS);
		st->hShot    = GetDlgItem(hDlg, IDC_AUDIT_SHOT);
		st->hShotCap = GetDlgItem(hDlg, IDC_AUDIT_SHOTCAP);
		st->hPause   = GetDlgItem(hDlg, IDC_AUDIT_PAUSE);
		st->hSearch  = GetDlgItem(hDlg, IDC_AUDIT_SEARCH);
		EnableWindow(st->hPause, FALSE);

		const int cores = DefaultThreadCount();
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

		st->audit_dir = FindDirNearExe("audit");
		if (st->audit_dir.empty()) st->audit_dir = "audit";
		st->roms_dir = FindDirNearExe("Roms");
		if (st->roms_dir.empty()) st->roms_dir = "Roms";

		st->linkmeta = LoadLinkMeta(st->audit_dir.c_str());
		st->show_on.assign(kShowCount, 0);
		RebuildColumns(st);

		// The list arrives from a worker so the dialog is not held behind
		// thousands of zip headers; everything that needs it waits greyed.
		st->scanning = true;
		EnableIdleUI(st, FALSE);
		EnableWindow(GetDlgItem(hDlg, IDC_AUDIT_RUN), FALSE);
		EnableWindow(GetDlgItem(hDlg, IDC_AUDIT_RUNSEL), FALSE);
		SetCtrlText(st->hStat, ("Scanning " + st->roms_dir + "...").c_str());
		st->scan_thread = std::thread(ScanThread, st);
		return TRUE;
	}

	case WM_AUDIT_SCANPROG:
	{
		AuditDlgState *st = GetState(hDlg);
		if (!st || !st->scanning) return TRUE;
		SendMessage(st->hProg, PBM_SETRANGE32, 0, (LPARAM)lParam);
		SendMessage(st->hProg, PBM_SETPOS, wParam, 0);
		char buf[128];
		snprintf(buf, sizeof buf, "Scanning %s... %d/%d files",
		         st->roms_dir.c_str(), (int)wParam, (int)lParam);
		SetCtrlText(st->hStat, buf);
		return TRUE;
	}

	case WM_AUDIT_SCANDONE:
	{
		AuditDlgState *st = GetState(hDlg);
		std::vector<Rom> *roms = (std::vector<Rom> *)lParam;
		if (st && st->scanning) ScanDone(st, roms);
		else                    delete roms;
		return TRUE;
	}

	case WM_DRAWITEM:
	{
		DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
		AuditDlgState *st = GetState(hDlg);
		if (st && dis->CtlID == IDC_AUDIT_SHOT)
		{
			PaintShot(st, dis);
			return TRUE;
		}
		break;
	}

	case WM_NOTIFY:
	{
		NMHDR *nm = (NMHDR *)lParam;
		AuditDlgState *st = GetState(hDlg);
		if (!st) break;
		if (nm->idFrom == IDC_AUDIT_LIST && nm->code == NM_DBLCLK)
		{
			// Double-click plays the ROM in the emulator behind us -
			// through the full app load path, exactly like drag-drop.
			const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)lParam;
			if (!st->running && ia->iItem >= 0 &&
			    ia->iItem < (int)st->rows.size())
			{
				const Rom &r = st->roms[st->rows[ia->iItem]];
				Utf8ToWide wpath(r.path.c_str());
				WinLoadROMFromDialog(wpath);
			}
			return TRUE;
		}
		if (nm->idFrom == IDC_AUDIT_LIST && nm->code == LVN_ITEMCHANGED)
		{
			NMLISTVIEW *lv = (NMLISTVIEW *)lParam;
			// Selecting a row - mouse or keyboard - shows every capture of
			// that game in the preview; < > and clicks on the image step
			// through them.
			if ((lv->uNewState & LVIS_SELECTED) &&
			    lv->iItem >= 0 && lv->iItem < (int)st->rows.size())
				SelectShot(st, st->rows[lv->iItem], -1);
			break;
		}
		if (nm->idFrom == IDC_AUDIT_LIST && nm->code == NM_RCLICK)
		{
			// The listview has already moved the selection to the clicked
			// row (or kept a multi-selection that includes it).
			if (ListView_GetSelectedCount(st->hList) > 0)
			{
				HMENU menu = CreatePopupMenu();
				AppendMenu(menu, MF_STRING, 1, TEXT("Copy Info"));
				POINT pt;
				GetCursorPos(&pt);
				const int cmd = TrackPopupMenu(menu,
					TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
					pt.x, pt.y, 0, hDlg, NULL);
				DestroyMenu(menu);
				if (cmd == 1) CopySelectedRows(st);
			}
			break;
		}
		if (nm->idFrom == IDC_AUDIT_LIST && nm->code == NM_CUSTOMDRAW)
		{
			NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lParam;
			switch (cd->nmcd.dwDrawStage)
			{
			case CDDS_PREPAINT:
				SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
				return TRUE;
			case CDDS_ITEMPREPAINT:
			{
				const int row = (int)cd->nmcd.dwItemSpec;
				COLORREF clr  = CLR_DEFAULT;
				if (row < (int)st->rows.size())
				{
					const int i = st->rows[row];
					if (st->status[i] == kRunning)
						clr = RGB(176, 96, 0);
					else if (st->status[i] == kDone)
					{
						bool err = false;
						for (int c = 0; c < kComboCount; ++c)
							if (st->results[i].combos[c].Cell() == Match::Error)
								err = true;
						if (err) clr = RGB(192, 0, 0);
						else if (ActiveBaseline(st))
							clr = st->results[i].AllPassed(st->roms[i])
							      ? RGB(0, 128, 0) : RGB(192, 0, 0);
						else clr = RGB(0, 0, 192);
					}
				}
				if (clr != CLR_DEFAULT) cd->clrText = clr;
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
		case IDC_AUDIT_THREADS:
		{
			// Mid-run (paused included) the pool follows the pick: more
			// wakes parked workers now, fewer parks each as its ROM ends.
			AuditDlgState *st = GetState(hDlg);
			if (!st || HIWORD(wParam) != CBN_SELCHANGE) return TRUE;
			const int sel = (int)SendMessage(st->hThreads, CB_GETCURSEL, 0, 0);
			if (sel == CB_ERR) return TRUE;
			st->threads = sel + 1;
			if (st->running) st->threads_live.store(sel + 1);
			return TRUE;
		}
		case IDC_AUDIT_RUN:
			RunAuditUI(GetState(hDlg), false);
			return TRUE;
		case IDC_AUDIT_RUNSEL:
			RunAuditUI(GetState(hDlg), true);
			return TRUE;
		case IDC_AUDIT_SEARCH:
		{
			AuditDlgState *st = GetState(hDlg);
			if (!st || HIWORD(wParam) != EN_CHANGE || st->running) return TRUE;
			TCHAR text[128];
			GetWindowText(st->hSearch, text, 128);
			WideToUtf8 utf8(text);
			st->search = (const char *)utf8;
			ApplyFilter(st);
			return TRUE;
		}
		case IDC_AUDIT_FOLDERS:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st) CheckMenu(st, IDC_AUDIT_FOLDERS, st->folders, st->folder_on,
			                  "All folders");
			return TRUE;
		}
		case IDC_AUDIT_CARTTYPE:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st) CheckMenu(st, IDC_AUDIT_CARTTYPE, st->carttypes,
			                  st->carttype_on, "All cart types");
			return TRUE;
		}
		case IDC_AUDIT_SHOW:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st)
			{
				const std::vector<std::string> names(kShowNames,
				                                     kShowNames + kShowCount);
				CheckMenu(st, IDC_AUDIT_SHOW, names, st->show_on, "All results");
			}
			return TRUE;
		}
		case IDC_AUDIT_BASEPICK:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st && !st->running) PickBaseline(st);
			return TRUE;
		}
		case IDC_AUDIT_SAVERUN:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st && !st->scanning) SaveRunUI(st);
			return TRUE;
		}
		case IDC_AUDIT_SAVEBASE:
		{
			// Allowed mid-run too - a checkpoint of every ROM finished so
			// far; merge-on-save lets a later save complete the set.
			AuditDlgState *st = GetState(hDlg);
			if (st && !st->scanning) SaveBaselineUI(st);
			return TRUE;
		}
		case IDC_AUDIT_RESCAN:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st && !st->running) RescanBaselines(st);
			return TRUE;
		}
		case IDC_AUDIT_SYNCPOS:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st)
			{
				st->sync_pos =
					IsDlgButtonChecked(hDlg, IDC_AUDIT_SYNCPOS) == BST_CHECKED;
				// Anchor on whatever the pane is showing right now.
				if (st->sync_pos) RememberShotPos(st);
			}
			return TRUE;
		}
		case IDC_AUDIT_SHOTPREV:
		case IDC_AUDIT_SHOTNEXT:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st) StepShot(st, LOWORD(wParam) == IDC_AUDIT_SHOTNEXT ? 1 : -1);
			return TRUE;
		}
		case IDC_AUDIT_SHOT:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st && HIWORD(wParam) == STN_CLICKED) StepShot(st, 1);
			return TRUE;
		}
		case IDC_AUDIT_CLEAR:
		{
			AuditDlgState *st = GetState(hDlg);
			if (!st || st->running) return TRUE;
			st->search.clear();
			std::fill(st->folder_on.begin(), st->folder_on.end(), (char)0);
			std::fill(st->carttype_on.begin(), st->carttype_on.end(), (char)0);
			std::fill(st->show_on.begin(), st->show_on.end(), (char)0);
			SetWindowText(st->hSearch, TEXT(""));
			ApplyFilter(st);
			return TRUE;
		}
		case IDC_AUDIT_PAUSE:
		{
			AuditDlgState *st = GetState(hDlg);
			if (!st || !st->running) return TRUE;
			const bool now = !st->paused.load();
			st->paused.store(now);
			if (now) st->pause_since = GetTickCount();
			else     st->pause_accum += GetTickCount() - st->pause_since;
			SetWindowText(st->hPause, now ? TEXT("&Resume") : TEXT("&Pause"));
			return TRUE;
		}
		case IDCANCEL:
		{
			AuditDlgState *st = GetState(hDlg);
			if (st && st->running)
			{
				st->close_when_done = true;
				st->cancelled = true;
				st->paused.store(false);
				SetWindowText(st->hStat, TEXT("Stopping..."));
				return TRUE;
			}
			DestroyWindow(hDlg);
			RestoreSNESDisplay();
			return TRUE;
		}
		}
		break;

	case WM_CLOSE:
	{
		AuditDlgState *st = GetState(hDlg);
		if (st && st->running)
		{
			st->close_when_done = true;
			st->cancelled = true;
			st->paused.store(false);
			SetWindowText(st->hStat, TEXT("Stopping..."));
			return TRUE;
		}
		DestroyWindow(hDlg);
		RestoreSNESDisplay();
		return TRUE;
	}

	case WM_DESTROY:
	{
		AuditDlgState *st = GetState(hDlg);
		SetWindowLongPtr(hDlg, DWLP_USER, 0);
		if (st) StopScan(st);   // it posts to this window, so it goes first
		delete st;
		s_hAuditDlg = NULL;
		return TRUE;
	}
	}
	return FALSE;
}

} // anonymous

bool WinAuditTestsAvailable()
{
	return !FindDirNearExe("audit").empty();
}

void WinShowAuditTestsDialog()
{
	if (s_hAuditDlg)
	{
		SetForegroundWindow(s_hAuditDlg);
		return;
	}
	// Modeless - the loaded session keeps running behind the list; the
	// audit cores' Settings overrides are scoped to the run (RunAuditUI).
	RestoreGUIDisplay();
	s_hAuditDlg = CreateDialog(g_hInst, MAKEINTRESOURCE(IDD_AUDIT_TESTS),
	                           GUI.hWnd, AuditDlgProc);
	if (s_hAuditDlg) ShowWindow(s_hAuditDlg, SW_SHOW);
	else             RestoreSNESDisplay();
}

HWND WinAuditTestsDialogHwnd() { return s_hAuditDlg; }
