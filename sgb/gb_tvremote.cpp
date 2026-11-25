/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Frame formats per the IRP definitions (DecodeIR / IrpTransmogrifier); key codes
// from the irdb tables for each maker's TV device.

#include "gb_tvremote.h"
#include "gb_memory.h"
#include "gb_ppu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SGB {

namespace {

constexpr double kDotsPerUs = 4.194304;

// The sensor's answer to one carrier pulse [on, off) of a remote: [on + rise, off + fall).
// A remote's LED is far brighter than a console's and the receiver lets go of it
// slower. Doraemon Memories counts each burst's lit polls: its windows (AEHA leader
// >= 296, NEC leader < 946, Sony '1' >= 76) hold with the sensor lit ~70% of a burst.
constexpr int64_t kRiseDots = 24;
constexpr int64_t kFallDots = 64;

enum class Proto : uint8_t { Nec, NecX2, Pioneer, Sony12, Kaseikyo, Jvc, Mitsubishi, Sharp, Rc5 };

struct Vendor
{
	const char *name;
	Proto       proto;
	uint8_t     device, sub;
	int16_t     key[kTvRemoteKeys];   // function code per key; -1: the remote has no such key
};

// Power, Mute, 1-12, CH+, CH-, VOL+, VOL-. A remote without 11 and 12 sends 0 for 10.
const Vendor kVendors[] =
{
	{ "Sony",       Proto::Sony12,     1,    0,    { 21, 20,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 16, 17, 18, 19 } },
	{ "Panasonic",  Proto::Kaseikyo,   0x80, 0x00, { 61, 50, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 52, 53, 32, 33 } },
	{ "Toshiba",    Proto::Nec,        0x40, 0xBF, { 18, 16,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 27, 31, 26, 30 } },
	{ "Hitachi",    Proto::Nec,        0x50, 0xAF, { 23, 11, 13, 14, 15, 28, 29, 30, 31,  4,  5, 12, -1, -1, 25, 24, 18, 21 } },
	{ "Sharp",      Proto::Sharp,      1,    0,    { 22, 23,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 17, 18, 20, 21 } },
	{ "JVC",        Proto::Jvc,        3,    0,    { 23, 28, 33, 34, 35, 36, 37, 38, 39, 40, 41, 32, -1, -1, 25, 24, 30, 31 } },
	{ "Mitsubishi", Proto::Mitsubishi, 71,   0,    {  2, 50,  0,  8, 16, 24, 32, 40, 48, 56,  1,  9, -1, -1, 10, 18, 34, 42 } },
	{ "Sanyo",      Proto::Nec,        0x38, 0xC7, { 18, 24,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0, -1, -1, 10, 11, 14, 15 } },
	{ "Pioneer",    Proto::Pioneer,    0xAA, 0x55, { 28, 73,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0, -1, -1, 16, 17, 10, 11 } },
	{ "LG",         Proto::Nec,        0x04, 0xFB, {  8,  9, 17, 18, 19, 20, 21, 22, 23, 24, 25, 16, -1, -1,  0,  1,  2,  3 } },
	{ "Samsung",    Proto::NecX2,      0x07, 0x07, {  2, 15,  4,  5,  6,  8,  9, 10, 12, 13, 14, 17, -1, -1, 18, 16,  7, 11 } },
	{ "Philips",    Proto::Rc5,        0,    0,    { 12, 13,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0, -1, -1, 32, 33, 16, 17 } },
};
constexpr int kVendorCount = static_cast<int>(sizeof kVendors / sizeof kVendors[0]);

const char *const kKeyNames[kTvRemoteKeys] =
{
	"Power", "Mute", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "CH+", "CH-", "VOL+", "VOL-"
};

double CarrierHz(Proto p)
{
	switch (p)
	{
	case Proto::Pioneer: case Proto::Sony12: return 40000;
	case Proto::Kaseikyo:                    return 37000;
	case Proto::Mitsubishi:                  return 32600;
	case Proto::Rc5:                         return 36000;
	case Proto::Jvc: case Proto::Sharp:      return 38000;
	default:                                 return 38400;
	}
}

// One frame: marks and spaces in microseconds, a mark first, a space last.
struct Builder
{
	std::vector<uint32_t> us;
	void Add(bool mark, uint32_t t)
	{
		if (!t) return;
		const bool last_mark = us.size() & 1;
		if (us.empty()) { if (mark) us.push_back(t); return; }   // a leading space is not seen
		if (mark == last_mark) us.back() += t; else us.push_back(t);
	}
	// Pulse distance, LSB first: a mark of t, then a space of `zero` or `one` times t.
	void Distance(uint32_t value, int bits, uint32_t t, int zero, int one)
	{
		for (int i = 0; i < bits; ++i)
		{
			Add(true, t);
			Add(false, t * (((value >> i) & 1) ? one : zero));
		}
	}
	void PadTo(uint32_t total)
	{
		uint32_t sum = 0;
		for (uint32_t v : us) sum += v;
		Add(false, total > sum ? total - sum : 1000);
	}
};

// A frame on the console's clock: the dot each mark and space starts at, and the end.
struct Signal
{
	std::vector<int64_t> edge;
	void Set(const Builder &b)
	{
		edge.assign(1, 0);
		double sum = 0;
		for (uint32_t v : b.us) { sum += v; edge.push_back(std::llround(sum * kDotsPerUs)); }
	}
	int64_t Length() const { return edge.back(); }
};

struct Press
{
	int     key = -1;
	int64_t pressed = 0, t0 = 0, release = INT64_MAX;   // the button went down, its first frame starts, it came up
	Signal  first, repeat;      // the first frame, then the one a held button repeats
	int     min_frames = 1;
	double  period = 0, lit = 0;   // the carrier's period and the sensor's lit part of it, in dots
};

const Memory *g_face = nullptr;
int           g_vendor = -1;
Press         g_press;
bool          g_rc5_toggle = false;
// A frontend reports buttons once a frame, on the console's VBlank; a finger lands anywhere
// in the frame. (Mission: Impossible is deaf for ~5 ms after each VBlank and loses a leader there.)
uint32_t      g_phase = 0x2545F491;
constexpr int64_t kFrameDots = 70224;

void Build(const Vendor &v, int f, Press &p)
{
	Builder a, r;
	p.min_frames = 1;
	switch (v.proto)
	{
	case Proto::Nec: case Proto::Pioneer: case Proto::NecX2:
	{
		const uint32_t t = 564;
		const bool x2 = v.proto == Proto::NecX2;
		a.Add(true, (x2 ? 8 : 16) * t); a.Add(false, 8 * t);
		a.Distance(v.device, 8, t, 1, 3); a.Distance(v.sub, 8, t, 1, 3);
		a.Distance(f, 8, t, 1, 3);        a.Distance(~f & 0xFF, 8, t, 1, 3);
		a.Add(true, t); a.PadTo(108000);
		if (v.proto == Proto::Nec)   // a held button: the short "same again" frame
		{
			r.Add(true, 16 * t); r.Add(false, 4 * t); r.Add(true, t); r.PadTo(108000);
		}
		else r = a;
		break;
	}
	case Proto::Sony12:
	{
		const uint32_t t = 600;
		a.Add(true, 4 * t); a.Add(false, t);
		const uint32_t bits = static_cast<uint32_t>(f & 0x7F) | (static_cast<uint32_t>(v.device & 0x1F) << 7);
		for (int i = 0; i < 12; ++i) { a.Add(true, ((bits >> i) & 1) ? 2 * t : t); a.Add(false, t); }
		a.PadTo(45000);
		r = a;
		p.min_frames = 3;
		break;
	}
	case Proto::Kaseikyo:
	{
		const uint32_t t = 432;
		a.Add(true, 8 * t); a.Add(false, 4 * t);
		a.Distance(0x02, 8, t, 1, 3); a.Distance(0x20, 8, t, 1, 3);
		a.Distance(v.device, 8, t, 1, 3); a.Distance(v.sub, 8, t, 1, 3);
		a.Distance(f, 8, t, 1, 3); a.Distance((v.device ^ v.sub ^ f) & 0xFF, 8, t, 1, 3);
		a.Add(true, t); a.Add(false, 173 * t);
		r = a;
		break;
	}
	case Proto::Jvc:
	{
		const uint32_t t = 525;
		r.Distance(v.device, 8, t, 1, 3); r.Distance(f, 8, t, 1, 3);
		r.Add(true, t); r.Add(false, 45 * t);
		a.Add(true, 16 * t); a.Add(false, 8 * t);
		for (size_t i = 0; i < r.us.size(); ++i) a.Add(!(i & 1), r.us[i]);
		break;
	}
	case Proto::Mitsubishi:
	{
		const uint32_t t = 300;
		a.Distance(v.device, 8, t, 3, 7); a.Distance(f, 8, t, 3, 7);
		a.Add(true, t); a.Add(false, 80 * t);
		r = a;
		break;
	}
	case Proto::Sharp:
	{
		const uint32_t t = 264;
		a.Distance(v.device, 5, t, 3, 7); a.Distance(f, 8, t, 3, 7); a.Distance(1, 2, t, 3, 7);
		a.Add(true, t); a.Add(false, 165 * t);
		a.Distance(v.device, 5, t, 3, 7); a.Distance(~f & 0xFF, 8, t, 3, 7); a.Distance(2, 2, t, 3, 7);
		a.Add(true, t); a.Add(false, 165 * t);
		r = a;
		break;
	}
	case Proto::Rc5:
	{
		// MSB first, a '1' is space then mark: start, the inverted 7th function bit, toggle, 5 + 6 bits.
		const uint32_t t = 889;
		const uint32_t bits = (1u << 13) | ((f & 0x40) ? 0 : 1u << 12) | (g_rc5_toggle ? 1u << 11 : 0) |
		                      (static_cast<uint32_t>(v.device & 0x1F) << 6) | static_cast<uint32_t>(f & 0x3F);
		for (int i = 13; i >= 0; --i)
		{
			const bool one = (bits >> i) & 1;
			a.Add(!one, t); a.Add(one, t);
		}
		a.PadTo(114000);
		r = a;
		break;
	}
	}
	p.first.Set(a);
	p.repeat.Set(r);
	p.period = kDotsPerUs * 1e6 / CarrierHz(v.proto);
	p.lit    = p.period / 3 + static_cast<double>(kFallDots - kRiseDots);   // a 1/3 duty carrier
}

// ---- the TV: the console's LED, demodulated and decoded ----

// Carrier pulses at most this far apart belong to one burst (a 30 kHz carrier's third).
constexpr int64_t kBurstGapDots = 420;
// A pulse longer than a 30 kHz carrier's half is a steady light, not a remote.
constexpr int64_t kMaxPulseDots = 170;
// Darkness this long ends a frame.
constexpr int64_t kFrameGapDots = 41943;   // 10 ms

struct Tv
{
	std::vector<uint32_t> us;   // the frame so far: marks and spaces, a mark first
	int64_t  mark_start = 0, last_on = 0, last_off = 0;
	int      pulses = 0;
	bool     led = false, in_mark = false, bad = false;
	unsigned serial = 0;
	char     text[64] = "";
	char     last_nec[64] = "";   // what a "same again" frame repeats
};
Tv g_tv;
void (*g_notify)(const char *) = nullptr;

bool Near(double x, double nominal)
{
	return std::fabs(x - nominal) <= nominal * 0.25 + 80;
}

Proto Family(Proto p) { return p == Proto::Pioneer ? Proto::Nec : p; }

void Report(const char *text)
{
	std::snprintf(g_tv.text, sizeof g_tv.text, "%s", text);
	++g_tv.serial;
	if (g_notify) g_notify(g_tv.text);
}

void ReportCode(Proto family, const char *label, unsigned d, unsigned s, unsigned f)
{
	const bool two = family == Proto::Nec || family == Proto::NecX2 || family == Proto::Kaseikyo;   // a two-byte address
	char text[64];
	const Vendor *maker = nullptr;
	for (const Vendor &v : kVendors)
		if (Family(v.proto) == family && v.device == d && (!two || v.sub == s)) { maker = &v; break; }
	int key = -1;
	for (int k = 0; maker && k < kTvRemoteKeys; ++k)
		if (maker->key[k] == static_cast<int>(f)) { key = k; break; }
	if (key >= 0)   std::snprintf(text, sizeof text, "%s: %s", maker->name, kKeyNames[key]);
	else if (maker) std::snprintf(text, sizeof text, "%s: code %02X", maker->name, f);
	else if (two)   std::snprintf(text, sizeof text, "%s %02X%02X: code %02X", label, d, s, f);
	else            std::snprintf(text, sizeof text, "%s %02X: code %02X", label, d, f);
	if (family == Proto::Nec) std::snprintf(g_tv.last_nec, sizeof g_tv.last_nec, "%s", text);
	Report(text);
}

// `bits` pulse-distance bits from pair `first` on, LSB first: a space over `threshold` is a 1.
bool Bits(const std::vector<uint32_t> &us, size_t first, int bits, uint32_t threshold, uint64_t *out)
{
	if (us.size() < (first + bits) * 2) return false;
	uint64_t v = 0;
	for (int i = 0; i < bits; ++i)
		if (us[(first + i) * 2 + 1] > threshold) v |= 1ull << i;
	*out = v;
	return true;
}

void Decode()
{
	Tv &tv = g_tv;
	std::vector<uint32_t> us;
	us.swap(tv.us);
	const bool bad = tv.bad;
	tv.bad = false;
	const size_t marks = (us.size() + 1) / 2;
	if (bad || !marks) return;
	const double m0 = us[0], s0 = us.size() > 1 ? us[1] : 0;
	uint64_t v = 0;

	if (Near(m0, 9024) || Near(m0, 8400))
	{
		if (marks == 2 && Near(s0, 2256)) { if (tv.last_nec[0]) Report(tv.last_nec); return; }
		if (marks == 18 && Bits(us, 1, 16, 1050, &v))
			return ReportCode(Proto::Jvc, "JVC", v & 0xFF, 0, (v >> 8) & 0xFF);
		if (marks == 34 && Near(s0, 4512) && Bits(us, 1, 32, 1128, &v) && (((v >> 16) ^ (v >> 24)) & 0xFF) == 0xFF)
			return ReportCode(Proto::Nec, "NEC", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
	}
	else if (Near(m0, 4512) && Near(s0, 4512))
	{
		if (marks == 34 && Bits(us, 1, 32, 1128, &v) && (((v >> 16) ^ (v >> 24)) & 0xFF) == 0xFF)
			return ReportCode(Proto::NecX2, "NECx", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
	}
	else if (Near(m0, 3456) && Near(s0, 1728))
	{
		if (marks == 50 && Bits(us, 1, 48, 864, &v) && (v & 0xFFFF) == 0x2002 &&
		    (((v >> 16) ^ (v >> 24) ^ (v >> 32) ^ (v >> 40)) & 0xFF) == 0)
			return ReportCode(Proto::Kaseikyo, "Kaseikyo", (v >> 16) & 0xFF, (v >> 24) & 0xFF, (v >> 32) & 0xFF);
	}
	else if (Near(m0, 2400) && Near(s0, 600))
	{
		if (marks == 13)
		{
			for (int i = 0; i < 12; ++i)
				if (us[(1 + i) * 2] > 900) v |= 1ull << i;
			return ReportCode(Proto::Sony12, "Sony", (v >> 7) & 0x1F, 0, v & 0x7F);
		}
	}
	else if (marks == 17 && m0 < 700 && Bits(us, 0, 16, 0, &v))
	{
		uint32_t shortest = UINT32_MAX;
		for (size_t i = 1; i < 32; i += 2) shortest = (std::min)(shortest, us[i]);
		if (shortest < 700)
		{
			Bits(us, 0, 16, 1050, &v);   // JVC's held button: frames without the leader
			return ReportCode(Proto::Jvc, "JVC", v & 0xFF, 0, (v >> 8) & 0xFF);
		}
		Bits(us, 0, 16, 1500, &v);
		return ReportCode(Proto::Mitsubishi, "Mitsubishi", v & 0xFF, 0, (v >> 8) & 0xFF);
	}
	else if (marks == 16 && m0 < 700 && Bits(us, 0, 15, 1320, &v))
	{
		if (((v >> 13) & 3) != 1) return;   // the inverted second half
		return ReportCode(Proto::Sharp, "Sharp", v & 0x1F, 0, (v >> 5) & 0xFF);
	}
	else if (marks >= 7 && marks <= 14 && (Near(m0, 889) || Near(m0, 1778)))
	{
		// RC-5: half bits, the first one the unseen space of the start bit.
		std::vector<uint8_t> half(1, 0);
		bool ok = true;
		for (size_t i = 0; i < us.size() && ok; ++i)
		{
			const bool mark = !(i & 1);
			const int n = Near(us[i], 889) ? 1 : Near(us[i], 1778) ? 2 : (!mark && i + 1 == us.size()) ? 0 : -1;
			if (n < 0) ok = false;
			for (int k = 0; k < n; ++k) half.push_back(mark ? 1 : 0);
		}
		half.resize(28, 0);
		for (int i = 0; i < 14 && ok; ++i)
		{
			if (half[i * 2] == half[i * 2 + 1]) ok = false;
			v = (v << 1) | half[i * 2 + 1];
		}
		if (ok && (v & 0x2000))
			return ReportCode(Proto::Rc5, "RC-5", (v >> 6) & 0x1F, 0, (v & 0x3F) | ((v & 0x1000) ? 0 : 0x40));
	}

	if (marks >= 8)
	{
		char text[64];
		std::snprintf(text, sizeof text, "unknown remote signal (%u bursts)", static_cast<unsigned>(marks));
		Report(text);
	}
}

void CloseMark()
{
	Tv &tv = g_tv;
	if (tv.pulses < 3) tv.bad = true;
	// From the first pulse's start to the end of the last one's carrier period.
	const double period = tv.pulses > 1 ? static_cast<double>(tv.last_on - tv.mark_start) / (tv.pulses - 1) : 0;
	tv.us.push_back(static_cast<uint32_t>(((tv.last_on - tv.mark_start) + period) / kDotsPerUs + 0.5));
	tv.in_mark = false;
}

void TvReset()
{
	const unsigned serial = g_tv.serial;
	g_tv = Tv();
	g_tv.serial = serial;
}

} // anonymous

int TvRemoteVendors() { return kVendorCount; }

const char *TvRemoteVendorName(int vendor)
{
	return (vendor >= 0 && vendor < kVendorCount) ? kVendors[vendor].name : "";
}

bool TvRemoteHasKey(int vendor, int key)
{
	return vendor >= 0 && vendor < kVendorCount && key >= 0 && key < kTvRemoteKeys && kVendors[vendor].key[key] >= 0;
}

const char *TvRemoteKeyName(int key)
{
	return (key >= 0 && key < kTvRemoteKeys) ? kKeyNames[key] : "";
}

void TvRemoteFace(const Memory *m, int vendor)
{
	if (vendor < 0 || vendor >= kVendorCount) { vendor = -1; m = nullptr; }
	if (m == g_face && vendor == g_vendor) return;
	g_face = m;
	g_vendor = vendor;
	g_press = Press();
	TvReset();
}

int TvRemoteVendor() { return g_vendor; }

void TvRemoteKey(int key, bool down)
{
	Press &p = g_press;
	if (!g_face || !g_face->ppu || g_vendor < 0) return;
	const int64_t now = g_face->ppu->t_cycles;
	if (!down)
	{
		if (key == p.key && p.release == INT64_MAX) p.release = now;
		return;
	}
	if (!TvRemoteHasKey(g_vendor, key) || (p.key >= 0 && p.release == INT64_MAX)) return;
	p = Press();
	p.key = key;
	g_phase = g_phase * 1664525u + 1013904223u;
	p.pressed = now;
	p.t0 = now + static_cast<int64_t>((g_phase >> 8) % kFrameDots);
	if (kVendors[g_vendor].proto == Proto::Rc5) g_rc5_toggle = !g_rc5_toggle;
	Build(kVendors[g_vendor], kVendors[g_vendor].key[key], p);
}

bool TvRemoteSeen(const Memory &m)
{
	Press &p = g_press;
	if (&m != g_face || p.key < 0 || !m.ppu) return false;
	if (m.ppu->t_cycles < p.pressed) { p.key = -1; return false; }   // the clock went back: a reset or a loaded state
	const int64_t u = m.ppu->t_cycles - kRiseDots - p.t0;
	if (u < 0) return false;

	const int64_t first = p.first.Length(), repeat = p.repeat.Length();
	const int64_t frame = u < first ? 0 : 1 + (u - first) / repeat;
	if (p.release != INT64_MAX)
	{
		// A released button finishes the frame it is in, and the fewest its format sends.
		const int64_t r = p.release - p.t0;
		const int64_t frames = (std::max<int64_t>)(p.min_frames, r < first ? 1 : 2 + (r - first) / repeat);
		if (frame >= frames) { p.key = -1; return false; }
	}

	const Signal &s = frame ? p.repeat : p.first;
	const int64_t o = frame ? (u - first) % repeat : u;
	const size_t i = static_cast<size_t>(std::upper_bound(s.edge.begin(), s.edge.end(), o) - s.edge.begin()) - 1;
	if ((i & 1) || i + 1 >= s.edge.size()) return false;   // a space

	// A burst is whole carrier cycles; the sensor reports each pulse for `lit` dots.
	const double  off    = static_cast<double>(o - s.edge[i]);
	const int64_t pulses = std::llround(static_cast<double>(s.edge[i + 1] - s.edge[i]) / p.period);
	const int64_t k      = static_cast<int64_t>(off / p.period);
	return k < pulses && off - static_cast<double>(k) * p.period < p.lit;
}

void TvRemoteLed(const Memory &m, bool on)
{
	Tv &tv = g_tv;
	if (&m != g_face || !m.ppu || on == tv.led) return;
	const int64_t t = m.ppu->t_cycles;
	if (t < tv.last_on || t < tv.last_off) TvReset();   // the clock went back
	tv.led = on;
	if (!on)
	{
		tv.last_off = t;
		if (t - tv.last_on > kMaxPulseDots) tv.bad = true;
		return;
	}
	if (tv.in_mark && t - tv.last_on <= kBurstGapDots)
		++tv.pulses;
	else
	{
		if (tv.in_mark) CloseMark();
		if (!tv.us.empty())
		{
			if (t - tv.last_off > kFrameGapDots) Decode();
			else tv.us.push_back(static_cast<uint32_t>((t - tv.last_off) / kDotsPerUs + 0.5));
		}
		tv.in_mark = true;
		tv.mark_start = t;
		tv.pulses = 1;
	}
	tv.last_on = t;
}

void TvRemoteFrame(const Memory &m)
{
	Tv &tv = g_tv;
	if (&m != g_face || !m.ppu || tv.led || (!tv.in_mark && tv.us.empty())) return;
	const int64_t t = m.ppu->t_cycles;
	if (t < tv.last_off) { TvReset(); return; }
	if (t - tv.last_off <= kFrameGapDots) return;
	if (tv.in_mark) CloseMark();
	Decode();
}

bool TvReceived(unsigned *serial, const char **text)
{
	if (serial) *serial = g_tv.serial;
	if (text)   *text = g_tv.text;
	return g_tv.serial != 0;
}

void TvSetNotify(void (*fn)(const char *text)) { g_notify = fn; }

} // namespace SGB
