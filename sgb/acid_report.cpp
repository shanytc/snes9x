/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "acid_report.h"
#include "acid_baseline.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace AcidTests {

namespace {

const char *StatusWord(const ReportRow &r)
{
	if (!r.result) return "skipped";
	switch (r.result->status)
	{
		case Status::Pass:  return "pass";
		case Status::Fail:  return "fail";
		case Status::Info:  return "info";
		default:            return "error";
	}
}

const char *StatusTag(const ReportRow &r)
{
	return r.result ? StatusName(r.result->status) : "SKIP";
}

// Short baseline verdict for the text and HTML tables.
std::string MatchTag(const ReportRow &r)
{
	switch (r.match)
	{
		case Match::Same:    return "SAME";
		case Match::Differs: return "DIFF";
		case Match::NoImage: return "MISSING";
		case Match::NoFrame: return "-";
		default:             return "";
	}
}

// True once any row carries a baseline verdict.
bool AnyBaseline(const std::vector<ReportRow> &rows)
{
	for (const ReportRow &r : rows)
		if (r.match != Match::None) return true;
	return false;
}

struct MatchCounts
{
	int same = 0, differs = 0, missing = 0, noframe = 0;

	void Add(const ReportRow &r)
	{
		switch (r.match)
		{
			case Match::Same:    ++same;    break;
			case Match::Differs: ++differs; break;
			case Match::NoImage: ++missing; break;
			case Match::NoFrame: ++noframe; break;
			default: break;
		}
	}
};

struct Counts
{
	int total = 0, pass = 0, fail = 0, info = 0, err = 0, skip = 0;

	void Add(const ReportRow &r)
	{
		++total;
		if      (!r.result)                          ++skip;
		else if (r.result->status == Status::Pass)   ++pass;
		else if (r.result->status == Status::Fail)   ++fail;
		else if (r.result->status == Status::Info)   ++info;
		else                                         ++err;
	}
	// Informational tests have no reference image to match, so they count
	// toward the score: a clean run is every test in the manifest.
	int Ok()     const { return pass + info; }
	int Scored() const { return total - skip; }
};

// Suite tallies in first-seen order.
struct SuiteCount { std::string name; Counts c; };

std::vector<SuiteCount> TallySuites(const std::vector<ReportRow> &rows)
{
	std::vector<SuiteCount> out;
	for (const ReportRow &r : rows)
	{
		if (!r.test) continue;
		auto it = std::find_if(out.begin(), out.end(),
			[&](const SuiteCount &s) { return s.name == r.test->suite; });
		if (it == out.end())
		{
			out.push_back({ r.test->suite, Counts() });
			it = out.end() - 1;
		}
		it->c.Add(r);
	}
	return out;
}

} // anonymous

std::string Timestamp()
{
	const std::time_t t = std::time(nullptr);
	std::tm tm = {};
#if defined(_MSC_VER)
	localtime_s(&tm, &t);
#else
	tm = *std::localtime(&t);
#endif
	char buf[64];
	std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

namespace {

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string Fmt(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return buf;
}

/*--------------------------------------------------------------------------
  PNG writer. Self-contained so the headless CLI needs no image library.
--------------------------------------------------------------------------*/

uint32_t Crc32(const uint8_t *d, size_t n)
{
	static uint32_t table[256];
	static bool built = false;
	if (!built)
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
			table[i] = c;
		}
		built = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i)
		crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const uint8_t *d, size_t n)
{
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < n; ++i)
	{
		a += d[i];
		if (a >= 65521) a -= 65521;
		b += a;
		if (b >= 65521) b -= 65521;
	}
	return (b << 16) | a;
}

// Deflate bit stream: plain values go out LSB-first, Huffman codes MSB-first.
struct BitOut
{
	std::vector<uint8_t> &out;
	uint32_t bits = 0;
	int      n    = 0;

	explicit BitOut(std::vector<uint8_t> &o) : out(o) {}

	void Put(uint32_t v, int count)
	{
		if (count <= 0) return;
		bits |= (v & ((1u << count) - 1)) << n;
		n += count;
		while (n >= 8)
		{
			out.push_back(static_cast<uint8_t>(bits & 0xFF));
			bits >>= 8;
			n -= 8;
		}
	}
	void PutCode(uint32_t code, int count)
	{
		for (int i = count - 1; i >= 0; --i) Put((code >> i) & 1, 1);
	}
	void Align() { if (n) Put(0, 8 - n); }
};

const uint16_t kLenBase[29]   = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,
                                  43,51,59,67,83,99,115,131,163,195,227,258 };
const uint8_t  kLenExtra[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,
                                  4,4,5,5,5,5,0 };
const uint16_t kDistBase[30]  = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
                                  257,385,513,769,1025,1537,2049,3073,4097,
                                  6145,8193,12289,16385,24577 };
const uint8_t  kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
                                  10,10,11,11,12,12,13,13 };

void PutSymbol(BitOut &b, int sym)
{
	if      (sym <= 143) b.PutCode(0x30 + sym, 8);
	else if (sym <= 255) b.PutCode(0x190 + sym - 144, 9);
	else if (sym <= 279) b.PutCode(sym - 256, 7);
	else                 b.PutCode(0xC0 + sym - 280, 8);
}

void PutMatch(BitOut &b, int len, int dist)
{
	int lc = 28;
	while (lc > 0 && len < kLenBase[lc]) --lc;
	PutSymbol(b, 257 + lc);
	b.Put(static_cast<uint32_t>(len - kLenBase[lc]), kLenExtra[lc]);
	int dc = 29;
	while (dc > 0 && dist < kDistBase[dc]) --dc;
	b.PutCode(static_cast<uint32_t>(dc), 5);
	b.Put(static_cast<uint32_t>(dist - kDistBase[dc]), kDistExtra[dc]);
}

// One fixed-Huffman block with greedy LZ77. Plenty for GB screenshots, which
// are mostly flat runs.
void Deflate(const uint8_t *data, size_t len, std::vector<uint8_t> &out)
{
	constexpr int kHashBits = 15, kHashSize = 1 << kHashBits;
	constexpr int kWindow = 32768, kMinMatch = 3, kMaxMatch = 258, kChain = 24;

	BitOut b(out);
	b.Put(1, 1);   // final block
	b.Put(1, 2);   // fixed Huffman

	std::vector<int> head(kHashSize, -1);
	std::vector<int> prev(len ? len : 1, -1);
	auto hash = [&](size_t p) {
		return static_cast<size_t>(((data[p] << 10) ^ (data[p + 1] << 5) ^
		                            data[p + 2]) & (kHashSize - 1));
	};

	size_t pos = 0;
	while (pos < len)
	{
		int best_len = 0, best_dist = 0;
		if (pos + kMinMatch <= len)
		{
			const size_t h = hash(pos);
			const size_t max = std::min<size_t>(kMaxMatch, len - pos);
			for (int cand = head[h], probe = 0;
			     cand >= 0 && probe < kChain; ++probe)
			{
				const size_t dist = pos - static_cast<size_t>(cand);
				if (dist == 0 || dist > kWindow) break;
				size_t l = 0;
				while (l < max && data[cand + l] == data[pos + l]) ++l;
				if (static_cast<int>(l) > best_len)
				{
					best_len  = static_cast<int>(l);
					best_dist = static_cast<int>(dist);
					if (l == max) break;
				}
				cand = prev[cand];
			}
			prev[pos] = head[h];
			head[h]   = static_cast<int>(pos);
		}

		if (best_len >= kMinMatch)
		{
			PutMatch(b, best_len, best_dist);
			for (int k = 1; k < best_len; ++k)
			{
				const size_t p = pos + k;
				if (p + kMinMatch > len) break;
				const size_t h = hash(p);
				prev[p] = head[h];
				head[h] = static_cast<int>(p);
			}
			pos += best_len;
		}
		else
		{
			PutSymbol(b, data[pos]);
			++pos;
		}
	}
	PutSymbol(b, 256);
	b.Align();
}

void PutU32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 24));
	v.push_back(static_cast<uint8_t>(x >> 16));
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t> &png, const char *type,
           const std::vector<uint8_t> &data)
{
	PutU32(png, static_cast<uint32_t>(data.size()));
	const size_t start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	PutU32(png, Crc32(png.data() + start, png.size() - start));
}

// Per-row filter pick, the usual minimum-absolute-sum heuristic.
void FilterRow(const uint8_t *row, const uint8_t *up, int stride,
               std::vector<uint8_t> &raw)
{
	static thread_local std::vector<uint8_t> cand[3];
	long best_sum = -1;
	int  best     = 0;
	for (int f = 0; f < 3; ++f)
	{
		cand[f].resize(stride);
		long sum = 0;
		for (int x = 0; x < stride; ++x)
		{
			int p = 0;
			if      (f == 1) p = (x >= 3) ? row[x - 3] : 0;
			else if (f == 2) p = up ? up[x] : 0;
			const uint8_t v = static_cast<uint8_t>(row[x] - p);
			cand[f][x] = v;
			sum += (v < 128) ? v : 256 - v;
		}
		if (best_sum < 0 || sum < best_sum) { best_sum = sum; best = f; }
	}
	raw.push_back(static_cast<uint8_t>(best));
	raw.insert(raw.end(), cand[best].begin(), cand[best].end());
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::vector<uint8_t> &in)
{
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3)
	{
		const uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += kB64[v & 63];
	}
	if (i + 1 == in.size())
	{
		const uint32_t v = in[i] << 16;
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == in.size())
	{
		const uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

/*--------------------------------------------------------------------------
  Escaping
--------------------------------------------------------------------------*/

std::string JsonStr(const std::string &s)
{
	std::string o = "\"";
	for (unsigned char c : s)
	{
		switch (c)
		{
			case '"':  o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n";  break;
			case '\r': o += "\\r";  break;
			case '\t': o += "\\t";  break;
			default:
				if (c < 0x20 || c >= 0x7F) o += Fmt("\\u%04x", c);
				else                       o += static_cast<char>(c);
		}
	}
	return o + "\"";
}

std::string Html(const std::string &s)
{
	std::string o;
	for (unsigned char c : s)
	{
		switch (c)
		{
			case '&': o += "&amp;";  break;
			case '<': o += "&lt;";   break;
			case '>': o += "&gt;";   break;
			case '"': o += "&quot;"; break;
			default:  o += (c < 0x20) ? ' ' : static_cast<char>(c);
		}
	}
	return o;
}

/*--------------------------------------------------------------------------
  Renderers
--------------------------------------------------------------------------*/

std::string ScoreLine(const Counts &c)
{
	return Fmt("%d/%d passed (%d matched + %d informational, %d failed, %d errors%s)",
	           c.Ok(), c.Scored(), c.pass, c.info, c.fail, c.err,
	           c.skip ? Fmt(", %d not run", c.skip).c_str() : "");
}

std::string RenderText(const std::vector<ReportRow> &rows, const ReportInfo &info)
{
	Counts total;
	MatchCounts mc;
	const bool base = AnyBaseline(rows);
	std::string o;
	o += "# " + info.title + " - GB Emulator Shootout\n";
	o += "# generated " + info.generated;
	o += Fmt(", %d thread%s, %.1fs\n", info.threads, info.threads == 1 ? "" : "s",
	         info.seconds);
	o += "# filter: " + info.filter + "\n";
	if (!info.source.empty()) o += "# tests: " + info.source + "\n";
	if (base && info.baseline)
	{
		o += "# baseline: " + info.baseline->dir;
		if (!info.baseline->title.empty()) o += " (" + info.baseline->title + ")";
		o += "\n";
	}
	if (!info.env.empty())    o += "# timing overrides in effect: " + info.env + "\n";
	if (info.cancelled)       o += "# run was cancelled - some tests never ran\n";
	o += "\n";

	for (const ReportRow &r : rows)
	{
		if (!r.test) continue;
		total.Add(r);
		mc.Add(r);
		o += Fmt("%-5s %-60s ", StatusTag(r), r.test->name.c_str());
		if (base)
			o += Fmt("%-8s ", r.match == Match::Differs
			                  ? Fmt("DIFF %d", r.diff_px).c_str()
			                  : MatchTag(r).c_str());
		if (r.result) o += r.result->detail;
		o += "\n";
	}

	o += "\n";
	o += Fmt("%-28s %5s %5s %5s %5s %5s %5s\n", "suite", "total", "pass",
	         "fail", "info", "err", "skip");
	for (const SuiteCount &s : TallySuites(rows))
		o += Fmt("%-28s %5d %5d %5d %5d %5d %5d\n", s.name.c_str(), s.c.total,
		         s.c.pass, s.c.fail, s.c.info, s.c.err, s.c.skip);

	o += "\n" + ScoreLine(total) + (info.cancelled ? ", cancelled" : "") + "\n";
	if (base)
		o += Fmt("baseline: %d same, %d differ, %d missing, %d not run\n",
		         mc.same, mc.differs, mc.missing, mc.noframe);
	return o;
}

std::string RenderJson(const std::vector<ReportRow> &rows, const ReportInfo &info)
{
	Counts total;
	for (const ReportRow &r : rows)
		if (r.test) total.Add(r);

	std::string o = "{\n";
	o += "  \"emulator\": " + JsonStr(info.title) + ",\n";
	o += "  \"generated\": " + JsonStr(info.generated) + ",\n";
	o += "  \"filter\": " + JsonStr(info.filter) + ",\n";
	o += "  \"tests_dir\": " + JsonStr(info.source) + ",\n";
	o += "  \"env_overrides\": " + JsonStr(info.env) + ",\n";
	o += Fmt("  \"threads\": %d,\n", info.threads);
	o += Fmt("  \"duration_seconds\": %.2f,\n", info.seconds);
	o += Fmt("  \"cancelled\": %s,\n", info.cancelled ? "true" : "false");

	o += "  \"summary\": {";
	o += Fmt("\"total\": %d, \"passed\": %d, \"failed\": %d, \"info\": %d, "
	         "\"errors\": %d, \"skipped\": %d, \"score\": %d, \"scored\": %d}",
	         total.total, total.pass, total.fail, total.info, total.err,
	         total.skip, total.Ok(), total.Scored());
	o += ",\n";

	if (AnyBaseline(rows))
	{
		MatchCounts mc;
		for (const ReportRow &r : rows) mc.Add(r);
		o += "  \"baseline\": {";
		o += "\"dir\": " + JsonStr(info.baseline ? info.baseline->dir : std::string());
		o += ", \"emulator\": " +
		     JsonStr(info.baseline ? info.baseline->title : std::string());
		o += Fmt(", \"same\": %d, \"differs\": %d, \"missing\": %d, \"not_run\": %d}",
		         mc.same, mc.differs, mc.missing, mc.noframe);
		o += ",\n";
	}

	o += "  \"suites\": [\n";
	const std::vector<SuiteCount> suites = TallySuites(rows);
	for (size_t i = 0; i < suites.size(); ++i)
	{
		const Counts &c = suites[i].c;
		o += "    {\"suite\": " + JsonStr(suites[i].name);
		o += Fmt(", \"total\": %d, \"passed\": %d, \"failed\": %d, \"info\": %d, "
		         "\"errors\": %d, \"skipped\": %d}", c.total, c.pass, c.fail,
		         c.info, c.err, c.skip);
		o += (i + 1 < suites.size()) ? ",\n" : "\n";
	}
	o += "  ],\n";

	o += "  \"tests\": [\n";
	bool first = true;
	for (const ReportRow &r : rows)
	{
		if (!r.test) continue;
		if (!first) o += ",\n";
		first = false;
		const Test &t = *r.test;
		o += "    {\"name\": " + JsonStr(t.name);
		o += ", \"suite\": " + JsonStr(t.suite);
		o += ", \"model\": " + JsonStr(ModelName(t.model));
		o += Fmt(", \"runtime\": %g", t.runtime);
		o += ", \"rom\": " + JsonStr(t.rom);
		o += ", \"pass_images\": [";
		for (size_t i = 0; i < t.pass_images.size(); ++i)
			o += (i ? ", " : "") + JsonStr(t.pass_images[i]);
		o += "], \"fail_images\": [";
		for (size_t i = 0; i < t.fail_images.size(); ++i)
			o += (i ? ", " : "") + JsonStr(t.fail_images[i]);
		o += "], \"status\": " + JsonStr(StatusWord(r));
		o += Fmt(", \"frames\": %d", r.result ? r.result->frames : 0);
		o += ", \"detail\": " + JsonStr(r.result ? r.result->detail : std::string());
		o += Fmt(", \"ran\": %s", r.result ? "true" : "false");
		if (r.match != Match::None)
			o += ", \"baseline\": " + JsonStr(MatchName(r.match)) +
			     Fmt(", \"baseline_diff_px\": %d", r.diff_px);
		o += "}";
	}
	o += "\n  ]\n}\n";
	return o;
}

const char kHtmlStyle[] = R"CSS(
:root { color-scheme: light dark;
  --bg:#fff; --fg:#1c1f24; --dim:#666; --line:#e2e5ea; --head:#f6f7f9;
  --pass:#1a7f37; --fail:#c02626; --info:#1f5fbf; --err:#a4341c; --skip:#8a8f98; }
@media (prefers-color-scheme: dark) { :root {
  --bg:#15181d; --fg:#e6e8ec; --dim:#9aa0aa; --line:#2b3038; --head:#1c2027;
  --pass:#4fc26b; --fail:#ff6b6b; --info:#69a4ff; --err:#ff9470; --skip:#7b818b; } }
* { box-sizing:border-box }
body { margin:0; padding:24px; background:var(--bg); color:var(--fg);
  font:14px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif }
h1 { font-size:20px; margin:0 0 4px }
h2 { font-size:15px; margin:28px 0 8px; font-weight:600 }
.meta { color:var(--dim); margin:0 0 16px; font-size:13px }
.warn { background:#ffe9c7; color:#6b4a00; padding:8px 12px; border-radius:6px;
  margin:0 0 16px; font-size:13px }
@media (prefers-color-scheme: dark) { .warn { background:#463714; color:#f0d091 } }
.cards { display:flex; flex-wrap:wrap; gap:10px; margin:0 0 8px }
.card { border:1px solid var(--line); border-radius:8px; padding:8px 14px;
  min-width:88px }
.card b { display:block; font-size:20px; font-variant-numeric:tabular-nums }
.card span { color:var(--dim); font-size:12px }
table { border-collapse:collapse; width:100%; font-size:13px }
th,td { text-align:left; padding:5px 8px; border-bottom:1px solid var(--line);
  vertical-align:top }
th { background:var(--head); position:sticky; top:0; font-weight:600 }
td.num,td.frames { text-align:right; font-variant-numeric:tabular-nums;
  color:var(--dim) }
td.name,td.detail { font-family:ui-monospace,Consolas,monospace; font-size:12px }
td.detail { color:var(--dim) }
.pass{color:var(--pass)} .fail{color:var(--fail)} .info{color:var(--info)}
.error{color:var(--err)} .skipped{color:var(--skip)}
.same{color:var(--pass)} .differs{color:var(--fail)} .missing{color:var(--skip)}
.noframe{color:var(--skip)} .none{color:var(--skip)}
td.result { font-weight:600 }
img.shot { width:160px; height:144px; image-rendering:pixelated;
  border:1px solid var(--line); border-radius:3px; background:#000 }
.noshot { color:var(--skip); font-size:12px }
table.hideshots col.shotcol, table.hideshots th.shotcol,
table.hideshots td.shotcol { display:none }
.filters { display:flex; flex-wrap:wrap; gap:8px; align-items:center;
  margin:0 0 10px }
.filters input[type=search], .filters select { font:inherit; padding:4px 6px;
  border:1px solid var(--line); border-radius:5px; background:var(--bg);
  color:var(--fg) }
.filters input[type=search] { min-width:240px }
#count { color:var(--dim); font-size:12px; margin-left:auto }
)CSS";

const char kHtmlScript[] = R"JS(
var rows = Array.prototype.slice.call(
  document.querySelectorAll('#tests tbody tr'));
var q = document.getElementById('q'), suite = document.getElementById('suite'),
    model = document.getElementById('model'), stat = document.getElementById('stat'),
    shots = document.getElementById('shots'), count = document.getElementById('count'),
    table = document.getElementById('tests');
function apply() {
  var t = q.value.toLowerCase(), s = suite.value, m = model.value, st = stat.value, n = 0;
  for (var i = 0; i < rows.length; i++) {
    var r = rows[i], d = r.dataset;
    var ok = (!t || d.name.indexOf(t) >= 0) && (!s || d.suite === s) &&
             (!m || d.model === m) &&
             (!st || (st === 'bad' ? (d.status === 'fail' || d.status === 'error')
                    : st.slice(0, 2) === 'b-' ? d.baseline === st.slice(2)
                                   : d.status === st));
    r.style.display = ok ? '' : 'none';
    if (ok) n++;
  }
  count.textContent = n + ' of ' + rows.length + ' shown';
}
[q, suite, model, stat].forEach(function (e) {
  e.addEventListener('input', apply);
  e.addEventListener('change', apply);
});
shots.addEventListener('change', function () {
  table.classList.toggle('hideshots', !shots.checked);
});
apply();
)JS";

// One screenshot cell: the frame as an embedded PNG, or a dash.
std::string ShotCell(const std::vector<uint8_t> *rgb)
{
	if (!rgb || rgb->empty()) return "<span class=\"noshot\">&mdash;</span>";
	return "<img class=\"shot\" loading=\"lazy\" alt=\"\" src=\"data:image/png;base64," +
	       Base64(EncodePng(rgb->data(), kShotWidth, kShotHeight)) + "\">";
}

std::string RenderHtml(const std::vector<ReportRow> &rows, const ReportInfo &info)
{
	Counts total;
	for (const ReportRow &r : rows)
		if (r.test) total.Add(r);
	const std::vector<SuiteCount> suites = TallySuites(rows);

	const bool any_shot = std::any_of(rows.begin(), rows.end(),
		[](const ReportRow &r) { return r.result && !r.result->shot.empty(); });
	const bool base = AnyBaseline(rows);
	MatchCounts mc;
	for (const ReportRow &r : rows) mc.Add(r);

	std::string o;
	o += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
	o += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
	o += "<title>Acid Tests - " + Html(info.title) + "</title>\n<style>";
	o += kHtmlStyle;
	o += "</style>\n</head>\n<body>\n";

	o += "<h1>Acid Tests &mdash; " + Html(info.title) + "</h1>\n";
	o += "<p class=\"meta\">" + Html(info.generated);
	o += Fmt(" &middot; %d thread%s &middot; %.1f s &middot; ", info.threads,
	         info.threads == 1 ? "" : "s", info.seconds);
	o += "filter: " + Html(info.filter);
	if (!info.source.empty()) o += " &middot; " + Html(info.source);
	o += "</p>\n";
	if (base && info.baseline)
	{
		o += "<p class=\"meta\">Compared against baseline " +
		     Html(info.baseline->dir);
		if (!info.baseline->title.empty())
			o += " (" + Html(info.baseline->title) + ")";
		o += Fmt(" &mdash; %d same, %d differ, %d missing", mc.same, mc.differs,
		         mc.missing);
		if (mc.noframe) o += Fmt(", %d not run", mc.noframe);
		o += "</p>\n";
	}
	if (!info.env.empty())
		o += "<p class=\"warn\">Timing overrides in effect: " + Html(info.env) +
		     " &mdash; not comparable with a clean run.</p>\n";
	if (info.cancelled)
		o += "<p class=\"warn\">Run was cancelled; tests marked SKIP never ran.</p>\n";

	o += "<div class=\"cards\">\n";
	o += Fmt("<div class=\"card\"><b>%d/%d</b><span>score</span></div>\n",
	         total.Ok(), total.Scored());
	o += Fmt("<div class=\"card\"><b class=\"pass\">%d</b><span>passed</span></div>\n", total.pass);
	o += Fmt("<div class=\"card\"><b class=\"fail\">%d</b><span>failed</span></div>\n", total.fail);
	o += Fmt("<div class=\"card\"><b class=\"info\">%d</b><span>informational</span></div>\n", total.info);
	o += Fmt("<div class=\"card\"><b class=\"error\">%d</b><span>errors</span></div>\n", total.err);
	if (total.skip)
		o += Fmt("<div class=\"card\"><b class=\"skipped\">%d</b><span>not run</span></div>\n", total.skip);
	if (base)
		o += Fmt("<div class=\"card\"><b class=\"%s\">%d</b>"
		         "<span>differ from baseline</span></div>\n",
		         mc.differs ? "differs" : "same", mc.differs);
	o += "</div>\n";

	o += "<h2>Suites</h2>\n<table>\n<thead><tr><th>Suite</th><th>Score</th>"
	     "<th>Passed</th><th>Failed</th><th>Info</th><th>Errors</th>"
	     "<th>Not run</th></tr></thead>\n<tbody>\n";
	for (const SuiteCount &s : suites)
	{
		const Counts &c = s.c;
		o += "<tr><td>" + Html(s.name) + "</td>";
		o += Fmt("<td class=\"num\">%d/%d</td><td class=\"num pass\">%d</td>"
		         "<td class=\"num fail\">%d</td><td class=\"num info\">%d</td>"
		         "<td class=\"num error\">%d</td><td class=\"num skipped\">%d</td></tr>\n",
		         c.Ok(), c.Scored(), c.pass, c.fail, c.info, c.err, c.skip);
	}
	o += "</tbody>\n</table>\n";

	o += "<h2>Tests</h2>\n<div class=\"filters\">\n";
	o += "<input type=\"search\" id=\"q\" placeholder=\"Filter by name\">\n";
	o += "<select id=\"suite\"><option value=\"\">All suites</option>";
	for (const SuiteCount &s : suites)
		o += "<option>" + Html(s.name) + "</option>";
	o += "</select>\n";
	o += "<select id=\"model\"><option value=\"\">All models</option>"
	     "<option>DMG</option><option>CGB</option><option>SGB</option></select>\n";
	o += "<select id=\"stat\"><option value=\"\">All results</option>"
	     "<option value=\"pass\">Passed</option>"
	     "<option value=\"fail\">Failed</option>"
	     "<option value=\"bad\">Failed or errored</option>"
	     "<option value=\"info\">Informational</option>"
	     "<option value=\"error\">Errors</option>"
	     "<option value=\"skipped\">Not run</option>";
	if (base)
		o += "<option value=\"b-differs\">Differs from baseline</option>"
		     "<option value=\"b-same\">Matches baseline</option>"
		     "<option value=\"b-missing\">Missing from baseline</option>";
	o += "</select>\n";
	o += "<label><input type=\"checkbox\" id=\"shots\"";
	o += any_shot ? " checked" : "";
	o += "> screenshots</label>\n<span id=\"count\"></span>\n</div>\n";

	o += "<table id=\"tests\"";
	if (!any_shot) o += " class=\"hideshots\"";
	o += ">\n<thead><tr><th>#</th><th>Test</th><th>Suite</th><th>Model</th>"
	     "<th>Result</th>";
	if (base) o += "<th>Baseline</th>";
	o += "<th>Frames</th><th>Detail</th><th class=\"shotcol\">Screen</th>";
	if (base) o += "<th class=\"shotcol\">Baseline frame</th>";
	o += "</tr></thead>\n<tbody>\n";

	int n = 0;
	std::string lower;
	for (const ReportRow &r : rows)
	{
		if (!r.test) continue;
		const Test &t = *r.test;
		const char *st = StatusWord(r);
		lower = t.name;
		for (char &c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

		o += "<tr data-name=\"" + Html(lower) + "\" data-suite=\"" +
		     Html(t.suite) + "\" data-model=\"" + ModelName(t.model) +
		     "\" data-status=\"" + st + "\" data-baseline=\"" +
		     MatchName(r.match) + "\">";
		o += Fmt("<td class=\"num\">%d</td>", ++n);
		o += "<td class=\"name\">" + Html(t.name) + "</td>";
		o += "<td>" + Html(t.suite) + "</td>";
		o += Fmt("<td>%s</td>", ModelName(t.model));
		o += Fmt("<td class=\"result %s\">%s</td>", st, StatusTag(r));
		if (base)
		{
			o += Fmt("<td class=\"result %s\">%s", MatchName(r.match),
			         MatchTag(r).c_str());
			if (r.match == Match::Differs)
				o += Fmt(" <span class=\"frames\">%d px</span>", r.diff_px);
			o += "</td>";
		}
		o += Fmt("<td class=\"frames\">%d</td>", r.result ? r.result->frames : 0);
		o += "<td class=\"detail\">" +
		     Html(r.result ? r.result->detail : std::string()) + "</td>";
		o += "<td class=\"shotcol\">" + ShotCell(r.result ? &r.result->shot : nullptr) +
		     "</td>";
		if (base)
		{
			std::vector<uint8_t> ref;
			const bool have = info.baseline &&
			                  LoadBaselineFrame(*info.baseline, t, ref);
			o += "<td class=\"shotcol\">" + ShotCell(have ? &ref : nullptr) + "</td>";
		}
		o += "</tr>\n";
	}

	o += "</tbody>\n</table>\n<script>";
	o += kHtmlScript;
	o += "</script>\n</body>\n</html>\n";
	return o;
}

} // anonymous

std::vector<uint8_t> EncodePng(const uint8_t *rgb, int w, int h)
{
	std::vector<uint8_t> png;
	if (!rgb || w <= 0 || h <= 0) return png;

	const int stride = w * 3;
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(h) * (stride + 1));
	for (int y = 0; y < h; ++y)
		FilterRow(rgb + static_cast<size_t>(y) * stride,
		          y ? rgb + static_cast<size_t>(y - 1) * stride : nullptr,
		          stride, raw);

	std::vector<uint8_t> zlib;
	zlib.push_back(0x78);
	zlib.push_back(0x01);
	Deflate(raw.data(), raw.size(), zlib);
	PutU32(zlib, Adler32(raw.data(), raw.size()));

	static const uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	png.insert(png.end(), kSig, kSig + 8);

	std::vector<uint8_t> ihdr;
	PutU32(ihdr, static_cast<uint32_t>(w));
	PutU32(ihdr, static_cast<uint32_t>(h));
	ihdr.push_back(8);   // 8 bits per channel
	ihdr.push_back(2);   // truecolour RGB
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", zlib);
	Chunk(png, "IEND", std::vector<uint8_t>());
	return png;
}

Format FormatFromPath(const char *path)
{
	if (!path) return Format::Text;
	const char *dot = std::strrchr(path, '.');
	if (!dot) return Format::Text;
	std::string ext(dot + 1);
	for (char &c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	if (ext == "json") return Format::Json;
	if (ext == "html" || ext == "htm") return Format::Html;
	return Format::Text;
}

std::string RenderReport(Format fmt, const std::vector<ReportRow> &rows,
                         const ReportInfo &in)
{
	ReportInfo info = in;
	if (info.generated.empty()) info.generated = Timestamp();
	switch (fmt)
	{
		case Format::Json: return RenderJson(rows, info);
		case Format::Html: return RenderHtml(rows, info);
		default:           return RenderText(rows, info);
	}
}

bool WriteReport(const char *path, Format fmt, const std::vector<ReportRow> &rows,
                 const ReportInfo &info, std::string &err)
{
	const std::string doc = RenderReport(fmt, rows, info);
	// Unqualified: the win32 build force-includes _tfwopen.h, which routes
	// fopen through a macro that std:: would break.
	FILE *f = fopen(path, "wb");
	if (!f)
	{
		err = std::string("cannot write ") + path;
		return false;
	}
	const bool ok = fwrite(doc.data(), 1, doc.size(), f) == doc.size();
	fclose(f);
	if (!ok) err = std::string("short write to ") + path;
	return ok;
}

} // namespace AcidTests
