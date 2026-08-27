/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "acid_baseline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
// Only FindFirstFile is wanted here, and min/max as macros would break
// std::min/std::max in anything that later includes this.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "stb_image.h"

namespace AcidTests {

namespace {

std::string Join(const std::string &dir, const std::string &rel)
{
	if (dir.empty()) return rel;
	std::string p = dir;
	if (p.back() != '/' && p.back() != '\\') p += '/';
	return p + rel;
}

bool EndsWith(const std::string &s, const char *suffix)
{
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void MakeDir(const std::string &path)
{
#ifdef _WIN32
	_mkdir(path.c_str());
#else
	mkdir(path.c_str(), 0755);
#endif
}

// Immediate subdirectory names of `dir`, unsorted. Empty when it is not
// there.
std::vector<std::string> SubDirs(const std::string &dir)
{
	std::vector<std::string> out;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		const std::string n = fd.cFileName;
		if (n == "." || n == "..") continue;
		out.push_back(n);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if (!d) return out;
	while (struct dirent *e = readdir(d))
	{
		const std::string n = e->d_name;
		if (n == "." || n == "..") continue;
		struct stat sb;
		if (stat((dir + "/" + n).c_str(), &sb) == 0 && S_ISDIR(sb.st_mode))
			out.push_back(n);
	}
	closedir(d);
#endif
	return out;
}

// Create every directory along `path`, which names a file.
void MakeParents(const std::string &path)
{
	for (size_t i = 0; i < path.size(); ++i)
	{
		if (path[i] != '/' && path[i] != '\\') continue;
		if (i == 0) continue;
		MakeDir(path.substr(0, i));
	}
}

} // anonymous

namespace {

std::string DerivedName(const std::string &name)
{
	std::string s = name;
	if      (EndsWith(s, ".gbc")) s.resize(s.size() - 4);
	else if (EndsWith(s, ".gb"))  s.resize(s.size() - 3);
	return s + ".png";
}

// One 160x144 RGB frame off disk.
bool LoadPng(const std::string &path, std::vector<uint8_t> &rgb)
{
	int w = 0, h = 0, comp = 0;
	unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 3);
	if (!px) return false;
	if (w != kShotWidth || h != kShotHeight)
	{
		stbi_image_free(px);
		return false;
	}
	rgb.assign(px, px + (size_t)w * h * 3);
	stbi_image_free(px);
	return true;
}

} // anonymous

std::string Baseline::ImageFor(const Test &t) const
{
	const auto it = images.find(t.name);
	return it != images.end() ? it->second : DerivedName(t.name);
}

std::vector<std::string> Baseline::CandidatesFor(const Test &t) const
{
	std::vector<std::string> out;
	auto add = [&](const std::string &p) {
		if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end())
			out.push_back(p);
	};
	const auto it = images.find(t.name);
	if (it != images.end()) add(it->second);
	add(DerivedName(t.name));
	for (const std::string &p : t.pass_images) add(p);
	return out;
}

bool LoadBaseline(const char *dir, Baseline &out, std::string &err)
{
	out = Baseline();
	out.dir = dir ? dir : "";
	if (out.dir.empty())
	{
		err = "no baseline directory given";
		return false;
	}

	// No index is fine - the derived paths still find a dump laid out the
	// way the shootout's reference images are.
	FILE *f = fopen(Join(out.dir, "baseline.txt").c_str(), "rb");
	if (!f)
	{
		struct stat st;
		if (stat(out.dir.c_str(), &st) != 0)
		{
			err = "cannot open " + out.dir;
			return false;
		}
		return true;
	}

	char line[2048];
	while (fgets(line, sizeof line, f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty()) continue;
		if (s[0] == '#')
		{
			// "# emulator: X" / "# generated: X" carry the provenance.
			const size_t colon = s.find(':');
			if (colon == std::string::npos) continue;
			std::string key = s.substr(1, colon - 1), val = s.substr(colon + 1);
			while (!key.empty() && key.front() == ' ') key.erase(key.begin());
			while (!key.empty() && key.back() == ' ') key.pop_back();
			while (!val.empty() && val.front() == ' ') val.erase(val.begin());
			if (key == "emulator")  out.title   = val;
			if (key == "generated") out.created = val;
			continue;
		}
		// name|status|frames|image
		std::vector<std::string> f_;
		size_t start = 0;
		for (size_t i = 0; i <= s.size(); ++i)
			if (i == s.size() || s[i] == '|')
			{
				f_.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		if (f_.size() >= 4 && !f_[3].empty()) out.images[f_[0]] = f_[3];
	}
	fclose(f);
	return true;
}

std::vector<Baseline> DiscoverBaselines(const char *acid_dir)
{
	std::vector<Baseline> out;
	const std::string root = acid_dir ? acid_dir : "acid";
	std::vector<std::string> names = SubDirs(Join(root, kBaselineDir));
	// default carries the suite's own screens, so it leads.
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		if ((a == kDefaultBaseline) != (b == kDefaultBaseline))
			return a == kDefaultBaseline;
		return a < b;
	});
	for (const std::string &n : names)
	{
		Baseline b;
		std::string err;
		if (!LoadBaseline(BaselinePath(root, n).c_str(), b, err)) continue;
		b.name = n;
		out.push_back(std::move(b));
	}
	return out;
}

bool LoadBaselineFrame(const Baseline &b, const Test &t,
                       std::vector<uint8_t> &rgb,
                       const std::vector<uint8_t> *prefer, int *diff_px)
{
	if (b.dir.empty()) return false;
	const bool rank = prefer && !prefer->empty();
	bool got  = false;
	int  best = -1;
	std::vector<uint8_t> cur;
	for (const std::string &rel : b.CandidatesFor(t))
	{
		if (!LoadPng(Join(b.dir, rel), cur)) continue;
		if (!rank)
		{
			rgb = std::move(cur);
			if (diff_px) *diff_px = -1;
			return true;
		}
		const int n = DiffPixels(*prefer, cur, kShotWidth * kShotHeight);
		if (!got || n < best) { rgb = cur; best = n; got = true; }
		if (best == 0) break;   // exact; nothing else can beat it
	}
	if (got && diff_px) *diff_px = best;
	return got;
}

Match CompareToBaseline(const Baseline &b, const Test &t,
                        const std::vector<uint8_t> &shot, int &diff_px)
{
	diff_px = 0;
	if (b.dir.empty()) return Match::None;
	std::vector<uint8_t> ref;
	int best = -1;
	if (!LoadBaselineFrame(b, t, ref, &shot, &best)) return Match::NoImage;
	if (shot.empty()) return Match::NoFrame;
	if (best < 0) return Match::NoImage;
	diff_px = best;
	return best ? Match::Differs : Match::Same;
}

BaselineClash CheckBaseline(const char *dir, const std::vector<ReportRow> &rows)
{
	BaselineClash c;
	const std::string root = dir ? dir : "";
	if (root.empty()) return c;

	Baseline existing;
	std::string err;
	LoadBaseline(root.c_str(), existing, err);   // for the index's provenance
	c.title   = existing.title;
	c.created = existing.created;

	struct stat sb;
	c.index = stat(Join(root, "baseline.txt").c_str(), &sb) == 0;

	// The paths WriteBaseline would use, which ignore any existing index.
	Baseline naming;
	for (const ReportRow &r : rows)
	{
		if (!r.test || !r.result || r.result->shot.empty()) continue;
		if (stat(Join(root, naming.ImageFor(*r.test)).c_str(), &sb) == 0)
			++c.images;
	}
	return c;
}

int WriteBaseline(const char *dir, const std::vector<ReportRow> &rows,
                  const ReportInfo &info, std::string &err)
{
	const std::string root = dir ? dir : "";
	if (root.empty())
	{
		err = "no baseline directory given";
		return -1;
	}
	// A folder with a manifest is the test suite itself, whose reference
	// PNGs sit at the very paths a save would write.
	struct stat sb;
	if (stat(Join(root, "manifest.txt").c_str(), &sb) == 0 &&
	    stat(Join(root, "baseline.txt").c_str(), &sb) != 0)
	{
		err = "refusing to overwrite the test suite in " + root +
		      " - save the baseline somewhere else";
		return -1;
	}
	MakeParents(root + "/");
	MakeDir(root);

	Baseline naming;   // for ImageFor's derived paths
	std::string index;
	index += "# Acid Tests baseline - captured GB frames, 160x144 PNG.\n";
	index += "# emulator: " + info.title + "\n";
	index += "# generated: " +
	         (info.generated.empty() ? Timestamp() : info.generated) + "\n";
	if (!info.env.empty())       index += "# overrides: " + info.env + "\n";
	index += "# name|status|frames|image\n";

	int written = 0;
	for (const ReportRow &r : rows)
	{
		if (!r.test || !r.result || r.result->shot.empty()) continue;
		const std::string rel  = naming.ImageFor(*r.test);
		const std::string path = Join(root, rel);
		MakeParents(path);

		const std::vector<uint8_t> png =
			EncodePng(r.result->shot.data(), kShotWidth, kShotHeight);
		FILE *f = fopen(path.c_str(), "wb");
		if (!f)
		{
			err = "cannot write " + path;
			return -1;
		}
		const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
		fclose(f);
		if (!ok)
		{
			err = "short write to " + path;
			return -1;
		}

		char buf[64];
		snprintf(buf, sizeof buf, "|%d|", r.result->frames);
		index += r.test->name;
		index += "|";
		index += StatusName(r.result->status);
		index += buf;
		index += rel;
		index += "\n";
		++written;
	}

	FILE *f = fopen(Join(root, "baseline.txt").c_str(), "wb");
	if (!f)
	{
		err = "cannot write " + Join(root, "baseline.txt");
		return -1;
	}
	fwrite(index.data(), 1, index.size(), f);
	fclose(f);
	return written;
}

} // namespace AcidTests
