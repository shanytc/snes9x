/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_ACID_BASELINE_H_
#define _SGB_ACID_BASELINE_H_

// A baseline is a folder of captured GB frames to diff a run against: our
// own output from an earlier build, or another emulator's dump. It lets a
// run be judged against something other than the reference images the
// shootout ships, which are one particular emulator's idea of correct.

#include <map>
#include <string>
#include <vector>

#include "acid.h"
#include "acid_report.h"

namespace AcidTests {

struct Baseline
{
	std::string dir;
	std::string name;     // folder under baseline/, and the column heading
	std::string title;    // emulator named in baseline.txt, when it has one
	std::string created;
	// Test name -> image path relative to `dir`, from the index file.
	std::map<std::string, std::string> images;

	// Where to write a test's frame: the index if it names one, else the
	// name with a trailing .gb/.gbc dropped and .png added, which is the
	// layout the shootout's own reference images use.
	std::string ImageFor(const Test &t) const;

	// Where to look for it, best first. Adds the test's own reference
	// images, which is where a dump laid out like acid/ keeps the ones
	// whose names do not follow from the test's.
	std::vector<std::string> CandidatesFor(const Test &t) const;

	bool Empty() const { return images.empty() && dir.empty(); }
};

// Read <dir>/baseline.txt if it is there. A folder without one still loads:
// images are then found by their derived path, so a foreign dump laid out
// like the suite works with no index at all.
bool LoadBaseline(const char *dir, Baseline &out, std::string &err);

// Every folder under <acid_dir>/baseline/, loaded, with default first and
// the rest in name order. Dropping a folder in is all it takes to have it
// compared against.
std::vector<Baseline> DiscoverBaselines(const char *acid_dir);

// The baseline's frame for a test, as 160x144 RGB. False when it has none.
// With `prefer` set, and a test that lists several acceptable screens, the
// closest of them wins and `diff_px` gets how far off it is.
bool LoadBaselineFrame(const Baseline &b, const Test &t,
                       std::vector<uint8_t> &rgb,
                       const std::vector<uint8_t> *prefer = nullptr,
                       int *diff_px = nullptr);

// Compare a captured frame against the baseline. `diff_px` gets the number
// of pixels outside tolerance when they differ.
Match CompareToBaseline(const Baseline &b, const Test &t,
                        const std::vector<uint8_t> &shot, int &diff_px);

// What a save into `dir` would overwrite. A foreign dump has no index but
// its frames still count, so this asks the only question that matters:
// which files already exist.
struct BaselineClash
{
	int  images = 0;      // frames that would be replaced
	bool index  = false;  // an existing baseline.txt would be replaced
	std::string title;    // what that index says about itself
	std::string created;

	bool Any() const { return images > 0 || index; }
};

BaselineClash CheckBaseline(const char *dir, const std::vector<ReportRow> &rows);

// Write every frame in `rows` under `dir` as a PNG, plus a baseline.txt
// index. Returns how many were written, -1 with a message in `err` on
// failure.
int WriteBaseline(const char *dir, const std::vector<ReportRow> &rows,
                  const ReportInfo &info, std::string &err);

} // namespace AcidTests

#endif
