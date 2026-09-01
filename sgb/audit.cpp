/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "audit.h"
#include "acid_report.h"   // EncodePng
#include "sgb.h"
#include "gb_cart.h"
#include "../snes9x.h"     // DECOMPOSE_PIXEL - BlitScreen emits host pixels

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#endif

#include "stb_image.h"

#ifdef UNZIP_SUPPORT
#include "unzip.h"
#endif

namespace AuditTests {

namespace {

// A channel this far off still counts as the same pixel: display pipelines
// round 5-bit color both ways.
constexpr int kTol = 8;

std::string Join(const std::string &dir, const std::string &rel)
{
	if (dir.empty()) return rel;
	std::string p = dir;
	if (p.back() != '/' && p.back() != '\\') p += '/';
	return p + rel;
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

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool EndsWithFold(const std::string &s, const char *suffix)
{
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; ++i)
		if (Lower(s[s.size() - n + i]) != Lower(suffix[i])) return false;
	return true;
}

// List one directory: names only, files and subdirectories separated.
void ListDir(const std::string &dir, std::vector<std::string> &files,
             std::vector<std::string> &dirs)
{
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		const std::string n = fd.cFileName;
		if (n == "." || n == "..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(n);
		else                                                files.push_back(n);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	while (dirent *e = readdir(d))
	{
		const std::string n = e->d_name;
		if (n == "." || n == "..") continue;
		struct stat st;
		if (stat(Join(dir, n).c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) dirs.push_back(n);
		else                     files.push_back(n);
	}
	closedir(d);
#endif
	std::sort(files.begin(), files.end());
	std::sort(dirs.begin(), dirs.end());
}

// Load a GB image from a plain file or the first .gb/.gbc inside a zip.
// `gbc_file` reports which extension the image itself carried.
bool LoadGBImage(const std::string &path, std::vector<uint8_t> &out,
                 bool &gbc_file)
{
	if (EndsWithFold(path, ".zip"))
	{
#ifdef UNZIP_SUPPORT
		unzFile z = unzOpen(path.c_str());
		if (!z) return false;
		bool ok = false;
		if (unzGoToFirstFile(z) == UNZ_OK)
		{
			do
			{
				char name[512];
				unz_file_info info;
				if (unzGetCurrentFileInfo(z, &info, name, sizeof name,
				                          NULL, 0, NULL, 0) != UNZ_OK)
					break;
				const std::string n = name;
				if (!EndsWithFold(n, ".gb") && !EndsWithFold(n, ".gbc"))
					continue;
				if (unzOpenCurrentFile(z) != UNZ_OK) break;
				out.resize(info.uncompressed_size);
				const int rd = unzReadCurrentFile(z, out.data(),
				                                  (unsigned)out.size());
				unzCloseCurrentFile(z);
				ok = rd == (int)out.size() && !out.empty();
				gbc_file = EndsWithFold(n, ".gbc");
				break;
			} while (unzGoToNextFile(z) == UNZ_OK);
		}
		unzClose(z);
		return ok;
#else
		return false;
#endif
	}
	gbc_file = EndsWithFold(path, ".gbc");
	return ReadWholeFile(path, out);
}

// Header-only probe for the scanner: inflates just the first 0x150 bytes
// of a zipped image, so classifying thousands of zips stays quick.
bool ProbeGBHeader(const std::string &path, std::vector<uint8_t> &hdr,
                   uint32_t &full_size, bool &gbc_file)
{
	const size_t kHdr = 0x150;
	if (EndsWithFold(path, ".zip"))
	{
#ifdef UNZIP_SUPPORT
		unzFile z = unzOpen(path.c_str());
		if (!z) return false;
		bool ok = false;
		if (unzGoToFirstFile(z) == UNZ_OK)
		{
			do
			{
				char name[512];
				unz_file_info info;
				if (unzGetCurrentFileInfo(z, &info, name, sizeof name,
				                          NULL, 0, NULL, 0) != UNZ_OK)
					break;
				const std::string n = name;
				if (!EndsWithFold(n, ".gb") && !EndsWithFold(n, ".gbc"))
					continue;
				if (info.uncompressed_size < kHdr) break;
				if (unzOpenCurrentFile(z) != UNZ_OK) break;
				hdr.resize(kHdr);
				const int rd = unzReadCurrentFile(z, hdr.data(), (unsigned)kHdr);
				unzCloseCurrentFile(z);
				ok = rd == (int)kHdr;
				full_size = (uint32_t)info.uncompressed_size;
				gbc_file  = EndsWithFold(n, ".gbc");
				break;
			} while (unzGoToNextFile(z) == UNZ_OK);
		}
		unzClose(z);
		return ok;
#else
		return false;
#endif
	}
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < (long)kHdr) { fclose(f); return false; }
	hdr.resize(kHdr);
	const bool ok = fread(hdr.data(), 1, kHdr, f) == kHdr;
	fclose(f);
	full_size = (uint32_t)n;
	gbc_file  = EndsWithFold(path, ".gbc");
	return ok;
}

std::string Sanitize(const std::string &s)
{
	std::string r = s;
	for (char &c : r)
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
		    c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	return r;
}

std::string Stem(const std::string &file)
{
	const size_t dot = file.find_last_of('.');
	return dot == std::string::npos ? file : file.substr(0, dot);
}

/*--------------------------------------------------------------------------
  Frame capture off a private core
--------------------------------------------------------------------------*/

// Captures live PNG-compressed: a shade panel deflates to a few KB, which
// is what lets a whole-library run keep every shot in memory.
void CapturePanelRgb(SGB::Emulator &emu, Capture &cap, int frame)
{
	cap.frame = frame;
	cap.w = kPanelW; cap.h = kPanelH;
	static thread_local std::vector<uint8_t> rgb;
	rgb.resize(kPanelW * kPanelH * 3);
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < kPanelW * kPanelH; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			rgb[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
			rgb[i * 3 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
			rgb[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < kPanelW * kPanelH; ++i)
		{
			const uint8_t g = (uint8_t)((3 - (fb.pixels[i] & 3)) * 85);
			rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = g;
		}
	}
	cap.png = AcidTests::EncodePng(rgb.data(), cap.w, cap.h);
}

// SGB modes render into palettes; BlitScreen's composite is the picture the
// user sees - border and panel both - in HOST pixel format (BUILD_PIXEL),
// so it must be split with DECOMPOSE_PIXEL, not read as raw BGR555.
void CaptureSgbRgb(SGB::Emulator &emu, Capture &cap, int frame)
{
	cap.frame = frame;
	cap.w = kSgbW; cap.h = kSgbH;
	static thread_local std::vector<uint16_t> bgr;
	bgr.assign(kSgbW * kSgbH, 0);
	emu.BlitScreen(bgr.data(), kSgbW);
	static thread_local std::vector<uint8_t> rgb;
	rgb.resize(kSgbW * kSgbH * 3);
	for (int i = 0; i < kSgbW * kSgbH; ++i)
	{
		const uint16_t c = bgr[i];
		uint32_t r5, g5, b5;
		DECOMPOSE_PIXEL(c, r5, g5, b5);
		rgb[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
		rgb[i * 3 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
		rgb[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
	}
	cap.png = AcidTests::EncodePng(rgb.data(), cap.w, cap.h);
}

int DiffRgb(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	if (a.size() != b.size() || a.empty()) return -1;
	int n = 0;
	for (size_t i = 0; i < a.size(); i += 3)
	{
		for (int k = 0; k < 3; ++k)
		{
			const int d = (int)a[i + k] - (int)b[i + k];
			if (d > kTol || d < -kTol) { ++n; break; }
		}
	}
	return n;
}

// Grayscale panel for the slip history: cheap to keep hundreds of.
void PanelGray(SGB::Emulator &emu, std::vector<uint8_t> &out)
{
	out.resize(kPanelW * kPanelH);
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < kPanelW * kPanelH; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			out[i] = (uint8_t)((r5 * 299 + g5 * 587 + b5 * 114) * 255 / (31 * 1000));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < kPanelW * kPanelH; ++i)
			out[i] = (uint8_t)((3 - (fb.pixels[i] & 3)) * 85);
	}
}

bool GrayUniform(const std::vector<uint8_t> &g)
{
	for (size_t i = 1; i < g.size(); ++i)
		if (g[i] != g[0]) return false;
	return true;
}

int DiffGray(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	if (a.size() != b.size() || a.empty()) return -1;
	int n = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		const int d = (int)a[i] - (int)b[i];
		if (d > kTol || d < -kTol) ++n;
	}
	return n;
}

void RgbToGray(const std::vector<uint8_t> &rgb, std::vector<uint8_t> &out)
{
	out.resize(rgb.size() / 3);
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = (uint8_t)((rgb[i * 3] * 299 + rgb[i * 3 + 1] * 587 +
		                    rgb[i * 3 + 2] * 114) / 1000);
}

} // anonymous

/*--------------------------------------------------------------------------
  DMG boot-logo detector
--------------------------------------------------------------------------*/

// The settled DMG boot screen, rebuilt from the cart's own header logo
// ($0104-$0133): 2 bytes per 8x8 tile, a nibble per 4-pixel source row,
// every source pixel doubled both ways. 96x16 mask, drawn at (32,64).
std::vector<uint8_t> DecodeHeaderLogo(const std::vector<uint8_t> &rom)
{
	std::vector<uint8_t> mask;
	if (rom.size() < 0x134) return mask;
	uint8_t src[8][48] = {};
	int set = 0;
	for (int i = 0; i < 48; ++i)
	{
		const uint8_t byte = rom[0x104 + i];
		const int col = ((i % 24) / 2) * 4;
		const int row = (i / 24) * 4 + (i % 2) * 2;
		for (int n = 0; n < 2; ++n)
		{
			const uint8_t nib = (byte >> (n ? 0 : 4)) & 0x0F;
			for (int b = 0; b < 4; ++b)
				if ((nib >> (3 - b)) & 1) { src[row + n][col + b] = 1; ++set; }
		}
	}
	// An all-blank "logo" would match any blank screen - refuse it.
	if (set < 32) return mask;
	mask.resize(kLogoW * kLogoH);
	for (int y = 0; y < kLogoH; ++y)
		for (int x = 0; x < kLogoW; ++x)
			mask[y * kLogoW + x] = src[y / 2][x / 2];
	return mask;
}

// True when the panel is exactly the settled boot-logo screen: the logo
// dark at (32,64), everything else blank. The (R) tile beside the logo is
// boot-ROM art, not header data, so its cell is left unjudged.
bool GrayShowsBootLogo(const std::vector<uint8_t> &gray,
                       const std::vector<uint8_t> &logo)
{
	if (logo.size() != (size_t)kLogoW * kLogoH ||
	    gray.size() != (size_t)kPanelW * kPanelH) return false;
	for (int y = 0; y < kPanelH; ++y)
		for (int x = 0; x < kPanelW; ++x)
		{
			const bool dark = gray[y * kPanelW + x] < 128;
			const bool in_logo = x >= kLogoX && x < kLogoX + kLogoW &&
			                     y >= kLogoY && y < kLogoY + kLogoH;
			if (in_logo)
			{
				if (dark != (logo[(y - kLogoY) * kLogoW + (x - kLogoX)] != 0))
					return false;
			}
			else if (x >= kLogoX + kLogoW && x < kLogoX + kLogoW + 8 &&
			         y >= kLogoY && y < kLogoY + kLogoH)
				continue;
			else if (dark) return false;
		}
	return true;
}

/*--------------------------------------------------------------------------
  Combos
--------------------------------------------------------------------------*/

const char *ComboId(Combo c)
{
	static const char *ids[] = { "gb+bios", "gb", "gbc+bios", "gbc",
	                             "sgb1", "sgb2", "sgb1+cb", "sgb2+cb" };
	return ids[(int)c];
}

const char *ComboName(Combo c)
{
	static const char *names[] = { "GB BIOS", "GB", "GBC BIOS", "GBC",
	                               "SGB1", "SGB2",
	                               "SGB1 Border", "SGB2 Border" };
	return names[(int)c];
}

bool ComboIsRing(Combo c)   { return c >= Combo::SGB1; }
bool ComboNeedsBoot(Combo c) { return c == Combo::GB_Bios || c == Combo::GBC_Bios; }

const char *ComboShotId(Combo c)
{
	return ComboIsRing(c) ? "border" : "title";
}

const char *MatchName(Match m)
{
	switch (m)
	{
		case Match::Same:    return "same";
		case Match::Differs: return "differs";
		case Match::Slip:    return "slip";
		case Match::NoBase:  return "nobase";
		case Match::Skip:    return "skip";
		case Match::Error:   return "error";
		default:             return "none";
	}
}

bool RomSgbEnhanced(const Rom &r) { return r.sgb_flag == 0x03; }
bool RomCgbCapable(const Rom &r)  { return (r.cgb_flag & 0x80) != 0; }
bool RomCgbOnly(const Rom &r)     { return r.cgb_flag == 0xC0; }

std::string RomBootClass(const Rom &r)
{
	if (RomCgbOnly(r))
		return RomSgbEnhanced(r) ? "GBC+SGB" : "GBC only";
	if (RomCgbCapable(r))
		return RomSgbEnhanced(r) ? "Tri-boot" : "GB+GBC";
	return RomSgbEnhanced(r) ? "GB+SGB" : "GB";
}

// The same rules the app's boot-policy menu applies: everything runs on GB,
// GBC and SGB except a CGB-only cart (locked out of mono consoles); the
// border columns need a cart that declares SGB support.
bool ComboApplies(Combo c, const Rom &r)
{
	switch (c)
	{
		case Combo::GB:
		case Combo::GB_Bios:
		case Combo::SGB1:
		case Combo::SGB2:
			return !RomCgbOnly(r);
		case Combo::GBC:
		case Combo::GBC_Bios:
			return true;
		case Combo::SGB1_CB:
		case Combo::SGB2_CB:
			// Only carts that declare SGB support upload custom borders.
			return RomSgbEnhanced(r) && !RomCgbOnly(r);
		default:
			return false;
	}
}

bool Capture::Decode(std::vector<uint8_t> &rgb) const
{
	if (png.empty()) return false;
	int dw = 0, dh = 0, comp = 0;
	unsigned char *px = stbi_load_from_memory(png.data(), (int)png.size(),
	                                          &dw, &dh, &comp, 3);
	if (!px) return false;
	const bool ok = dw == w && dh == h;
	if (ok) rgb.assign(px, px + (size_t)dw * dh * 3);
	stbi_image_free(px);
	return ok;
}

bool RomResult::AllPassed(const Rom &r) const
{
	bool any = false;
	for (int c = 0; c < kComboCount; ++c)
	{
		if (!ComboApplies((Combo)c, r)) continue;
		const Match m = combos[c].Cell();
		if (m == Match::Skip) continue;   // no boot ROM staged, say
		if (m != Match::Same) return false;
		any = true;
	}
	return any;
}

/*--------------------------------------------------------------------------
  Scanning
--------------------------------------------------------------------------*/

// Mapper-family display bucket, from the core's own detection.
const char *CartTypeBucket(const SGB::Cart &c)
{
	using SGB::MbcType;
	switch (c.mbc.type)
	{
		case MbcType::SachenMMC1:  return "Sachen";
		case MbcType::Rocket:      return "Rocket";
		case MbcType::NtOld1:
		case MbcType::NtOld2:
		case MbcType::NtNew:
		case MbcType::PokeJadeDia: return "Makon/NT";
		case MbcType::BBD:
		case MbcType::Ggb81:
		case MbcType::Hitek:       return "BBD";
		case MbcType::Sintax:
		case MbcType::SkobLee8:    return "Sintax";
		case MbcType::LiCheng:     return "Li Cheng";
		case MbcType::Vf001:       return "Vast Fame";
		case MbcType::MMM01:       return "MMM01";
		case MbcType::M161:        return "M161";
		case MbcType::MBC6:        return "MBC6";
		case MbcType::MBC7:        return "MBC7";
		case MbcType::HuC1:        return "HuC1";
		case MbcType::HuC3:        return "HuC3";
		case MbcType::TAMA5:       return "TAMA5";
		case MbcType::Camera:      return "Camera";
		default: break;
	}
	if (c.mbc1_multicart)  return "MBC1M";
	if (c.duz_multicart)   return "Duz 2-in-1";
	if (c.mbc5_multicart)  return "MBC5 multi";
	if (c.flat_unlicensed) return "Unl MBC1";
	return "Official";
}

constexpr uint8_t kNintendoLogo48[48] = {
	0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
	0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
	0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
	0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E };

// Sub-game headers (logo + valid header checksum) at 32 KiB boundaries -
// two or more marks a multicart family no structural probe knows yet (the
// 36-in-1 hides its first sub-game at $120000, out of every probe's sight).
int CountSubHeaders(const std::vector<uint8_t> &img)
{
	int n = 0;
	for (size_t b = 0x8000; b + 0x150 <= img.size(); b += 0x8000)
	{
		bool logo = true;
		for (int i = 0; i < 48 && logo; ++i)
			if (img[b + 0x104 + i] != kNintendoLogo48[i]) logo = false;
		if (!logo) continue;
		uint8_t sum = 0;
		for (size_t i = b + 0x134; i <= b + 0x14C; ++i)
			sum = static_cast<uint8_t>(sum - img[i] - 1);
		if (sum != img[b + 0x14D]) continue;
		// Mirror padding repeats the cart's own header (Yars' Revenge is
		// 16 copies of itself); only a DIFFERENT title marks a sub-game.
		if (std::memcmp(&img[b + 0x134], &img[0x134], 11) == 0) continue;
		++n;
	}
	return n;
}

// LotCheck-grade forensics. The boot ROM only enforces the logo and header
// checksum, which every bootleg forges; official releases also pass the
// publishing checks Nintendo's LotCheck ran before manufacture.
bool LotCheckClean(const std::vector<uint8_t> &img)
{
	if (img.size() < 0x150) return false;

	// Global checksum: every ROM byte except its own two.
	uint16_t sum = 0;
	for (size_t i = 0; i < img.size(); ++i)
		if (i != 0x14E && i != 0x14F) sum = static_cast<uint16_t>(sum + img[i]);
	if (sum != ((img[0x14E] << 8) | img[0x14F])) return false;

	// ROM-size byte must match the image; RAM-size byte must be a
	// documented code, and $00 on an MBC2 (its 512x4 RAM is internal).
	const uint8_t sz = img[0x148];
	const size_t expect = sz <= 8    ? (size_t)0x8000 << sz
	                    : sz == 0x52 ? 0x120000
	                    : sz == 0x53 ? 0x140000
	                    : sz == 0x54 ? 0x180000 : 0;
	if (expect != img.size()) return false;
	if (img[0x149] > 5) return false;
	if ((img[0x147] == 0x05 || img[0x147] == 0x06) && img[0x149] != 0)
		return false;

	// CGB flag is $80/$C0 or a plain title byte - never $80 with low bits.
	const uint8_t cgb = img[0x143];
	const bool cgb_hdr = (cgb & 0x80) != 0;
	if (cgb_hdr && cgb != 0x80 && cgb != 0xC0) return false;

	// GBC and SGB releases must use the new-licensee scheme: $014B = $33
	// with a two-character alphanumeric publisher ID at $0144.
	if ((cgb_hdr || img[0x146] == 0x03) && img[0x14B] != 0x33) return false;
	auto alnum = [](uint8_t ch) {
		return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
	};
	if (img[0x14B] == 0x33 && (!alnum(img[0x144]) || !alnum(img[0x145])))
		return false;

	// Title: text with $00 padding and no text after it. On CGB carts the
	// 4-char manufacturer code at $013F may follow the padding - but early
	// CGB releases still run plain titles to 15 chars (POKEMON YELLOW), so
	// the 11-char truncation can't be a hard rule. Extended bytes pass:
	// Gluecksrad ships "GL\x9ACKSRAD", a codepage-850 umlaut.
	const size_t tend = cgb_hdr ? 0x143 : 0x144;
	bool padding = false;
	for (size_t i = 0x134; i < tend; ++i)
	{
		const uint8_t ch = img[i];
		if (ch == 0) { padding = true; continue; }
		if (ch < 0x20) return false;
		if (padding)
		{
			// Text resuming after padding is only the manufacturer code.
			if (!cgb_hdr || i < 0x13F) return false;
			for (size_t j = 0x13F; j < 0x143; ++j)
				if (img[j] < 0x20 || img[j] > 0x7E) return false;
			break;
		}
	}
	return true;
}

// Full-image classification - the unlicensed families and multicart wirings
// only show up to the core's structural detection, never the header byte.
std::string ClassifyCartType(const std::string &path)
{
	std::vector<uint8_t> img;
	bool gbc = false;
	if (!LoadGBImage(path, img, gbc) || img.empty()) return "Unknown";
	SGB::Cart cart;
	if (!SGB::CartLoad(cart, img.data(), img.size(), nullptr)) return "Unknown";
	const std::string bucket = CartTypeBucket(cart);
	// A cart no probe recognizes can still betray itself: hidden sub-game
	// headers, or hardware-valid header fields that fail LotCheck.
	if (bucket == "Official")
	{
		if (CountSubHeaders(img) >= 2) return "Multicart";
		if (!LotCheckClean(img))       return "Bootleg";
	}
	return bucket;
}

// Cart-type cache: audit/cart_types.txt, "<size>\t<type>\t<key>" lines under
// a version header. Bump kCartTypeCacheVer whenever detection changes.
constexpr int kCartTypeCacheVer = 4;

std::map<std::string, std::string> LoadCartTypeCache(const std::string &file)
{
	std::map<std::string, std::string> m;
	FILE *f = fopen(file.c_str(), "rb");
	if (!f) return m;
	char line[1024];
	bool ver_ok = false;
	while (fgets(line, sizeof line, f))
	{
		if (char *nl = strpbrk(line, "\r\n")) *nl = 0;
		if (!ver_ok)
		{
			ver_ok = atoi(line) == kCartTypeCacheVer;
			if (!ver_ok) break;
			continue;
		}
		char *type = strchr(line, '\t');
		if (!type) continue;
		*type++ = 0;
		char *key = strchr(type, '\t');
		if (!key) continue;
		*key++ = 0;
		m[std::string(key) + "|" + line] = type;
	}
	fclose(f);
	return m;
}

void SaveCartTypeCache(const std::string &file,
                       const std::map<std::string, std::string> &m)
{
	FILE *f = fopen(file.c_str(), "wb");
	if (!f) return;
	fprintf(f, "%d\n", kCartTypeCacheVer);
	for (const auto &kv : m)
	{
		const size_t bar = kv.first.rfind('|');
		if (bar == std::string::npos) continue;
		fprintf(f, "%s\t%s\t%s\n", kv.first.c_str() + bar + 1,
		        kv.second.c_str(), kv.first.substr(0, bar).c_str());
	}
	fclose(f);
}

std::vector<Rom> ScanRoms(const char *roms_dir, const char *audit_dir,
                          ScanProgressFn progress, void *user)
{
	std::vector<Rom> out;
	const std::string root = roms_dir ? roms_dir : "Roms";

	// Phase 1: the candidate list, so progress has a denominator. Anything
	// with an SNES extension never gets opened.
	struct Cand { std::string dir, file, folder; };
	std::vector<Cand> cands;
	auto collect = [&](const std::string &dir, const std::string &folder) {
		std::vector<std::string> files, dirs;
		ListDir(dir, files, dirs);
		for (const std::string &f : files)
			if (EndsWithFold(f, ".gb") || EndsWithFold(f, ".gbc") ||
			    EndsWithFold(f, ".zip"))
				cands.push_back({ dir, f, folder });
	};
	std::vector<std::string> files, dirs;
	ListDir(root, files, dirs);
	collect(root, "");
	for (const std::string &d : dirs)
		collect(Join(root, d), d);

	// Cart-type cache - classification reads whole images, so remember.
	const std::string cache_file =
		Join(audit_dir ? audit_dir : "audit", "cart_types.txt");
	std::map<std::string, std::string> type_cache = LoadCartTypeCache(cache_file);
	std::mutex cache_mtx;
	bool cache_dirty = false;

	// Phase 2: classify, spread over the cores - each header probe is an
	// independent file open. Slots keep the output in list order, and the
	// calling thread owns progress and the abort decision.
	std::vector<Rom>  slots(cands.size());
	std::vector<char> keep(cands.size(), 0);
	std::atomic<size_t> next{0};
	std::atomic<size_t> done{0};
	std::atomic<bool>   abort{false};
	unsigned hw = std::thread::hardware_concurrency();
	if (hw == 0) hw = 1;
	if (hw > 16) hw = 16;
	if (hw > cands.size()) hw = (unsigned)(cands.size() ? cands.size() : 1);

	auto worker = [&]() {
		for (;;)
		{
			const size_t i = next.fetch_add(1);
			if (i >= cands.size() || abort.load(std::memory_order_relaxed))
				break;
			const Cand &cd = cands[i];
			Rom r;
			r.path   = Join(cd.dir, cd.file);
			r.name   = Stem(cd.file);
			r.folder = cd.folder;
			r.key    = Sanitize(cd.folder.empty() ? r.name
			                                      : cd.folder + "/" + r.name);
			std::vector<uint8_t> hdr;
			uint32_t full = 0;
			bool gbc = false;
			if (ProbeGBHeader(r.path, hdr, full, gbc))
			{
				r.gbc_file = gbc;
				r.cgb_flag = hdr[0x143];
				r.sgb_flag = hdr[0x146];
				r.size     = full;
				// Cart type from the cache, else the full-image detection.
				char ck[32];
				snprintf(ck, sizeof ck, "|%u", full);
				const std::string cache_key = r.key + ck;
				{
					std::lock_guard<std::mutex> lk(cache_mtx);
					auto it = type_cache.find(cache_key);
					if (it != type_cache.end()) r.cart_type = it->second;
				}
				if (r.cart_type.empty())
				{
					r.cart_type = ClassifyCartType(r.path);
					std::lock_guard<std::mutex> lk(cache_mtx);
					type_cache[cache_key] = r.cart_type;
					cache_dirty = true;
				}
				slots[i] = std::move(r);
				keep[i]  = 1;
			}
			done.fetch_add(1);
		}
	};
	std::vector<std::thread> pool;
	pool.reserve(hw);
	for (unsigned t = 0; t < hw; ++t) pool.emplace_back(worker);
	while (done.load() < cands.size() && !abort.load())
	{
		if (progress && !progress(user, (int)done.load(), (int)cands.size()))
			abort.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}
	for (auto &th : pool) th.join();

	for (size_t i = 0; i < cands.size(); ++i)
		if (keep[i]) out.push_back(std::move(slots[i]));
	if (cache_dirty) SaveCartTypeCache(cache_file, type_cache);
	if (progress) progress(user, (int)cands.size(), (int)cands.size());
	return out;
}

std::map<std::string, std::string> LoadLinkMeta(const char *audit_dir)
{
	std::map<std::string, std::string> out;
	FILE *f = fopen(Join(audit_dir ? audit_dir : "audit", "linkmeta.txt").c_str(), "rb");
	if (!f) return out;
	char line[1024];
	while (fgets(line, sizeof line, f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty() || s[0] == '#') continue;
		const size_t bar = s.find('|');
		if (bar == std::string::npos) continue;
		out[s.substr(0, bar)] = s.substr(bar + 1);
	}
	fclose(f);
	return out;
}

std::string LinkLabelFor(const std::map<std::string, std::string> &meta,
                         const Rom &r)
{
	std::string key = r.key;
	for (char &c : key) c = Lower(c);
	for (const auto &kv : meta)
	{
		std::string pat = kv.first;
		for (char &c : pat) c = Lower(c);
		if (key.find(pat) != std::string::npos) return kv.second;
	}
	return std::string();
}

/*--------------------------------------------------------------------------
  Baseline IO
--------------------------------------------------------------------------*/

static std::string EntryKey(const std::string &rom_key, Combo c)
{
	return rom_key + "|" + ComboId(c) + "|" + ComboShotId(c);
}

const BaselineEntry *Baseline::Find(const Rom &r, Combo c) const
{
	const auto it = entries.find(EntryKey(r.key, c));
	return it == entries.end() ? nullptr : &it->second;
}

bool LoadBaseline(const char *dir, Baseline &out, std::string &err)
{
	out = Baseline();
	out.dir = dir ? dir : "";
	const size_t slash = out.dir.find_last_of("/\\");
	out.name = slash == std::string::npos ? out.dir : out.dir.substr(slash + 1);

	FILE *f = fopen(Join(out.dir, "index.txt").c_str(), "rb");
	if (!f)
	{
		err = "no index.txt in " + out.dir;
		return false;
	}
	char line[2048];
	while (fgets(line, sizeof line, f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty()) continue;
		if (s[0] == '#')
		{
			if (s.compare(0, 8, "# title=") == 0)   out.title   = s.substr(8);
			if (s.compare(0, 10, "# created=") == 0) out.created = s.substr(10);
			continue;
		}
		// rom_key|combo|shot|frame|file
		std::vector<std::string> parts;
		size_t start = 0;
		for (size_t i = 0; i <= s.size(); ++i)
			if (i == s.size() || s[i] == '|')
			{
				parts.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		if (parts.size() < 5) continue;
		BaselineEntry e;
		e.frame = atoi(parts[3].c_str());
		e.image = parts[4];
		out.entries[parts[0] + "|" + parts[1] + "|" + parts[2]] = std::move(e);
	}
	fclose(f);
	return true;
}

std::vector<Baseline> DiscoverBaselines(const char *audit_dir)
{
	std::vector<Baseline> out;
	const std::string root = Join(audit_dir ? audit_dir : "audit", kBaselineDir);
	std::vector<std::string> files, dirs;
	ListDir(root, files, dirs);
	// default first, like the acid columns.
	std::stable_sort(dirs.begin(), dirs.end(), [](const std::string &a,
	                                              const std::string &b) {
		return (a == kDefaultBaseline) > (b == kDefaultBaseline);
	});
	for (const std::string &d : dirs)
	{
		Baseline b;
		std::string err;
		if (LoadBaseline(Join(root, d).c_str(), b, err) && !b.Empty())
			out.push_back(std::move(b));
	}
	return out;
}

bool LoadBaselineShot(const Baseline &b, const BaselineEntry &e,
                      std::vector<uint8_t> &rgb, int &w, int &h)
{
	int comp = 0;
	unsigned char *px = stbi_load(Join(b.dir, e.image).c_str(), &w, &h, &comp, 3);
	if (!px) return false;
	rgb.assign(px, px + (size_t)w * h * 3);
	stbi_image_free(px);
	return true;
}

bool LoadCapturePng(const Baseline &b, const BaselineEntry &e, Capture &out)
{
	out = Capture();
	if (!ReadWholeFile(Join(b.dir, e.image), out.png)) return false;
	// PNG IHDR: width and height big-endian at offsets 16 and 20.
	if (out.png.size() < 24 ||
	    std::memcmp(out.png.data(), "\x89PNG\r\n\x1a\n", 8) != 0)
	{
		out.png.clear();
		return false;
	}
	const uint8_t *p = out.png.data();
	out.w = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
	out.h = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
	out.frame = e.frame;
	return true;
}

// A whole-library save is ~65k PNGs - far too many for one folder on
// Windows - so shots shard into a-z subfolders by the ROM name's first
// letter ("0" for digits, "_" for anything else).
static std::string ShardDir(const Rom &r)
{
	const char c = r.name.empty() ? 0 : r.name[0];
	if (c >= 'a' && c <= 'z') return std::string(1, c);
	if (c >= 'A' && c <= 'Z') return std::string(1, static_cast<char>(c - 'A' + 'a'));
	if (c >= '0' && c <= '9') return "0";
	return "_";
}

int WriteBaseline(const char *dir, const std::vector<Rom> &roms,
                  const std::vector<RomResult> &results,
                  const char *title, std::string &err)
{
	const std::string root = dir ? dir : "";
	if (root.empty()) { err = "no folder"; return -1; }
	// Create the folder and any missing parents.
	for (size_t i = 1; i <= root.size(); ++i)
	{
		if (i < root.size() && root[i] != '/' && root[i] != '\\') continue;
		const std::string part = root.substr(0, i);
		if (part.size() <= 1 || part.back() == ':') continue;
#ifdef _WIN32
		_mkdir(part.c_str());
#else
		mkdir(part.c_str(), 0755);
#endif
	}

	// Merge with whatever the folder already holds, so a baseline can be
	// built in chunks: save folder GB, then folder GBC into the same place.
	Baseline existing;
	{
		std::string ignore;
		LoadBaseline(root.c_str(), existing, ignore);
	}

	std::string index = "# audit baseline\n";
	if (title && *title) index += std::string("# title=") + title + "\n";
	else if (!existing.title.empty())
		index += "# title=" + existing.title + "\n";
	{
		char stamp[64];
		const time_t now = time(nullptr);
		strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", localtime(&now));
		index += std::string("# created=") + stamp + "\n";
	}

	int written = 0;
	std::map<std::string, char> shard_made;
	for (size_t i = 0; i < roms.size() && i < results.size(); ++i)
	{
		for (int c = 0; c < kComboCount; ++c)
		{
			const ComboResult &cr = results[i].combos[c];
			if (!cr.ran || cr.shot.Empty()) continue;
			const Capture &cap = cr.shot;
			const std::string shard = ShardDir(roms[i]);
			if (!shard_made.count(shard))
			{
#ifdef _WIN32
				_mkdir(Join(root, shard).c_str());
#else
				mkdir(Join(root, shard).c_str(), 0755);
#endif
				shard_made[shard] = 1;
			}
			const std::string file = shard + "/" + Sanitize(roms[i].key) +
				"." + ComboId((Combo)c) + "." + ComboShotId((Combo)c) + ".png";
			// Captures are held as PNG already - straight to disk.
			const std::vector<uint8_t> &png = cap.png;
			FILE *f = fopen(Join(root, file).c_str(), "wb");
			if (!f) { err = "cannot write " + file; return -1; }
			const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
			fclose(f);
			if (!ok) { err = "short write on " + file; return -1; }
			// A pre-shard save of this shot kept it in the folder root;
			// drop that copy so the folder migrates as it is re-saved.
			const auto old = existing.entries.find(EntryKey(roms[i].key, (Combo)c));
			if (old != existing.entries.end() && old->second.image != file)
				remove(Join(root, old->second.image).c_str());
			char line[1024];
			snprintf(line, sizeof line, "%s|%s|%s|%d|%s\n",
			         roms[i].key.c_str(), ComboId((Combo)c),
			         ComboShotId((Combo)c), cap.frame, file.c_str());
			index += line;
			++written;
		}
	}

	// Entries this save did not touch keep their line (and their PNGs).
	{
		std::map<std::string, char> ours;
		for (size_t i = 0; i < roms.size() && i < results.size(); ++i)
			for (int c = 0; c < kComboCount; ++c)
				if (results[i].combos[c].ran)
					ours[roms[i].key + "|" + ComboId((Combo)c)] = 1;
		for (const auto &kv : existing.entries)
		{
			// key is "rom|combo|shot"; drop the shot for the ownership test.
			const size_t bar = kv.first.rfind('|');
			if (bar == std::string::npos) continue;
			if (ours.count(kv.first.substr(0, bar))) continue;
			char line[1024];
			snprintf(line, sizeof line, "%s|%d|%s\n",
			         kv.first.c_str(), kv.second.frame,
			         kv.second.image.c_str());
			index += line;
			++written;
		}
	}

	FILE *f = fopen(Join(root, "index.txt").c_str(), "wb");
	if (!f) { err = "cannot write index.txt"; return -1; }
	fwrite(index.data(), 1, index.size(), f);
	fclose(f);
	return written;
}

/*--------------------------------------------------------------------------
  Running one combo
--------------------------------------------------------------------------*/

namespace {

// Configure a private core the way the app's load path would for this
// combo, stage the boot ROM when one applies, and load the cart.
bool ConfigureAndLoad(SGB::Emulator &emu, Combo c, const std::vector<uint8_t> &rom,
                      const std::vector<uint8_t> *dmg_boot,
                      const std::vector<uint8_t> *cgb_boot, std::string &err)
{
	emu.SetForceModel(0);
	emu.SetClockMultiplier(1.0f);
	switch (c)
	{
		case Combo::GB:
		case Combo::GB_Bios:
			emu.SetSgbCgbHack(false);
			emu.SetCgbOverride(0);
			emu.SetRunMode(SGB::RunMode::DMG);
			break;
		case Combo::GBC:
		case Combo::GBC_Bios:
			emu.SetSgbCgbHack(false);
			emu.SetCgbOverride(1);
			emu.SetRunMode(SGB::RunMode::DMG);
			break;
		case Combo::SGB1:
		case Combo::SGB2:
			emu.SetSgbCgbHack(false);
			emu.SetCgbOverride(-1);
			emu.SetRunMode(c == Combo::SGB1 ? SGB::RunMode::SGB
			                                : SGB::RunMode::SGB2);
			break;
		case Combo::SGB1_CB:
		case Combo::SGB2_CB:
			// Authentic SGB: force model 3 keeps command processing and the
			// idle 2-player ID rotation live, so carts detect the SGB and
			// upload their custom borders.
			emu.SetSgbCgbHack(false);
			emu.SetCgbOverride(-1);
			emu.SetForceModel(3);
			emu.SetRunMode(c == Combo::SGB1_CB ? SGB::RunMode::SGB
			                                   : SGB::RunMode::SGB2);
			break;
		default:
			err = "bad combo";
			return false;
	}

	const std::vector<uint8_t> *boot = nullptr;
	if (c == Combo::GB_Bios)  boot = dmg_boot;
	if (c == Combo::GBC_Bios) boot = cgb_boot;
	bool boot_ok;
	if (c == Combo::SGB1_CB || c == Combo::SGB2_CB)
	{
		// Authentic SGB boots through the embedded SGB boot ROM so the
		// cart sees the real handoff signature and engages its SGB path.
		boot_ok = emu.LoadBootROM(
			SGB::SgbEmbeddedBootRom(c == Combo::SGB2_CB ? 2 : 1), 256);
	}
	else
	{
		boot_ok = emu.LoadBootROM(boot && !boot->empty() ? boot->data() : nullptr,
		                          boot ? boot->size() : 0);
	}
	if (!boot_ok)
	{
		err = "boot ROM rejected";
		return false;
	}
	if (!emu.LoadROM(rom.data(), rom.size(), nullptr))
	{
		err = "core rejected ROM";
		return false;
	}
	return true;
}

// Auto-pick capture points: the title shot waits, from 90 frames after the
// panel first shows anything, for the picture to hold still - never
// mid-animation (logo wipes etc.). Mid-game is 1500 frames after the title,
// and the border (SGB modes) is read at the mid point, past any transfer.
constexpr int kFirstActiveCap = 900;
constexpr int kTitleSettle    = 90;    // earliest title shot after activity
constexpr int kTitleStable    = 3;     // frames the picture must hold still
constexpr int kTitleWaitCap   = 240;   // perpetual animation: shoot here anyway

// The custom-border columns: authentic SGB through the embedded SGB boot
// ROM, waiting for the cart's own border upload.
// The shot is taken on EVENTS, not a fixed frame: nothing captures until a
// PCT_TRN has actually landed (carts blank the border first, chat palettes
// for a while, or upload late), then the border plane must hold still for
// kBorderHold frames - a staged build or a second border restarts the
// clock. Command silence is only a tiebreaker for a clean panel; a cart
// that chatters forever is shot once the plane alone has held kBorderChatty.
// A settled ring that is one flat color is a BLANKING upload (Arcade
// Classics), not the border - keep waiting. A cart that never shows one
// documents whatever the ring holds at the cap.
constexpr int kBorderWaitCap = 3600;
constexpr int kBorderHold    = 120;
constexpr int kBorderChatty  = 600;

// True when every border-ring pixel of BlitScreen is (near) one color.
bool BorderRingUniform(SGB::Emulator &emu)
{
	static thread_local std::vector<uint16_t> px;
	px.assign(kSgbW * kSgbH, 0);
	emu.BlitScreen(px.data(), kSgbW);
	uint32_t r0 = 0, g0 = 0, b0 = 0;
	DECOMPOSE_PIXEL(px[0], r0, g0, b0);
	for (int y = 0; y < kSgbH; ++y)
		for (int x = 0; x < kSgbW; ++x)
		{
			if (x >= 48 && x < 208 && y >= 40 && y < 184) continue;
			uint32_t r, g, b;
			DECOMPOSE_PIXEL(px[y * kSgbW + x], r, g, b);
			if ((r > r0 ? r - r0 : r0 - r) > 1 ||
			    (g > g0 ? g - g0 : g0 - g) > 1 ||
			    (b > b0 ? b - b0 : b0 - b) > 1)
				return false;
		}
	return true;
}

void RunBorderCombo(SGB::Emulator &emu, const Rom &r, Combo c,
                    ComboResult &out, const std::vector<uint8_t> &rom,
                    const Baseline *base,
                    const std::atomic<bool> &cancel,
                    const std::atomic<bool> *pause,
                    const std::function<void(int, int)> &tick)
{
	out = ComboResult();

	std::string err;
	if (!ConfigureAndLoad(emu, c, rom, nullptr, nullptr, err))
	{
		out.error = err;
		out.verdict.m = Match::Error;
		return;
	}
	out.ran = true;

	const BaselineEntry *be = base ? base->Find(r, c) : nullptr;
	const int pin = be ? be->frame : -1;

	int frame = 0, last_cmd = 0, last_plane = 0;
	uint32_t seen_cmd = 0, seen_plane = 0;
	for (;;)
	{
		emu.RunFrame();
		++frame;
		if (pin >= 0)
		{
			if (frame >= pin) break;
		}
		else
		{
			const uint32_t nc = emu.BorderTransferCount();
			const uint32_t np = emu.BorderPlaneVersion();
			if (nc != seen_cmd)   { seen_cmd = nc;   last_cmd = frame; }
			if (np != seen_plane) { seen_plane = np; last_plane = frame; }
			// The (frame & 15) throttle keeps the blit off the hot loop
			// while a blanked ring waits out its real border.
			if (emu.BorderPctCount() > 0 &&
			    frame >= last_plane + kBorderHold &&
			    (frame >= last_cmd + kBorderHold ||
			     frame >= last_plane + kBorderChatty) &&
			    (frame & 15) == 0 &&
			    !BorderRingUniform(emu))
				break;
			if (frame >= kBorderWaitCap) break;
		}
		if ((frame & 63) == 63)
		{
			if (cancel.load(std::memory_order_relaxed)) return;
			tick(frame, pin >= 0 ? pin : kBorderWaitCap);
			while (pause && pause->load(std::memory_order_relaxed) &&
			       !cancel.load(std::memory_order_relaxed))
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	CaptureSgbRgb(emu, out.shot, frame);

	ShotVerdict &v = out.verdict;
	if (!base) return;
	if (!be) { v.m = Match::NoBase; return; }
	std::vector<uint8_t> ref;
	int rw = 0, rh = 0;
	if (!LoadBaselineShot(*base, *be, ref, rw, rh)) { v.m = Match::Error; return; }
	std::vector<uint8_t> ours;
	if (!out.shot.Decode(ours) || rw != out.shot.w || rh != out.shot.h)
	{ v.m = Match::Error; return; }
	v.diff_px = DiffRgb(ours, ref);
	v.m = (v.diff_px == 0) ? Match::Same : Match::Differs;
}

// One column of one ROM, on one private core: run to the column's event -
// the settled boot logo or the settled title - take THE shot, compare.
void RunCombo(SGB::Emulator &emu, const Rom &r, Combo c, ComboResult &out,
              const std::vector<uint8_t> &rom,
              const std::vector<uint8_t> *dmg_boot,
              const std::vector<uint8_t> *cgb_boot,
              const Baseline *base, int slip_window,
              const std::atomic<bool> &cancel, const std::atomic<bool> *pause,
              const std::function<void(int, int)> &tick)
{
	out = ComboResult();

	if (!ComboApplies(c, r))
	{
		out.verdict.m = Match::Skip;
		return;
	}
	if (ComboNeedsBoot(c))
	{
		const std::vector<uint8_t> *boot =
			c == Combo::GB_Bios ? dmg_boot : cgb_boot;
		if (!boot || boot->empty())
		{
			out.verdict.m = Match::Skip;
			out.error = "no boot ROM in audit/";
			return;
		}
	}

	if (c == Combo::SGB1_CB || c == Combo::SGB2_CB)
	{
		RunBorderCombo(emu, r, c, out, rom, base, cancel, pause, tick);
		return;
	}

	std::string err;
	if (!ConfigureAndLoad(emu, c, rom, dmg_boot, cgb_boot, err))
	{
		out.error = err;
		out.verdict.m = Match::Error;
		return;
	}
	out.ran = true;

	const bool ring = ComboIsRing(c);   // SGB1/SGB2: 256x224 bezel composite

	// The baseline pins the frame when it has this column, else the shot
	// lands at the settle event and that frame becomes the pin.
	const BaselineEntry *be = base ? base->Find(r, c) : nullptr;
	int pin = be ? be->frame : -1;
	const bool pinned = pin >= 0;

	// Slip history: panel grayscale of the recent frames.
	std::vector<std::vector<uint8_t>> history;
	std::vector<int> history_frame;
	const int keep = slip_window > 0 ? slip_window : 1;

	// SGB hands the cart over with no DMG boot, so its logo APPEARING on
	// the panel is a handoff regression. The handoff itself leaves the logo
	// in VRAM (games expect that), so a match running from frame 1 until
	// the game clears the screen is legitimate residue, not a boot.
	const std::vector<uint8_t> logo_mask =
		ring ? DecodeHeaderLogo(rom) : std::vector<uint8_t>();
	int  logo_frame   = -1;
	bool logo_residue = true;

	std::vector<uint8_t> gray, prev_gray;
	int stable = 0;
	int first_active = -1;
	int frame = 0;
	int last_needed = pinned ? pin + (slip_window > 0 ? slip_window : 0)
	                         : kFirstActiveCap + kTitleSettle + kTitleWaitCap;

	while (frame < last_needed)
	{
		emu.RunFrame();
		++frame;

		PanelGray(emu, gray);
		if (first_active < 0 && !GrayUniform(gray)) first_active = frame;
		if (!logo_mask.empty())
		{
			const bool logo_now = GrayShowsBootLogo(gray, logo_mask);
			if (logo_now && !logo_residue && logo_frame < 0)
				logo_frame = frame;
			if (!logo_now) logo_residue = false;
		}

		if (!pinned && pin < 0)
		{
			stable = (!prev_gray.empty() && gray == prev_gray)
			         ? stable + 1 : 0;
			prev_gray = gray;
			if (first_active >= 0)
			{
				// The first frame past the settle delay where the picture
				// has held still - a logo wipe caught mid-flight never
				// becomes the shot; perpetual motion is shot at the cap.
				const int earliest = first_active + kTitleSettle;
				if (frame >= earliest &&
				    (stable >= kTitleStable || frame >= earliest + kTitleWaitCap))
				{
					pin = frame;
					last_needed = frame;
				}
			}
			else if (frame >= kFirstActiveCap)
			{
				// Never drew a thing: shoot here so the capture exists and
				// is deterministic, and say so.
				pin = frame;
				last_needed = frame;
				out.error = "no picture by frame " + std::to_string(frame);
			}
		}

		if (frame == pin)
		{
			if (ring) CaptureSgbRgb(emu, out.shot, frame);
			else      CapturePanelRgb(emu, out.shot, frame);
		}

		// Only kept when a slipped picture may need to be found later.
		if (pinned && slip_window > 0)
		{
			history.push_back(gray);
			history_frame.push_back(frame);
			if ((int)history.size() > 2 * keep + 1)
			{
				history.erase(history.begin());
				history_frame.erase(history_frame.begin());
			}
		}

		if ((frame & 63) == 63)
		{
			if (cancel.load(std::memory_order_relaxed)) return;
			tick(frame, last_needed);
			while (pause && pause->load(std::memory_order_relaxed) &&
			       !cancel.load(std::memory_order_relaxed))
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	// The Nintendo boot logo in an SGB mode fails the column outright: the
	// game must start straight after the splash, never through the scroll.
	if (logo_frame >= 0)
	{
		out.error = "Nintendo boot logo on panel at frame " +
		            std::to_string(logo_frame);
		out.verdict.m = Match::Error;
		return;
	}

	// Verdict against the baseline.
	ShotVerdict &v = out.verdict;
	if (!base) return;
	if (!be) { v.m = Match::NoBase; return; }

	std::vector<uint8_t> ref;
	int rw = 0, rh = 0;
	if (!LoadBaselineShot(*base, *be, ref, rw, rh)) { v.m = Match::Error; return; }
	std::vector<uint8_t> ours;
	if (!out.shot.Decode(ours) || rw != out.shot.w || rh != out.shot.h)
	{ v.m = Match::Error; return; }
	v.diff_px = DiffRgb(ours, ref);
	if (v.diff_px == 0) { v.m = Match::Same; return; }

	// The picture may simply be late or early: look for it in the panel
	// history around the pinned frame. Ring shots do not slip per frame.
	v.m = Match::Differs;
	if (ring || slip_window <= 0) return;
	std::vector<uint8_t> ref_gray;
	RgbToGray(ref, ref_gray);
	int best = -1, best_dist = 0;
	for (size_t k = 0; k < history.size(); ++k)
	{
		if (history_frame[k] == out.shot.frame) continue;
		if (DiffGray(history[k], ref_gray) != 0) continue;
		const int dist = history_frame[k] - out.shot.frame;
		const int a = dist < 0 ? -dist : dist;
		if (best < 0 || a < (best_dist < 0 ? -best_dist : best_dist))
		{
			best = (int)k;
			best_dist = dist;
		}
	}
	if (best >= 0)
	{
		v.m    = Match::Slip;
		v.slip = best_dist;
	}
}

} // anonymous

/*--------------------------------------------------------------------------
  The run
--------------------------------------------------------------------------*/

int DefaultThreadCount()
{
	const unsigned hw = std::thread::hardware_concurrency();
	return hw ? (int)hw : 1;
}

Summary RunAudit(const std::vector<Rom> &roms, const RunOptions &opts,
                 std::vector<RomResult> *out_results)
{
	Summary sum;
	if (roms.empty()) return sum;
	const std::string dir = opts.audit_dir ? opts.audit_dir : "audit";

	// Boot ROMs the pack pins for the +BIOS combos, read once and shared;
	// missing files just turn those combos into SKIP.
	std::vector<uint8_t> dmg_boot, cgb_boot;
	ReadWholeFile(Join(dir, "dmg_boot.bin"), dmg_boot);
	ReadWholeFile(Join(dir, "cgb_boot.bin"), cgb_boot);

	int nthreads = opts.threads > 0 ? opts.threads : DefaultThreadCount();
	if (nthreads > (int)roms.size()) nthreads = (int)roms.size();
	if (nthreads < 1) nthreads = 1;
	// With live control the pool is built at full size; workers beyond the
	// live count park between ROMs, so the dial moves both ways mid-run.
	int pool_n = nthreads;
	if (opts.threads_live)
	{
		pool_n = DefaultThreadCount();
		if (pool_n > (int)roms.size()) pool_n = (int)roms.size();
		if (pool_n < nthreads) pool_n = nthreads;
	}

	std::vector<RomResult> results(roms.size());
	std::vector<char>      ran(roms.size(), 0);
	std::atomic<size_t> next{0};
	std::atomic<bool>   cancel{false};
	std::atomic<int>    live{pool_n};
	std::mutex          mu;
	struct Event
	{
		size_t index; int kind; int combo; int done; int total;
		ComboResult cell;   // EvCell only
	};
	enum { EvStart, EvRunning, EvCell, EvFinished };
	std::vector<Event> events;

	auto worker = [&](int id) {
		SGB::Emulator emu;
		SGB::ScopedActiveEmulator bind(emu);
		if (emu.Init())
		{
			for (;;)
			{
				if (cancel.load() || next.load() >= roms.size()) break;
				// Parked: over the live count. Work may still open up when
				// the dial goes back the other way.
				if (opts.threads_live &&
				    id >= opts.threads_live->load(std::memory_order_relaxed))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
				const size_t i = next.fetch_add(1);
				if (i >= roms.size() || cancel.load()) break;
				{
					std::lock_guard<std::mutex> lk(mu);
					events.push_back({ i, EvStart, 0, 0, 0, ComboResult() });
				}
				std::vector<uint8_t> rom;
				bool gbc = false;
				RomResult rr;
				if (!LoadGBImage(roms[i].path, rom, gbc) || rom.size() < 0x150)
				{
					for (int c = 0; c < kComboCount; ++c)
					{
						rr.combos[c].error = "cannot read ROM";
						rr.combos[c].verdict.m = Match::Error;
					}
				}
				else
				{
					for (int c = 0; c < kComboCount && !cancel.load(); ++c)
					{
						auto tick = [&](int done, int total) {
							std::lock_guard<std::mutex> lk(mu);
							events.push_back({ i, EvRunning, c, done, total,
							                   ComboResult() });
						};
						RunCombo(emu, roms[i], (Combo)c, rr.combos[c], rom,
						         &dmg_boot, &cgb_boot, opts.baseline,
						         opts.slip_window, cancel, opts.pause, tick);
						std::lock_guard<std::mutex> lk(mu);
						events.push_back({ i, EvCell, c, 0, 0, rr.combos[c] });
					}
				}
				if (cancel.load()) break;
				std::lock_guard<std::mutex> lk(mu);
				results[i] = std::move(rr);
				ran[i] = 1;
				events.push_back({ i, EvFinished, 0, 0, 0, ComboResult() });
			}
		}
		live.fetch_sub(1);
	};

	std::vector<std::thread> pool;
	pool.reserve(pool_n);
	for (int i = 0; i < pool_n; ++i) pool.emplace_back(worker, i);

	size_t reported = 0;
	for (;;)
	{
		std::vector<Event> batch;
		{
			std::lock_guard<std::mutex> lk(mu);
			batch.swap(events);
		}
		for (Event &ev : batch)
		{
			switch (ev.kind)
			{
				case EvStart:
					if (opts.on_start) opts.on_start(opts.user, (int)ev.index);
					break;
				case EvRunning:
					break;   // reserved; per-cell RUN state covers it
				case EvCell:
					if (opts.on_cell)
						opts.on_cell(opts.user, (int)ev.index, (Combo)ev.combo,
						             ev.cell);
					break;
				case EvFinished:
				{
					const RomResult &r = results[ev.index];
					bool err = false;
					for (int c = 0; c < kComboCount; ++c)
						if (r.combos[c].Cell() == Match::Error) err = true;
					if (err)                              ++sum.errors;
					else if (r.AllPassed(roms[ev.index])) ++sum.passed;
					else                                  ++sum.failed;
					++sum.total;
					++reported;
					if (opts.on_result)
						opts.on_result(opts.user, (int)ev.index, r);
					break;
				}
			}
		}
		if (opts.progress &&
		    !opts.progress(opts.user, (int)reported, (int)roms.size()))
			cancel.store(true);
		if (live.load() == 0)
		{
			std::lock_guard<std::mutex> lk(mu);
			if (events.empty()) break;
			continue;
		}
		if (batch.empty())
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	for (auto &th : pool) th.join();
	sum.cancelled = cancel.load() ? 1 : 0;
	if (out_results) *out_results = std::move(results);
	return sum;
}

} // namespace AuditTests
