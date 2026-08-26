/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Headless GB Emulator Shootout runner ("Acid Tests").
//
// Runs every test in acid/manifest.txt through the SGB core with the
// shootout's screenshot-compare rule and prints a per-test PASS/FAIL table.
// The same runner backs the win32 Emulation > Acid Tests menu entry, so a
// green run here is a green run in the GUI.
//
// Build:
//   cd sgb/tests && make acid_test
//
// Run (from sgb/tests, or pass the acid dir explicitly):
//   ./acid_test [acid_dir] [--filter substring] [--dump] [--quiet]
//
// Exit code: number of failed tests (capped at 200), 255 on setup error.

#include <cstdio>
#include <cstring>
#include <string>

#include "../../snes9x.h"
#include "../../memmap.h"
#include "../../ppu.h"

#include "../acid.h"

// snes9x globals the sgb/ subsystem links against.
struct SSettings   Settings;
struct SPPU        PPU;
struct InternalPPU IPPU;
struct CMemory     Memory;

void S9xMessage(int, int, const char *) {}

namespace {

struct Ctx
{
	bool quiet = false;
	int  fails = 0;
};

bool OnProgress(void *, int, int, const AcidTests::Test &, int, int)
{
	return true;   // no cancellation in CLI runs
}

void OnResult(void *user, int index, const AcidTests::Test &t,
              const AcidTests::Result &r)
{
	Ctx *ctx = static_cast<Ctx *>(user);
	static const char *kStatus[] = { "PASS", "FAIL", "INFO", "ERROR" };
	const bool bad = r.status == AcidTests::Status::Fail ||
	                 r.status == AcidTests::Status::Error;
	if (bad) ++ctx->fails;
	if (!ctx->quiet || bad)
	{
		printf("[%3d] %-5s %-58s %s\n", index + 1,
		       kStatus[static_cast<int>(r.status)], t.name.c_str(),
		       r.detail.c_str());
		fflush(stdout);
	}
}

} // anonymous

int main(int argc, char **argv)
{
	AcidTests::RunOptions opts;
	Ctx ctx;
	opts.acid_dir = "../../acid";
	opts.progress = &OnProgress;
	opts.on_result = &OnResult;
	opts.user = &ctx;

	for (int i = 1; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--filter") && i + 1 < argc)
			opts.filter = argv[++i];
		else if (!std::strcmp(argv[i], "--dump"))
			opts.dump_failures = true;
		else if (!std::strcmp(argv[i], "--quiet"))
			ctx.quiet = true;
		else
			opts.acid_dir = argv[i];
	}

	// Freeze the audio drift-rate controller at the authentic clock.
	Settings.Mute = TRUE;

	const AcidTests::Summary s = AcidTests::Run(opts);
	if (s.total == 0)
	{
		fprintf(stderr, "no tests ran — bad acid dir? (%s)\n", opts.acid_dir);
		return 255;
	}

	printf("\n%d/%d passed (%d failed, %d info, %d errors%s)\n",
	       s.passed, s.passed + s.failed, s.failed, s.info, s.errors,
	       s.cancelled ? ", cancelled" : "");
	return s.failed + s.errors > 200 ? 200 : s.failed + s.errors;
}
