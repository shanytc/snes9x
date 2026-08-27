/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_ACID_H_
#define _SGB_ACID_H_

// GB Emulator Shootout test runner ("Acid Tests"). Drives the SGB core
// through every ROM listed in acid/manifest.txt and screenshot-compares the
// GB framebuffer against the suite's reference PNGs, using the shootout's
// own rule: both sides converted to grayscale, pass when no pixel differs
// by more than 50/255. Shared by the win32 Emulation menu entry and the
// headless sgb/tests/acid_test CLI.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace AcidTests {

enum class Model : uint8_t { DMG, CGB, SGB };

enum class Status : uint8_t
{
	Pass,   // matched a pass image
	Fail,   // matched a fail image, or timed out with pass images defined
	Info,   // informational test (no pass images) ran to completion
	Error   // ROM missing / reference unreadable / load failure
};

struct Test
{
	std::string name;
	Model       model   = Model::DMG;
	double      runtime = 1.0;    // seconds at real hardware speed
	std::string rom;              // path relative to the acid directory
	std::vector<std::string> pass_images;
	std::vector<std::string> fail_images;
};

// A captured GB frame: 160x144 RGB triplets, row 0 at the top. Empty when
// the test never got far enough to render one.
constexpr int kShotWidth  = 160;
constexpr int kShotHeight = 144;

struct Result
{
	Status      status = Status::Error;
	int         frames = 0;       // frames emulated until the decision
	std::string detail;           // error text / serial excerpt on failure
	// The frame the verdict was taken on - the matching one for a pass,
	// the last one rendered otherwise. Same picture the shootout's report
	// shows beside each test.
	std::vector<uint8_t> shot;
};

// Called as tests finish (and periodically while they run). test_index and
// frames_done both carry the number completed so far - with several cores
// running there is no single "current" test. Return false to cancel.
using ProgressFn = bool (*)(void *user, int test_index, int test_count,
                            const Test &test, int frames_done, int frames_total);

// Called once per finished test.
using ResultFn = void (*)(void *user, int test_index, const Test &test,
                          const Result &result);

// Called when a worker picks a test up, before its first frame. With N
// threads, N tests are in flight between their StartFn and their ResultFn.
using StartFn = void (*)(void *user, int test_index, const Test &test);

// Called every so often for a test that is still running, so a caller can
// show it advancing rather than just sitting there.
using RunningFn = void (*)(void *user, int test_index, int frames_done,
                           int frames_total);

struct RunOptions
{
	const char *acid_dir = "acid";  // directory holding manifest.txt + ROMs
	const char *filter   = nullptr; // substring filter on test names
	bool        dump_failures = false; // write _failures/<name>.ppm frames
	ProgressFn  progress  = nullptr;
	ResultFn    on_result = nullptr;
	StartFn     on_start  = nullptr;
	RunningFn   on_running = nullptr;
	// Set by the caller to hold every worker between frames. Cancelling
	// still takes effect while it is set.
	const std::atomic<bool> *pause = nullptr;
	void       *user      = nullptr;
	// Cores to run tests on at once. 0 picks one per hardware thread.
	// Each gets its own emulator instance; the ROMs are independent.
	int         threads   = 0;
};

struct Summary
{
	int total = 0, passed = 0, failed = 0, info = 0, errors = 0, cancelled = 0;
};

// Parse acid/manifest.txt. Returns false with a message in `err`.
bool LoadManifest(const char *acid_dir, std::vector<Test> &out, std::string &err);

// One emulator core per hardware thread, the runner's default.
int DefaultThreadCount();

// Any ACID_* timing overrides present in the environment, as "NAME=VALUE"
// separated by spaces. These retune the GB core, so a run that inherited
// one is not comparable with a run that did not - the runner reports them
// rather than letting them change results silently. Empty when clean.
std::string EnvOverrides();

// Run every (filtered) manifest test and write <acid_dir>/results.txt.
// Tests are spread over RunOptions::threads private emulator instances, so
// the caller's own GB session is left alone. Every callback - start,
// progress and result - is invoked on the calling thread, whichever worker
// the work happened on.
Summary Run(const RunOptions &opts);

} // namespace AcidTests

#endif
