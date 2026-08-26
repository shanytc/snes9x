/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "acid.h"
#include "sgb.h"
#include "gb_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "stb_image.h"


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
void CaptureFrameGray(uint8_t out[GB_W * GB_H])
{
	if (S9xSGBIsCgbRender())
	{
		const uint16_t *fb = S9xSGBGetCgbColorFB();
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
		const SGB::FrameBuffer &fb = SGB::Instance().GetFrameBuffer();
		for (int i = 0; i < GB_W * GB_H; ++i)
			out[i] = static_cast<uint8_t>((3 - (fb.pixels[i] & 3)) * 85);
	}
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
std::string *g_serial_sink = nullptr;

void OnSerialByte(uint8_t b)
{
	if (g_serial_sink && g_serial_sink->size() < 4096)
		g_serial_sink->push_back(static_cast<char>(b));
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
	if (opts.filter && *opts.filter)
	{
		std::vector<Test> kept;
		for (auto &t : tests)
			if (t.name.find(opts.filter) != std::string::npos)
				kept.push_back(std::move(t));
		tests.swap(kept);
	}

	S9xSGBInit();

	std::string serial;
	g_serial_sink = &serial;
	SGB::SetSerialCallback(&OnSerialByte);

	if (opts.dump_failures)
	{
#ifdef _WIN32
		_mkdir(JoinPath(dir, "_failures").c_str());
#else
		mkdir(JoinPath(dir, "_failures").c_str(), 0755);
#endif
	}

	const std::string report_path = JoinPath(dir, "results.txt");
	FILE *report = fopen(report_path.c_str(), "wb");

	std::vector<std::vector<uint8_t>> pass_refs, fail_refs;
	std::vector<uint8_t> frame(GB_W * GB_H);
	bool cancelled = false;

	for (size_t ti = 0; ti < tests.size() && !cancelled; ++ti)
	{
		const Test &t = tests[ti];
		Result r;
		serial.clear();

		// SGB1 pushes ~61.2 GB frames per second; everything else 59.73.
		const double fps = (t.model == Model::SGB) ? 61.2 : 59.7275;
		// Match the shootout's wall clock: runtime + startup_time (1s) + 5s.
		const int frames_total = static_cast<int>(std::ceil((t.runtime + 6.0) * fps));

		if (opts.progress &&
		    !opts.progress(opts.user, static_cast<int>(ti),
		                   static_cast<int>(tests.size()), t, 0, frames_total))
		{
			cancelled = true;
			break;
		}

		pass_refs.clear();
		fail_refs.clear();
		bool ref_error = false;
		for (const std::string &img : t.pass_images)
		{
			std::vector<uint8_t> g;
			if (!LoadReferenceGray(JoinPath(dir, img), g, r.detail)) { ref_error = true; break; }
			pass_refs.push_back(std::move(g));
		}
		for (const std::string &img : t.fail_images)
		{
			std::vector<uint8_t> g;
			if (ref_error) break;
			if (!LoadReferenceGray(JoinPath(dir, img), g, r.detail)) { ref_error = true; break; }
			fail_refs.push_back(std::move(g));
		}

		std::vector<uint8_t> rom;
		if (!ref_error && !ReadWholeFile(JoinPath(dir, t.rom), rom))
		{
			ref_error = true;
			r.detail = "cannot read ROM " + t.rom;
		}

		if (ref_error)
		{
			r.status = Status::Error;
		}
		else
		{
			// Model setup must precede LoadROM — LoadROM cold-resets with it.
			S9xSGBSetForceModel(t.model == Model::CGB ? 2 :
			                    t.model == Model::SGB ? 3 : 1);
			S9xSGBSetRunMode(t.model == Model::SGB ? 1 : 0);
			S9xSGBSetClockMultiplier(1.0f);

			if (!S9xSGBLoadROMBytes(rom.data(), rom.size(), nullptr))
			{
				r.status = Status::Error;
				r.detail = "core rejected ROM " + t.rom;
			}
			else
			{
				r.status = pass_refs.empty() ? Status::Info : Status::Fail;
				for (int fr = 0; fr < frames_total; ++fr)
				{
					S9xSGBRunFrame();
					r.frames = fr + 1;

					if (!pass_refs.empty() || !fail_refs.empty())
					{
						CaptureFrameGray(frame.data());
						bool decided = false;
						for (const auto &ref : pass_refs)
						{
							if (FramesMatch(frame.data(), ref.data()))
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
								if (FramesMatch(frame.data(), ref.data()))
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

					if (opts.progress && (fr & 63) == 63 &&
					    !opts.progress(opts.user, static_cast<int>(ti),
					                   static_cast<int>(tests.size()), t,
					                   fr + 1, frames_total))
					{
						cancelled = true;
						break;
					}
				}

				if (r.status == Status::Fail && r.detail.empty())
				{
					r.detail = "timeout after " + std::to_string(r.frames) + " frames";
					if (!serial.empty())
						r.detail += "; serial: \"" + OneLine(serial, 160) + "\"";
				}
				if (r.status == Status::Fail && opts.dump_failures)
				{
					CaptureFrameGray(frame.data());
					DumpGrayPpm(JoinPath(dir, "_failures/" + FlattenName(t.name) + ".ppm"),
					            frame.data());
				}
			}
		}

		switch (r.status)
		{
			case Status::Pass:  ++sum.passed; break;
			case Status::Fail:  ++sum.failed; break;
			case Status::Info:  ++sum.info;   break;
			case Status::Error: ++sum.errors; break;
		}
		++sum.total;

		if (report)
		{
			static const char *kStatus[] = { "PASS", "FAIL", "INFO", "ERROR" };
			fprintf(report, "%-5s %-60s %s\n",
			        kStatus[static_cast<int>(r.status)], t.name.c_str(),
			        r.detail.c_str());
			fflush(report);
		}
		if (opts.on_result)
			opts.on_result(opts.user, static_cast<int>(ti), t, r);
	}

	sum.cancelled = cancelled ? 1 : 0;
	if (report)
	{
		fprintf(report, "\n%d/%d passed (%d failed, %d info, %d errors%s)\n",
		        sum.passed, sum.passed + sum.failed, sum.failed, sum.info,
		        sum.errors, cancelled ? ", cancelled" : "");
		fclose(report);
	}

	SGB::SetSerialCallback(nullptr);
	g_serial_sink = nullptr;
	S9xSGBSetForceModel(0);
	return sum;
}

} // namespace AcidTests
