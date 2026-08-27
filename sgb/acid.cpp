/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "acid.h"
#include "acid_report.h"
#include "sgb.h"
#include "gb_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "stb_image.h"

// The process environment, for reporting inherited ACID_* timing overrides.
#ifdef _WIN32
extern "C" char **_environ;
#define ACID_ENVIRON _environ
#else
extern "C" char **environ;
#define ACID_ENVIRON environ
#endif


namespace AcidTests {

namespace {

constexpr int GB_W = 160;
constexpr int GB_H = 144;
// The shootout's compareImage(): grayscale, per-pixel tolerance of 50.
constexpr int TOLERANCE = 50;

std::string JoinPath(const std::string &dir, const std::string &rel)
{
	if (dir.empty()) return rel;
	std::string p = dir;
	if (p.back() != '/' && p.back() != '\\') p += '/';
	return p + rel;
}

// PIL Image.convert("L") luma — ITU-R 601-2, integer-truncated.
inline uint8_t Luma(int r, int g, int b)
{
	return static_cast<uint8_t>((r * 299 + g * 587 + b * 114) / 1000);
}

// Load a reference PNG as a 160x144 grayscale buffer.
bool LoadReferenceGray(const std::string &path, std::vector<uint8_t> &out,
                       std::string &err)
{
	int w = 0, h = 0, comp = 0;
	unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 3);
	if (!px)
	{
		err = "cannot read " + path;
		return false;
	}
	if (w != GB_W || h != GB_H)
	{
		stbi_image_free(px);
		err = path + " is not 160x144";
		return false;
	}
	out.resize(GB_W * GB_H);
	for (int i = 0; i < GB_W * GB_H; ++i)
		out[i] = Luma(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
	stbi_image_free(px);
	return true;
}

// Snapshot the current GB frame as grayscale. DMG/SGB/compat frames are
// 2-bit shade indices (0 = lightest); native-CGB frames come from the raw
// BGR555 color framebuffer with full-range 5→8 bit expansion.
void CaptureFrameGray(SGB::Emulator &emu, uint8_t out[GB_W * GB_H])
{
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			out[i] = Luma((r5 << 3) | (r5 >> 2),
			              (g5 << 3) | (g5 >> 2),
			              (b5 << 3) | (b5 >> 2));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < GB_W * GB_H; ++i)
			out[i] = static_cast<uint8_t>((3 - (fb.pixels[i] & 3)) * 85);
	}
}

// The same frame in colour, for the UI. Native-CGB output is real BGR555;
// everything else is 2-bit shades, which the reference images also show as
// gray.
void CaptureFrameRgb(SGB::Emulator &emu, std::vector<uint8_t> &out)
{
	out.resize(GB_W * GB_H * 3);
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			out[i * 3 + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
			out[i * 3 + 1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
			out[i * 3 + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint8_t g = static_cast<uint8_t>((3 - (fb.pixels[i] & 3)) * 85);
			out[i * 3 + 0] = out[i * 3 + 1] = out[i * 3 + 2] = g;
		}
	}
}

// How far off the closest reference is, for a failure report: a handful of
// pixels reads very differently from a screen that never got there.
int ClosestDiff(const uint8_t *frame,
                const std::vector<std::vector<uint8_t>> &refs)
{
	int best = -1;
	for (const auto &ref : refs)
	{
		int n = 0;
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const int d = static_cast<int>(frame[i]) - static_cast<int>(ref[i]);
			if (d > TOLERANCE || d < -TOLERANCE) ++n;
		}
		if (best < 0 || n < best) best = n;
	}
	return best;
}

bool FramesMatch(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < GB_W * GB_H; ++i)
	{
		const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
		if (d > TOLERANCE || d < -TOLERANCE) return false;
	}
	return true;
}

std::string FlattenName(const std::string &name)
{
	std::string s = name;
	for (char &c : s)
		if (c == '/' || c == '\\' || c == ' ' || c == '(' || c == ')' || c == ',')
			c = '_';
	return s;
}

void DumpGrayPpm(const std::string &path, const uint8_t *gray)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", GB_W, GB_H);
	fwrite(gray, 1, GB_W * GB_H, f);
	fclose(f);
}

// Serial capture — blargg ROMs print their diagnosis here; keeping it makes
// failure reports self-explaining.
void OnSerialByte(void *user, uint8_t b)
{
	std::string *sink = static_cast<std::string *>(user);
	if (sink && sink->size() < 4096)
		sink->push_back(static_cast<char>(b));
}

std::string OneLine(const std::string &s, size_t cap)
{
	std::string r;
	for (char c : s)
	{
		if (r.size() >= cap) { r += "..."; break; }
		r += (c == '\n' || c == '\r') ? ' ' : c;
	}
	return r;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string LowerCopy(const std::string &s)
{
	std::string r = s;
	for (char &c : r) c = Lower(c);
	return r;
}

bool ContainsFold(const std::string &hay, const std::string &needle)
{
	return LowerCopy(hay).find(LowerCopy(needle)) != std::string::npos;
}

// "mooneye/acceptance/boot_div-S.gb" -> "mooneye".
std::string SuiteOfName(const std::string &name)
{
	const size_t slash = name.find_first_of("/\\");
	return slash == std::string::npos ? std::string("other") : name.substr(0, slash);
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return false; }
	out.resize(static_cast<size_t>(n));
	const size_t rd = fread(out.data(), 1, out.size(), f);
	fclose(f);
	return rd == out.size();
}

} // anonymous

std::string RomPath(const std::string &acid_dir, const std::string &rom)
{
	return JoinPath(JoinPath(acid_dir, kRomDir), rom);
}

std::string BaselinePath(const std::string &acid_dir, const std::string &name)
{
	return JoinPath(JoinPath(acid_dir, kBaselineDir), name);
}

std::string ImagePath(const std::string &acid_dir, const std::string &baseline,
                      const std::string &image)
{
	return JoinPath(BaselinePath(acid_dir, baseline), image);
}

const char *ModelName(Model m)
{
	return m == Model::CGB ? "CGB" : m == Model::SGB ? "SGB" : "DMG";
}

const char *StatusName(Status s)
{
	switch (s)
	{
		case Status::Pass: return "PASS";
		case Status::Fail: return "FAIL";
		case Status::Info: return "INFO";
		default:           return "ERROR";
	}
}

const char *MatchName(Match m)
{
	switch (m)
	{
		case Match::Same:    return "same";
		case Match::Differs: return "differs";
		case Match::NoImage: return "missing";
		case Match::NoFrame: return "noframe";
		case Match::NoRef:   return "noref";
		default:             return "none";
	}
}

int DiffPixels(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
               int count)
{
	if ((int)a.size() < count * 3 || (int)b.size() < count * 3) return -1;
	int n = 0;
	for (int i = 0; i < count; ++i)
	{
		const int ga = Luma(a[i * 3], a[i * 3 + 1], a[i * 3 + 2]);
		const int gb = Luma(b[i * 3], b[i * 3 + 1], b[i * 3 + 2]);
		if (ga - gb > TOLERANCE || gb - ga > TOLERANCE) ++n;
	}
	return n;
}

// Suites match on a substring so upstream's own names work - "mealybug"
// finds mealybug-tearoom-tests.
bool Filter::Matches(const Test &t) const
{
	if (models && !(models & ModelBit(t.model))) return false;
	if (!text.empty() && !ContainsFold(t.name, text)) return false;
	if (suites.empty()) return true;
	for (const std::string &s : suites)
		if (ContainsFold(t.suite, s)) return true;
	return false;
}

std::string Filter::Describe() const
{
	if (Empty()) return "all tests";
	std::string d;
	auto add = [&](const std::string &s) { if (!d.empty()) d += ", "; d += s; };
	if (!suites.empty())
	{
		std::string list;
		for (const std::string &s : suites)
		{
			if (!list.empty()) list += "+";
			list += s;
		}
		add("suite " + list);
	}
	if (models)
	{
		std::string list;
		for (Model m : { Model::DMG, Model::CGB, Model::SGB })
		{
			if (!(models & ModelBit(m))) continue;
			if (!list.empty()) list += "+";
			list += ModelName(m);
		}
		add("model " + list);
	}
	if (!text.empty()) add("name contains \"" + text + "\"");
	return d;
}

std::vector<std::string> SuitesOf(const std::vector<Test> &tests)
{
	std::vector<std::string> out;
	for (const Test &t : tests)
		if (std::find(out.begin(), out.end(), t.suite) == out.end())
			out.push_back(t.suite);
	return out;
}

bool LoadManifest(const char *acid_dir, std::vector<Test> &out, std::string &err)
{
	const std::string path = JoinPath(acid_dir ? acid_dir : "acid", "manifest.txt");
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
	{
		err = "cannot open " + path;
		return false;
	}

	char line[2048];
	while (fgets(line, sizeof line, f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty() || s[0] == '#') continue;

		// name|model|runtime|rom|pass;pass|fail;fail
		std::vector<std::string> fields;
		size_t start = 0;
		for (size_t i = 0; i <= s.size(); ++i)
		{
			if (i == s.size() || s[i] == '|')
			{
				fields.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		}
		if (fields.size() < 5)
		{
			fclose(f);
			err = "malformed manifest line: " + s;
			return false;
		}

		Test t;
		t.name = fields[0];
		t.model = fields[1] == "CGB" ? Model::CGB
		        : fields[1] == "SGB" ? Model::SGB : Model::DMG;
		t.runtime = atof(fields[2].c_str());
		t.rom = fields[3];
		auto split = [](const std::string &v, std::vector<std::string> &dst) {
			size_t p = 0;
			while (p < v.size())
			{
				size_t q = v.find(';', p);
				if (q == std::string::npos) q = v.size();
				if (q > p) dst.push_back(v.substr(p, q - p));
				p = q + 1;
			}
		};
		split(fields[4], t.pass_images);
		if (fields.size() >= 6) split(fields[5], t.fail_images);
		t.suite = SuiteOfName(t.name);
		out.push_back(std::move(t));
	}
	fclose(f);

	if (out.empty())
	{
		err = "no tests in " + path;
		return false;
	}
	return true;
}

namespace {

// One test, start to finish, on one core. Everything it touches is either
// its own emulator instance or a local, so several of these run in
// parallel without interfering.
Result RunOneTest(SGB::Emulator &emu, const std::string &dir, const Test &t,
                  bool dump_failures, const std::atomic<bool> &cancel,
                  const std::atomic<bool> *pause,
                  const std::function<void(int, int)> &tick, bool &aborted)
{
	aborted = false;
	Result r;
	std::string serial;
	std::vector<std::vector<uint8_t>> pass_refs, fail_refs;
	bool ref_error = false;
	// Pass and fail screens are the suite's own verdict, so they always
	// come from baseline/default however many other baselines exist.
	for (const std::string &img : t.pass_images)
	{
		std::vector<uint8_t> g;
		if (!LoadReferenceGray(ImagePath(dir, kDefaultBaseline, img), g, r.detail))
		{ ref_error = true; break; }
		pass_refs.push_back(std::move(g));
	}
	for (const std::string &img : t.fail_images)
	{
		if (ref_error) break;
		std::vector<uint8_t> g;
		if (!LoadReferenceGray(ImagePath(dir, kDefaultBaseline, img), g, r.detail))
		{ ref_error = true; break; }
		fail_refs.push_back(std::move(g));
	}

	std::vector<uint8_t> rom;
	if (!ref_error && !ReadWholeFile(RomPath(dir, t.rom), rom))
	{
		ref_error = true;
		r.detail = "cannot read ROM " + t.rom;
	}
	if (ref_error)
	{
		r.status = Status::Error;
		return r;
	}

	// Model setup must precede LoadROM — LoadROM cold-resets with it.
	emu.SetForceModel(t.model == Model::CGB ? 2 : t.model == Model::SGB ? 3 : 1);
	emu.SetRunMode(t.model == Model::SGB ? SGB::RunMode::SGB : SGB::RunMode::DMG);
	emu.SetClockMultiplier(1.0f);

	if (!emu.LoadROM(rom.data(), rom.size(), nullptr))
	{
		r.status = Status::Error;
		r.detail = "core rejected ROM " + t.rom;
		return r;
	}

	// SGB1 pushes ~61.2 GB frames per second; everything else 59.73.
	const double fps = (t.model == Model::SGB) ? 61.2 : 59.7275;
	// Match the shootout's wall clock: runtime + startup_time (1s) + 5s.
	const int frames_total = static_cast<int>(std::ceil((t.runtime + 6.0) * fps));

	// Armed here, not earlier: it points at a local, and every path above
	// returns without running a frame.
	emu.SetSerialSink(&OnSerialByte, &serial);
	struct SinkGuard
	{
		SGB::Emulator &e;
		~SinkGuard() { e.SetSerialSink(nullptr, nullptr); }
	} sink_guard{ emu };

	uint8_t frame[GB_W * GB_H];
	r.status = pass_refs.empty() ? Status::Info : Status::Fail;
	for (int fr = 0; fr < frames_total; ++fr)
	{
		emu.RunFrame();
		r.frames = fr + 1;

		if (!pass_refs.empty() || !fail_refs.empty())
		{
			CaptureFrameGray(emu, frame);
			bool decided = false;
			for (const auto &ref : pass_refs)
			{
				if (FramesMatch(frame, ref.data()))
				{
					r.status = Status::Pass;
					decided = true;
					break;
				}
			}
			if (!decided)
			{
				for (const auto &ref : fail_refs)
				{
					if (FramesMatch(frame, ref.data()))
					{
						r.status = Status::Fail;
						r.detail = "matched fail image";
						decided = true;
						break;
					}
				}
			}
			if (decided) break;
		}

		if ((fr & 63) == 63)
		{
			if (cancel.load(std::memory_order_relaxed))
			{
				aborted = true;
				return r;   // being cancelled; this one has no verdict
			}
			tick(fr + 1, frames_total);
			while (pause && pause->load(std::memory_order_relaxed) &&
			       !cancel.load(std::memory_order_relaxed))
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	if (r.status == Status::Fail && r.detail.empty())
	{
		CaptureFrameGray(emu, frame);
		const int off = ClosestDiff(frame, pass_refs);
		r.detail = "timeout after " + std::to_string(r.frames) + " frames";
		if (off >= 0)
			r.detail += "; " + std::to_string(off) + " px off the closest reference";
		if (!serial.empty())
			r.detail += "; serial: \"" + OneLine(serial, 160) + "\"";
	}
	if (r.status == Status::Fail && dump_failures)
	{
		CaptureFrameGray(emu, frame);
		DumpGrayPpm(JoinPath(dir, "_failures/" + FlattenName(t.name) + ".ppm"), frame);
	}
	CaptureFrameRgb(emu, r.shot);
	return r;
}

} // anonymous

std::string EnvOverrides()
{
	std::string out;
	char **env = ACID_ENVIRON;
	for (; env && *env; ++env)
	{
		if (std::strncmp(*env, "ACID_", 5) != 0) continue;
		if (!out.empty()) out += ' ';
		out += *env;
	}
	return out;
}

int DefaultThreadCount()
{
	const unsigned hw = std::thread::hardware_concurrency();
	return hw ? static_cast<int>(hw) : 1;
}

Summary Run(const RunOptions &opts)
{
	Summary sum;
	const std::string dir = opts.acid_dir ? opts.acid_dir : "acid";

	std::vector<Test> tests;
	std::string err;
	if (!LoadManifest(dir.c_str(), tests, err))
	{
		fprintf(stderr, "acid: %s\n", err.c_str());
		return sum;
	}
	if (!opts.filter.Empty())
	{
		std::vector<Test> kept;
		for (auto &t : tests)
			if (opts.filter.Matches(t)) kept.push_back(std::move(t));
		tests.swap(kept);
	}
	return RunTests(tests, opts);
}

Summary RunTests(const std::vector<Test> &tests, const RunOptions &opts,
                 std::vector<Result> *out_results)
{
	Summary sum;
	const std::string dir = opts.acid_dir ? opts.acid_dir : "acid";
	if (tests.empty()) return sum;

	const auto wall_start = std::chrono::steady_clock::now();

	if (opts.dump_failures)
	{
#ifdef _WIN32
		_mkdir(JoinPath(dir, "_failures").c_str());
#else
		mkdir(JoinPath(dir, "_failures").c_str(), 0755);
#endif
	}

	int nthreads = opts.threads > 0 ? opts.threads : DefaultThreadCount();
	if (nthreads > static_cast<int>(tests.size())) nthreads = static_cast<int>(tests.size());
	if (nthreads < 1) nthreads = 1;

	// Longest test first: the slowest ROM decides when the last core goes
	// idle, so starting it early keeps the tail short.
	std::vector<size_t> order(tests.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(),
	                 [&](size_t a, size_t b) { return tests[a].runtime > tests[b].runtime; });

	std::vector<Result> results(tests.size());
	std::vector<char>   ran(tests.size(), 0);   // false for cancelled-away tests
	std::atomic<size_t> next_test{0};
	std::atomic<bool>   cancel{false};
	std::atomic<int>    live{nthreads};
	std::mutex          done_mu;
	// What the workers have to say, in the order they said it.
	struct Event { size_t index; int kind; int done; int total; };
	enum { EvStart, EvRunning, EvFinished };
	std::vector<Event> events;

	// Every worker owns a core of its own; the scoped bind keeps the GB
	// side's host hooks pointed at it instead of the singleton.
	auto worker = [&]() {
		SGB::Emulator emu;
		SGB::ScopedActiveEmulator bind(emu);
		if (emu.Init())
		{
			for (;;)
			{
				const size_t slot = next_test.fetch_add(1);
				if (slot >= order.size() || cancel.load()) break;
				const size_t i = order[slot];
				{
					std::lock_guard<std::mutex> lk(done_mu);
					events.push_back({ i, EvStart, 0, 0 });
				}
				auto tick = [&](int done, int total) {
					std::lock_guard<std::mutex> lk(done_mu);
					events.push_back({ i, EvRunning, done, total });
				};
				bool aborted = false;
				Result r = RunOneTest(emu, dir, tests[i], opts.dump_failures,
				                      cancel, opts.pause, tick, aborted);
				if (aborted) break;
				std::lock_guard<std::mutex> lk(done_mu);
				results[i] = std::move(r);
				ran[i] = 1;
				events.push_back({ i, EvFinished, results[i].frames, results[i].frames });
			}
		}
		live.fetch_sub(1);
	};

	std::vector<std::thread> pool;
	pool.reserve(nthreads);
	for (int i = 0; i < nthreads; ++i) pool.emplace_back(worker);

	// Results are reported from THIS thread, so a UI caller's callbacks
	// stay on the thread that owns its windows.
	size_t reported = 0;
	size_t last_seen = 0;
	for (;;)
	{
		std::vector<Event> batch;
		{
			std::lock_guard<std::mutex> lk(done_mu);
			batch.swap(events);
		}
		for (const auto &ev : batch)
		{
			const size_t idx = ev.index;
			last_seen = idx;
			if (ev.kind == EvStart)
			{
				if (opts.on_start)
					opts.on_start(opts.user, static_cast<int>(idx), tests[idx]);
				continue;
			}
			if (ev.kind == EvRunning)
			{
				if (opts.on_running)
					opts.on_running(opts.user, static_cast<int>(idx), ev.done, ev.total);
				continue;
			}
			const Result &r = results[idx];
			switch (r.status)
			{
				case Status::Pass:  ++sum.passed; break;
				case Status::Fail:  ++sum.failed; break;
				case Status::Info:  ++sum.info;   break;
				case Status::Error: ++sum.errors; break;
			}
			++sum.total;
			++reported;
			if (opts.on_result)
				opts.on_result(opts.user, static_cast<int>(idx), tests[idx], r);
		}
		if (opts.progress && !batch.empty() &&
		    !opts.progress(opts.user, static_cast<int>(reported),
		                   static_cast<int>(tests.size()), tests[last_seen],
		                   static_cast<int>(reported), static_cast<int>(tests.size())))
			cancel.store(true);

		if (live.load() == 0)
		{
			std::lock_guard<std::mutex> lk(done_mu);
			if (events.empty()) break;
			continue;
		}
		if (batch.empty())
		{
			if (opts.progress &&
			    !opts.progress(opts.user, static_cast<int>(reported),
			                   static_cast<int>(tests.size()), tests[last_seen],
			                   static_cast<int>(reported), static_cast<int>(tests.size())))
				cancel.store(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	for (auto &th : pool) th.join();

	const bool cancelled = cancel.load();
	sum.cancelled = cancelled ? 1 : 0;

	// The report goes out in manifest order, whichever worker produced it.
	if (opts.report && *opts.report)
	{
		ReportInfo info;
		info.env       = EnvOverrides();
		info.filter    = opts.filter.Describe();
		info.source    = dir;
		info.threads   = nthreads;
		info.cancelled = cancelled;
		info.seconds   = std::chrono::duration<double>(
		                     std::chrono::steady_clock::now() - wall_start).count();
		std::vector<ReportRow> rows;
		rows.reserve(tests.size());
		for (size_t i = 0; i < tests.size(); ++i)
			rows.push_back({ &tests[i], ran[i] ? &results[i] : nullptr });
		const std::string path = JoinPath(dir, opts.report);
		std::string err;
		WriteReport(path.c_str(), FormatFromPath(opts.report), rows, info, err);
	}
	if (out_results) *out_results = std::move(results);
	return sum;
}

} // namespace AcidTests
