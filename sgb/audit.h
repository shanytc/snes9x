/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_AUDIT_H_
#define _SGB_AUDIT_H_

// Regression audit ("Audit Tests"). Runs every ROM under Roms/ on every
// boot column the cart supports - each column is ONE capture with one
// meaning: the boot logos (GB/GBC BIOS), the title (GB/GBC), the SGB rings
// with the default bezel (SGB1/SGB2), and the cart's custom border
// (SGB1/SGB2 Border) - compared against a saved baseline under
// audit/baseline/. A mismatch reports how many pixels moved and, when the
// same picture exists nearby, how many frames it slipped. Shared by the
// win32 Tests menu entry and the headless harness probe.

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace AuditTests {

/*--------------------------------------------------------------------------
  Combos and ROM classification
--------------------------------------------------------------------------*/

// One column = ONE capture with one meaning, ordered like the real
// power-on flow of each model. Baselines key combos by id string, not
// position, so the order can change without invalidating saved captures.
enum class Combo : uint8_t
{
	GB_Bios,   // the DMG boot ROM's settled Nintendo logo
	GB,        // the game's title, monochrome
	GBC_Bios,  // the CGB boot ROM's settled color logo
	GBC,       // the game's title in color
	SGB1,      // plain SGB: forced-DMG boot in the default bezel - a
	SGB2,      // CGB-only cart's own lockout screen is the capture
	SGB1_CB,   // the cart's custom border, once uploaded and settled
	SGB2_CB,   // likewise on SGB2 (both: SGB-enhanced carts only)
	SGBC,      // Super Game Boy Color: CGB colors (compat ones for a mono
	           // cart) inside the SGB2 frame, the cart's border if it sends one
	Count
};
constexpr int kComboCount = static_cast<int>(Combo::Count);

const char *ComboId(Combo c);     // "gb", "gb+bios", ... (baseline index key)
const char *ComboName(Combo c);   // "GB", "GB BIOS", ... (column heading)
bool ComboIsRing(Combo c);        // 256x224 SGB composite (vs 160x144 panel)
bool ComboNeedsBoot(Combo c);     // needs the DMG/CGB boot ROM (the BIOS
                                  // manager's file, else audit/*_boot.bin)
// Baseline shot id per column: panel shots store as "title", ring shots as
// "border" - captures from the older multi-shot layout keep matching.
const char *ComboShotId(Combo c);

struct Rom
{
	std::string path;      // full path to the .gb/.gbc/.zip
	std::string name;      // display name: file stem
	std::string key;       // stable baseline key: "<folder>/<stem>" sanitized
	std::string folder;    // immediate folder under Roms/
	uint8_t     cgb_flag = 0;   // header $0143
	uint8_t     sgb_flag = 0;   // header $0146
	bool        gbc_file = false;   // the image inside is .gbc
	uint32_t    size     = 0;
	// Mapper-family bucket ("Official", "Sachen", "MBC1M", ...) from the
	// core's cart detection over the full image; cached across scans.
	std::string cart_type;
};

bool RomSgbEnhanced(const Rom &r);   // $0146 == $03
bool RomCgbCapable(const Rom &r);    // $0143 bit 7
bool RomCgbOnly(const Rom &r);       // $0143 == $C0
// "GB" / "GB+SGB" / "GB+GBC" / "Tri-boot" / "GBC" / "GBC+SGB" - which
// devices the cart itself claims to support.
std::string RomBootClass(const Rom &r);
bool ComboApplies(Combo c, const Rom &r);

/*--------------------------------------------------------------------------
  Captures
--------------------------------------------------------------------------*/

constexpr int kPanelW = 160, kPanelH = 144;   // panel shots
constexpr int kSgbW   = 256, kSgbH   = 224;   // Border (full SGB composite)
// Where the settled DMG boot logo sits on the panel, and its size.
constexpr int kLogoX = 32, kLogoY = 64, kLogoW = 96, kLogoH = 16;

struct Capture
{
	int  frame = -1;               // frame the shot was taken on
	int  w = 0, h = 0;
	// PNG-encoded w*h RGB - held compressed so a whole-library run fits in
	// memory, and saved to a baseline as-is. Empty when not captured.
	std::vector<uint8_t> png;

	bool Empty() const { return png.empty(); }
	// Decode back to w*h*3 RGB for comparing or drawing.
	bool Decode(std::vector<uint8_t> &rgb) const;
};

// One shot compared against the baseline.
enum class Match : uint8_t
{
	None,      // not compared yet
	Same,
	Differs,   // pixels moved and no nearby frame matches
	Slip,      // the baseline picture exists, `slip` frames away
	NoBase,    // baseline has no entry for this shot
	Skip,      // combo not applicable / no boot ROM available
	Error
};
const char *MatchName(Match m);

struct ShotVerdict
{
	Match m       = Match::None;
	int   diff_px = 0;
	int   slip    = 0;    // signed frames late (+) or early (-)
};

struct ComboResult
{
	bool        ran = false;
	std::string error;                    // load failure etc.
	Capture     shot;                     // THE column's one capture
	ShotVerdict verdict;
	Match Cell() const { return verdict.m; }
	int   CellDiff() const { return verdict.diff_px; }
	int   CellSlip() const { return verdict.slip; }
};

struct RomResult
{
	ComboResult combos[kComboCount];
	// PASS when every applicable combo compared Same.
	bool AllPassed(const Rom &r) const;
};

/*--------------------------------------------------------------------------
  Baseline - audit/baseline/<name>/
--------------------------------------------------------------------------*/

constexpr const char *kBaselineDir     = "baseline";
constexpr const char *kDefaultBaseline = "default";

struct BaselineEntry
{
	int         frame = -1;
	std::string image;    // path relative to the baseline folder
};

struct Baseline
{
	std::string dir, name, title, created;
	// "<rom key>|<combo id>|<shot id>" -> entry
	std::map<std::string, BaselineEntry> entries;

	bool Empty() const { return entries.empty(); }
	const BaselineEntry *Find(const Rom &r, Combo c) const;
};

bool LoadBaseline(const char *dir, Baseline &out, std::string &err);
std::vector<Baseline> DiscoverBaselines(const char *audit_dir);
bool LoadBaselineShot(const Baseline &b, const BaselineEntry &e,
                      std::vector<uint8_t> &rgb, int &w, int &h);
// The same shot as a still-compressed Capture (frame from the index, size
// from the PNG header) - how a saved run's captures are restored.
bool LoadCapturePng(const Baseline &b, const BaselineEntry &e, Capture &out);

// Write the captured shots of `results` (parallel to `roms`) as PNGs plus
// an index under `dir`. Shots shard into a-z subfolders by ROM name so a
// whole-library save never piles ~65k files into one folder; the index
// carries folder-relative paths either way, and a re-save migrates any
// pre-shard flat files. Returns shots written, -1 with `err` on failure.
int WriteBaseline(const char *dir, const std::vector<Rom> &roms,
                  const std::vector<RomResult> &results,
                  const char *title, std::string &err);

/*--------------------------------------------------------------------------
  Scanning
--------------------------------------------------------------------------*/

// Every .gb/.gbc/.zip one level under roms_dir's subfolders (and in
// roms_dir itself), header-classified. Sorted by folder then name. SNES
// images are never opened; a zip is only decompressed when it holds a
// .gb/.gbc entry. `progress` (done, total files) may return false to
// abort - the scan then returns what it has.
using ScanProgressFn = bool (*)(void *user, int done, int total);
// Cart types come from the core's mapper detection (a full-image read),
// memoized in audit_dir/cart_types.txt so later scans stay header-quick.
std::vector<Rom> ScanRoms(const char *roms_dir, const char *audit_dir,
                          ScanProgressFn progress = nullptr,
                          void *user = nullptr);

// Optional curated link-cable metadata: audit/linkmeta.txt lines of
// "<substring of rom key>|<label>", e.g. "Faceball|2-15P ring". First hit
// wins; empty string when nothing matches.
std::map<std::string, std::string> LoadLinkMeta(const char *audit_dir);
std::string LinkLabelFor(const std::map<std::string, std::string> &meta,
                         const Rom &r);

/*--------------------------------------------------------------------------
  DMG boot-logo detector - SGB modes must never show the Nintendo scroll
--------------------------------------------------------------------------*/

// The settled boot-logo screen rebuilt from the cart's header bytes: 96x16
// mask drawn at (kLogoX, kLogoY). Empty when the header holds no logo.
std::vector<uint8_t> DecodeHeaderLogo(const std::vector<uint8_t> &rom);
// True when a kPanelW*kPanelH grayscale is exactly that screen. RunAudit
// fails an SGB combo outright the moment any frame matches.
bool GrayShowsBootLogo(const std::vector<uint8_t> &gray,
                       const std::vector<uint8_t> &logo);

/*--------------------------------------------------------------------------
  The run
--------------------------------------------------------------------------*/

using StartFn   = void (*)(void *user, int rom_index);
// One combo cell has its verdict (fires per combo as the worker finishes it).
using CellFn    = void (*)(void *user, int rom_index, Combo c,
                           const ComboResult &res);
// A whole ROM is done.
using ResultFn  = void (*)(void *user, int rom_index, const RomResult &res);
// Return false to cancel. `done`/`total` count ROMs.
using ProgressFn = bool (*)(void *user, int done, int total);

struct RunOptions
{
	const char *audit_dir = "audit";   // boot ROMs + baselines live here
	// Baseline that pins the capture frames; null runs baseline-less
	// (auto-picked frames, for creating a fresh baseline).
	const Baseline *baseline = nullptr;
	ProgressFn  progress  = nullptr;
	StartFn     on_start  = nullptr;
	CellFn      on_cell   = nullptr;
	ResultFn    on_result = nullptr;
	void       *user      = nullptr;
	const std::atomic<bool> *pause = nullptr;
	int         threads   = 0;         // 0 = one per hardware thread
	// Live worker-count control. When set, the pool is created at the
	// hardware size and only the first *threads_live workers pull ROMs -
	// raising it wakes parked workers at once, lowering it parks each
	// worker as it finishes its current ROM. `threads` is the start value.
	const std::atomic<int> *threads_live = nullptr;
	// Frames to search around a pinned frame for a slipped picture.
	int         slip_window = 150;
};

struct Summary
{
	int total = 0, passed = 0, failed = 0, errors = 0, cancelled = 0;
};

int DefaultThreadCount();

// Run `roms` (already filtered, caller's order); callbacks arrive on the
// calling thread. `results` is resized to match and filled as ROMs finish.
Summary RunAudit(const std::vector<Rom> &roms, const RunOptions &opts,
                 std::vector<RomResult> *results);

} // namespace AuditTests

#endif
