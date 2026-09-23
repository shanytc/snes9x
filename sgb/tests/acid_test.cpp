/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Headless GB Emulator Shootout runner ("Acid Tests").
//
// Runs every test in acid/manifest.txt through the SGB core with the
// shootout's screenshot-compare rule and prints a per-test PASS/FAIL table.
// The same runner backs the win32 Tests > Acid Tests menu entry, so a
// green run here is a green run in the GUI.
//
// Build:
//   cd sgb/tests && make acid_test
//
// Run (from sgb/tests, or pass the acid dir explicitly):
//   ./acid_test [acid_dir] [options]
//
//   --suite NAME      only this suite (repeatable): blargg, mooneye, acid,
//                     samesuite, mealybug, daid, ax6, ashiepaws, cpp
//   --model DMG|CGB|SGB|SGB2   only tests for this model (repeatable)
//   --filter SUBSTR   only tests whose name contains SUBSTR
//   --list            print the selected tests and exit
//   --suites          print the suite names with test counts and exit
//   --threads N       emulator cores to run on (default: one per hw thread)
//   --dump            write failing frames to <acid_dir>/_failures
//   --quiet           only print failures
//   --txt/--json/--html PATH   also write a report in that format
//   --save-baseline DIR   write the captured frames to DIR as a baseline
//   --baseline DIR        diff the run against a baseline (ours or another
//                         emulator's dump laid out like acid/)
//
//   --dmg-boot/--cgb-boot PATH   boot DMG / CGB tests through this boot ROM
//   --sgb1-bios/--sgb2-bios PATH run SGB / SGB2 tests on this SNES BIOS, each
//                     in an acid_sgb_child process (make acid_sgb_child)
//   --sgb1-boot/--sgb2-boot PATH the GB-side boot ROM paired with that BIOS
//   --sgb-child PATH  the child program (default: acid_sgb_child beside this one)
//
// Exit code: number of failed tests (capped at 200), 255 on setup error.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../snes9x.h"
#include "../../memmap.h"
#include "../../ppu.h"

#include "../acid.h"
#include "../acid_report.h"
#include "../acid_baseline.h"

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

// An --txt/--json/--html output the run should also produce.
struct Output
{
	AcidTests::Format fmt;
	std::string       path;
};

bool ReadWhole(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? static_cast<size_t>(n) : 0);
	const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
	fclose(f);
	return got == out.size();
}

// "BIOS-less", or which boot ROMs and SGB BIOSes the tests ran through.
std::string BootDescription(const std::string &dmg, const std::string &cgb,
                            const std::string &sgb1, const std::string &sgb2)
{
	std::string d;
	auto add = [&](const char *what, const std::string &path) {
		if (!path.empty()) d += (d.empty() ? "" : ", ") + std::string(what) + " " + path;
	};
	add("DMG", dmg);
	add("CGB", cgb);
	add("SGB", sgb1);
	add("SGB2", sgb2);
	return d.empty() ? "BIOS-less" : d;
}

} // anonymous

int main(int argc, char **argv)
{
	AcidTests::RunOptions opts;
	Ctx ctx;
	std::vector<Output> outputs;
	std::string save_baseline, compare_baseline;
	std::string dmg_boot_path, cgb_boot_path;
	bool list_tests = false, list_suites = false;
	opts.acid_dir = "../../acid";
	opts.progress = &OnProgress;
	opts.on_result = &OnResult;
	opts.user = &ctx;

	for (int i = 1; i < argc; ++i)
	{
		const char *arg = argv[i];
		const bool has_next = i + 1 < argc;
		if (!std::strcmp(arg, "--filter") && has_next)
			opts.filter.text = argv[++i];
		else if (!std::strcmp(arg, "--suite") && has_next)
			opts.filter.suites.push_back(argv[++i]);
		else if (!std::strcmp(arg, "--model") && has_next)
		{
			const std::string m = argv[++i];
			if      (m == "CGB" || m == "cgb") opts.filter.models |= AcidTests::ModelBit(AcidTests::Model::CGB);
			else if (m == "SGB" || m == "sgb") opts.filter.models |= AcidTests::ModelBit(AcidTests::Model::SGB);
			else if (m == "SGB2" || m == "sgb2") opts.filter.models |= AcidTests::ModelBit(AcidTests::Model::SGB2);
			else if (m == "DMG" || m == "dmg") opts.filter.models |= AcidTests::ModelBit(AcidTests::Model::DMG);
			else
			{
				fprintf(stderr, "unknown model '%s' (want DMG, CGB, SGB or SGB2)\n", m.c_str());
				return 255;
			}
		}
		else if (!std::strcmp(arg, "--txt") && has_next)
			outputs.push_back({ AcidTests::Format::Text, argv[++i] });
		else if (!std::strcmp(arg, "--json") && has_next)
			outputs.push_back({ AcidTests::Format::Json, argv[++i] });
		else if (!std::strcmp(arg, "--html") && has_next)
			outputs.push_back({ AcidTests::Format::Html, argv[++i] });
		else if (!std::strcmp(arg, "--save-baseline") && has_next)
			save_baseline = argv[++i];
		else if (!std::strcmp(arg, "--baseline") && has_next)
			compare_baseline = argv[++i];
		else if (!std::strcmp(arg, "--list"))
			list_tests = true;
		else if (!std::strcmp(arg, "--suites"))
			list_suites = true;
		else if (!std::strcmp(arg, "--dump"))
			opts.dump_failures = true;
		else if (!std::strcmp(arg, "--quiet"))
			ctx.quiet = true;
		else if (!std::strcmp(arg, "--threads") && has_next)
			opts.threads = std::atoi(argv[++i]);
		else if (!std::strcmp(arg, "--suppress-nrx"))
			opts.suppress_nrx = true;
		else if ((!std::strcmp(arg, "--dmg-boot") || !std::strcmp(arg, "--cgb-boot")) && has_next)
		{
			const bool cgb = arg[2] == 'c';
			const char *path = argv[++i];
			if (!ReadWhole(path, cgb ? opts.cgb_boot : opts.dmg_boot))
			{
				fprintf(stderr, "cannot read boot ROM %s\n", path);
				return 255;
			}
			(cgb ? cgb_boot_path : dmg_boot_path) = path;
		}
		else if (!std::strcmp(arg, "--sgb1-bios") && has_next)
			opts.sgb1_bios = argv[++i];
		else if (!std::strcmp(arg, "--sgb2-bios") && has_next)
			opts.sgb2_bios = argv[++i];
		else if (!std::strcmp(arg, "--sgb1-boot") && has_next)
			opts.sgb1_boot = argv[++i];
		else if (!std::strcmp(arg, "--sgb2-boot") && has_next)
			opts.sgb2_boot = argv[++i];
		else if (!std::strcmp(arg, "--sgb-child") && has_next)
			opts.sgb_child = argv[++i];
		else if (arg[0] == '-')
		{
			fprintf(stderr, "unknown option '%s'\n", arg);
			return 255;
		}
		else
			opts.acid_dir = arg;
	}

	if (opts.sgb_child.empty() && (!opts.sgb1_bios.empty() || !opts.sgb2_bios.empty()))
	{
		const std::string self = argv[0];
		const size_t slash = self.find_last_of("/\\");
		opts.sgb_child = (slash == std::string::npos ? std::string("./")
		                                              : self.substr(0, slash + 1)) +
		                 "acid_sgb_child";
	}

	std::vector<AcidTests::Test> all;
	std::string err;
	if (!AcidTests::LoadManifest(opts.acid_dir, all, err))
	{
		fprintf(stderr, "acid: %s\n", err.c_str());
		return 255;
	}

	std::vector<AcidTests::Test> tests;
	for (const AcidTests::Test &t : all)
		if (opts.filter.Matches(t)) tests.push_back(t);

	if (list_suites)
	{
		for (const std::string &s : AcidTests::SuitesOf(all))
		{
			int n = 0, sel = 0;
			for (const AcidTests::Test &t : all)
			{
				if (t.suite != s) continue;
				++n;
				if (opts.filter.Matches(t)) ++sel;
			}
			printf("%-28s %3d tests%s\n", s.c_str(), n,
			       sel == n ? "" : (sel ? "  (partly selected)" : "  (not selected)"));
		}
		return 0;
	}

	if (list_tests)
	{
		for (size_t i = 0; i < tests.size(); ++i)
			printf("[%3d] %-5s %-58s %gs\n", static_cast<int>(i) + 1,
			       AcidTests::ModelName(tests[i].model), tests[i].name.c_str(),
			       tests[i].runtime);
		printf("\n%d of %d tests selected (%s)\n", static_cast<int>(tests.size()),
		       static_cast<int>(all.size()), opts.filter.Describe().c_str());
		return 0;
	}

	if (tests.empty())
	{
		fprintf(stderr, "no tests match %s\n", opts.filter.Describe().c_str());
		return 255;
	}

	// Freeze the audio drift-rate controller at the authentic clock.
	Settings.Mute = TRUE;

	const auto t0 = std::chrono::steady_clock::now();
	std::vector<AcidTests::Result> results;
	const AcidTests::Summary s = AcidTests::RunTests(tests, opts, &results);
	const double secs = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();

	printf("\n%d/%d passed (%d failed, %d info, %d errors%s)\n",
	       s.passed, s.passed + s.failed, s.failed, s.info, s.errors,
	       s.cancelled ? ", cancelled" : "");

	AcidTests::ReportInfo info;
	info.env     = AcidTests::EnvOverrides();
	info.filter  = opts.filter.Describe();
	info.boot    = BootDescription(dmg_boot_path, cgb_boot_path,
	                               opts.sgb1_bios, opts.sgb2_bios);
	info.suppress_nrx = opts.suppress_nrx;
	info.source  = opts.acid_dir;
	info.seconds = secs;
	info.threads = opts.threads > 0 ? opts.threads : AcidTests::DefaultThreadCount();
	std::vector<AcidTests::ReportRow> rows;
	rows.reserve(tests.size());
	for (size_t i = 0; i < tests.size(); ++i)
	{
		AcidTests::ReportRow row;
		row.test   = &tests[i];
		row.result = &results[i];
		rows.push_back(std::move(row));
	}

	// Every folder under acid/baseline/ is compared against; --baseline adds
	// one from anywhere else.
	std::vector<AcidTests::Baseline> baselines =
		AcidTests::DiscoverBaselines(opts.acid_dir);
	if (!compare_baseline.empty())
	{
		AcidTests::Baseline extra;
		if (!AcidTests::LoadBaseline(compare_baseline.c_str(), extra, err))
		{
			fprintf(stderr, "baseline: %s\n", err.c_str());
			return 255;
		}
		extra.name = compare_baseline;
		baselines.push_back(std::move(extra));
	}
	if (!baselines.empty())
	{
		info.baselines = &baselines;
		std::vector<int> same(baselines.size()), differ(baselines.size()),
		                 missing(baselines.size()), na(baselines.size());
		for (AcidTests::ReportRow &r : rows)
		{
			r.match.resize(baselines.size());
			r.diff_px.resize(baselines.size());
			for (size_t b = 0; b < baselines.size(); ++b)
			{
				r.match[b] = AcidTests::CompareToBaseline(
					baselines[b], *r.test, r.result->shot, r.diff_px[b]);
				if      (r.match[b] == AcidTests::Match::Same)    ++same[b];
				else if (r.match[b] == AcidTests::Match::Differs) ++differ[b];
				else if (r.match[b] == AcidTests::Match::NoImage) ++missing[b];
				else if (r.match[b] == AcidTests::Match::NoRef)   ++na[b];
				if (r.match[b] == AcidTests::Match::Differs)
					printf("DIFF  %-48s %-14s %d px\n", r.test->name.c_str(),
					       baselines[b].name.c_str(), r.diff_px[b]);
			}
		}
		for (size_t b = 0; b < baselines.size(); ++b)
			printf("baseline %-16s %d same, %d differ, %d missing, %d n/a\n",
			       baselines[b].name.c_str(), same[b], differ[b], missing[b],
			       na[b]);
	}

	if (!save_baseline.empty())
	{
		const int n = AcidTests::WriteBaseline(save_baseline.c_str(), rows, info, err);
		if (n < 0) fprintf(stderr, "%s\n", err.c_str());
		else       printf("wrote %d frames to %s\n", n, save_baseline.c_str());
	}

	for (const Output &o : outputs)
	{
		if (AcidTests::WriteReport(o.path.c_str(), o.fmt, rows, info, err))
			printf("wrote %s\n", o.path.c_str());
		else
			fprintf(stderr, "%s\n", err.c_str());
	}

	return s.failed + s.errors > 200 ? 200 : s.failed + s.errors;
}
