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

struct Result
{
	Status      status = Status::Error;
	int         frames = 0;       // frames emulated until the decision
	std::string detail;           // error text / serial excerpt on failure
};

// Called at test start (frames_done 0) and every few frames while a test
// runs. Return false to cancel the whole run.
using ProgressFn = bool (*)(void *user, int test_index, int test_count,
                            const Test &test, int frames_done, int frames_total);

// Called once per finished test.
using ResultFn = void (*)(void *user, int test_index, const Test &test,
                          const Result &result);

struct RunOptions
{
	const char *acid_dir = "acid";  // directory holding manifest.txt + ROMs
	const char *filter   = nullptr; // substring filter on test names
	bool        dump_failures = false; // write _failures/<name>.ppm frames
	ProgressFn  progress  = nullptr;
	ResultFn    on_result = nullptr;
	void       *user      = nullptr;
};

struct Summary
{
	int total = 0, passed = 0, failed = 0, info = 0, errors = 0, cancelled = 0;
};

// Parse acid/manifest.txt. Returns false with a message in `err`.
bool LoadManifest(const char *acid_dir, std::vector<Test> &out, std::string &err);

// Run every (filtered) manifest test through the SGB core singleton and
// write <acid_dir>/results.txt. The current GB session is destroyed; the
// caller is responsible for reloading the user's ROM afterwards.
Summary Run(const RunOptions &opts);

} // namespace AcidTests

#endif
