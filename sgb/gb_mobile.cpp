/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Mobile Adapter GB. Protocol per Hacky's notes with the REON team's
// (GBE+ docs/technical/Mobile_Adapter_GB.txt) and REON's libmobile: the game
// sends 99 66, a header (command, 0, length), the body and a 16-bit sum,
// then its device byte while the adapter answers 88 and command ^ 80. Its
// first 4B after that runs the command and the reply comes back the same way.
// Servers: REON's for Game Boy Wars 3's files and the GB00 challenge.

#include "gb_mobile.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace SGB {

namespace {

constexpr uint8_t kIdle     = 0xD2;     // the adapter, nothing to send
constexpr uint8_t kGbIdle   = 0x4B;     // the Game Boy, listening
constexpr uint8_t kDevice   = 0x88;     // blue (PDC) adapter: 0x08 | 0x80
constexpr uint8_t kBadSum   = 0xF1;
constexpr uint8_t kUnknown  = 0xF0;
constexpr int32_t kSleep    = 3 * 4194304;   // 3 s without a byte: it sleeps
constexpr size_t  kMaxBody  = 0xFF;
constexpr size_t  kChunk    = 0xFE;     // bytes one DATA reply carries
constexpr int     kConns    = 2;
constexpr size_t  kReqCap   = 2048;
constexpr size_t  kOutCap   = 8192;
constexpr size_t  kPathCap  = 512;
constexpr uint32_t kHostIp  = 0xC0000201;    // 192.0.2.1 answers every name
constexpr uint32_t kOwnIp   = 0x7F000001;    // the ISP's address for us

enum Phase : uint8_t { kMagic0, kMagic1, kHeader, kBody, kSum, kAck0, kAck1, kWaitGb, kSend, kSendAck0, kSendAck1 };
enum Line  : uint8_t { kOnHook, kIsp, kOnline };
enum Kind  : uint8_t { kHttp, kPop, kSmtp };

struct Conn
{
	uint8_t     open, closing, kind, stage;
	uint32_t    body_left;      // HTTP: request body still to come
	std::string req;            // the game's bytes not yet handled
	std::string out;            // the server's bytes not yet sent
	std::string file;           // the rest of the answer comes from here
	uint32_t    file_pos, file_left;
};

struct State
{
	uint8_t  phase, out, session, line, err, retries;
	uint16_t pos, len;
	uint8_t  hdr[4], sum[2];
	uint8_t  body[kMaxBody];
	uint8_t  pkt[2 + 4 + kMaxBody + 2];
	uint16_t pkt_len, pkt_pos;
	int32_t  idle;
	uint32_t dns[2];
	uint32_t seed;
	Conn     conn[kConns];
};

State       g_st;
bool        g_connected = false;
uint8_t     g_config[0x100];
bool        g_config_loaded = false;
std::string g_config_path, g_root;
void      (*g_log)(const char *) = nullptr;

void Log(const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

void Log(const char *fmt, ...)
{
	if (!g_log) return;
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	g_log(line);
}

// ----- Config memory -----

void Put(uint8_t *p, const char *s) { std::memcpy(p, s, std::strlen(s)); }

// What Mobile Trainer leaves after registering a blue adapter with DION.
void DefaultConfig(uint8_t *c)
{
	std::memset(c, 0, 0x100);
	Put(c, "MA");
	c[0x02] = 0x81;
	const uint8_t dns[8] = { 210, 196, 3, 183, 210, 141, 112, 163 };
	std::memcpy(c + 0x04, dns, 8);
	Put(c + 0x0C, "g000000000");
	Put(c + 0x2C, "player01@mbgb.dion.ne.jp");
	Put(c + 0x4A, "mail.mbgb.dion.ne.jp");
	Put(c + 0x5E, "pop.mbgb.dion.ne.jp");
	// Slot 1: #9677 in BCD (A = #, F ends it) and its name; 2-3 unused.
	for (int s = 0; s < 3; ++s) std::memset(c + 0x76 + s * 24, 0xFF, 8);
	const uint8_t isp[3] = { 0xA9, 0x67, 0x7F };
	std::memcpy(c + 0x76, isp, 3);
	Put(c + 0x7E, "DION PDC/CDMAONE");
	uint16_t sum = 0;
	for (int i = 0; i < 0xBE; ++i) sum = static_cast<uint16_t>(sum + c[i]);
	c[0xBE] = static_cast<uint8_t>(sum >> 8);
	c[0xBF] = static_cast<uint8_t>(sum);
}

void SaveConfig()
{
	if (g_config_path.empty()) return;
	std::error_code ec;
	const std::filesystem::path p = std::filesystem::u8path(g_config_path);
	if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
	FILE *f = fopen(g_config_path.c_str(), "wb");
	if (!f) return;
	fwrite(g_config, 1, sizeof g_config, f);
	fclose(f);
}

void LoadConfig()
{
	if (g_config_loaded) return;
	g_config_loaded = true;
	FILE *f = g_config_path.empty() ? nullptr : fopen(g_config_path.c_str(), "rb");
	if (f)
	{
		std::memset(g_config, 0, sizeof g_config);
		const size_t n = fread(g_config, 1, sizeof g_config, f);
		fclose(f);
		if (n > 0) return;
	}
	DefaultConfig(g_config);
	SaveConfig();
}

// ----- Packets -----

void Reply(State &st, uint8_t cmd, const uint8_t *d, size_t n)
{
	uint8_t *p = st.pkt;
	p[0] = 0x99; p[1] = 0x66;
	p[2] = static_cast<uint8_t>(cmd | 0x80); p[3] = 0; p[4] = 0; p[5] = static_cast<uint8_t>(n);
	if (n) std::memcpy(p + 6, d, n);
	uint16_t sum = 0;
	for (size_t i = 2; i < 6 + n; ++i) sum = static_cast<uint16_t>(sum + p[i]);
	p[6 + n] = static_cast<uint8_t>(sum >> 8);
	p[7 + n] = static_cast<uint8_t>(sum);
	st.pkt_len = static_cast<uint16_t>(8 + n);
	Log("ma> %02X len %u", p[2], static_cast<unsigned>(n));
}

void Fail(State &st, uint8_t cmd, uint8_t code)
{
	const uint8_t d[2] = { cmd, code };
	Reply(st, 0x6E, d, 2);
}

bool Known(uint8_t cmd)
{
	switch (cmd)
	{
	case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
	case 0x17: case 0x18: case 0x19: case 0x1A: case 0x21: case 0x22: case 0x23: case 0x24:
	case 0x25: case 0x26: case 0x28: case 0x3F:
		return true;
	default:
		return false;
	}
}

// ----- Server -----

void Close(Conn &c)
{
	c = Conn();
}

void CloseAll(State &st)
{
	for (Conn &c : st.conn) Close(c);
}

uint32_t Rand(State &st)
{
	uint32_t r = 0;
	for (int i = 0; i < 2; ++i)
	{
		st.seed = st.seed * 1103515245u + 12345u;
		r = (r << 16) | (st.seed >> 16);
	}
	return r;
}

std::string Base64(const uint8_t *d, size_t n)
{
	static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string s;
	for (size_t i = 0; i < n; i += 3)
	{
		const uint32_t v = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
		s += k[(v >> 18) & 63];
		s += k[(v >> 12) & 63];
		s += i + 1 < n ? k[(v >> 6) & 63] : '=';
		s += i + 2 < n ? k[v & 63] : '=';
	}
	return s;
}

std::string Lower(std::string s)
{
	for (char &ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
	return s;
}

// A request header's value, or "" (headers = the lines after the first).
std::string Header(const std::string &req, const char *name)
{
	const std::string want = Lower(name) + ":";
	size_t at = req.find("\r\n");
	while (at != std::string::npos && at + 2 < req.size())
	{
		const size_t from = at + 2;
		const size_t end  = req.find("\r\n", from);
		const std::string line = req.substr(from, end == std::string::npos ? std::string::npos : end - from);
		if (Lower(line.substr(0, want.size())) == want)
		{
			size_t v = want.size();
			while (v < line.size() && line[v] == ' ') ++v;
			return line.substr(v);
		}
		at = end;
	}
	return "";
}

std::string UrlDecode(const std::string &s)
{
	std::string r;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size())
		{
			r += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
			i += 2;
		}
		else r += s[i] == '+' ? ' ' : s[i];
	}
	return r;
}

// A name under the server root: no way out of it.
bool SafeName(const std::string &n)
{
	return !n.empty() && n[0] == '/' && n.find("..") == std::string::npos &&
	       n.find('\\') == std::string::npos && n.find(':') == std::string::npos;
}

bool FileSize(const std::string &path, uint32_t &size)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	if (n < 0) return false;
	size = static_cast<uint32_t>(n);
	return true;
}

// An HTTP/1.0 answer; the server hangs up once it's out. A file's bytes are
// read as they go, so any size fits the connection's buffer.
void Respond(Conn &c, int code, const std::string &extra, const std::string &body,
             const std::string &file = std::string(), uint32_t file_size = 0)
{
	const char *text = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Bad Request";
	const std::string &named = file.empty() ? body : file;
	const bool txt = named.size() >= 4 && !named.compare(named.size() - 4, 4, ".txt");
	char date[64];
	const time_t now = time(nullptr);
	strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
	char head[256];
	snprintf(head, sizeof head, "HTTP/1.0 %d %s\r\nDate: %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n",
	         code, text, date, file.empty() || txt ? "text/plain" : "application/octet-stream",
	         static_cast<unsigned>(file.empty() ? body.size() : file_size));
	c.out = head + extra + "\r\n" + body;
	if (c.out.size() > kOutCap) c.out.resize(kOutCap);
	c.file      = file.size() < kPathCap ? file : std::string();
	c.file_pos  = 0;
	c.file_left = c.file.empty() ? 0 : file_size;
	c.closing   = 1;
	Log("http %d, %u bytes", code, static_cast<unsigned>(file.empty() ? body.size() : file_size));
}

// A map file's number from "map_<digits>.cgb", or -1.
int MapNumber(const std::string &base)
{
	if (base.size() < 9 || base.compare(0, 4, "map_") || base.compare(base.size() - 4, 4, ".cgb")) return -1;
	const std::string digits = base.substr(4, base.size() - 8);
	if (digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
	return std::atoi(digits.c_str());
}

// Game Boy Wars 3 asks for map 1 as map/0/map_1.cgb; REON names it
// map_0001.cgb. Either name on disk answers either request.
std::string MapAlias(const std::string &name)
{
	const size_t slash = name.rfind('/');
	const std::string base = name.substr(slash + 1);
	const int n = MapNumber(base);
	if (n < 0) return std::string();
	char alt[16];
	snprintf(alt, sizeof alt, base.size() == 12 ? "map_%d.cgb" : "map_%04d.cgb", n);
	return name.substr(0, slash + 1) + alt;
}

uint32_t ByteSum(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return 0;
	uint32_t sum = 0;
	for (int ch; (ch = fgetc(f)) != EOF; ) sum += static_cast<uint32_t>(ch);
	fclose(f);
	return sum;
}

// The server's answer for a Mobile System GB path it has no file for:
// Game Boy Wars 3's menus, made from the maps and mail on disk with REON's
// formats and prices.
bool BuiltIn(const std::string &name, std::string &body)
{
	if (name.compare(0, 13, "/18/CGB-BWWJ/") && name.compare(0, 13, "/18/CGB-BWWE/")) return false;
	const std::string dir  = g_root + "/download" + name.substr(0, 13);
	const std::string rest = name.substr(13);
	if (rest == "0.map_menu.txt")
	{
		// One line per run of maps: first, last, price in yen.
		std::vector<int> maps;
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(std::filesystem::u8path(dir + "map"), ec), end;
		     !ec && it != end; it.increment(ec))
		{
			const int n = MapNumber(it->path().filename().u8string());
			if (n > 0) maps.push_back(n);
		}
		std::sort(maps.begin(), maps.end());
		maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
		for (size_t i = 0; i < maps.size(); )
		{
			size_t j = i;
			while (j + 1 < maps.size() && maps[j + 1] == maps[j] + 1) ++j;
			char line[32];
			snprintf(line, sizeof line, "%04d\t%04d\t0010\n", maps[i], maps[j]);
			body += line;
			i = j + 1;
		}
		return true;
	}
	if (rest == "0.youhei_menu.txt")
	{
		body = "0030\n0050\n0050\n0050\n0050\n";
		return true;
	}
	if (!rest.compare(0, 7, "charge/") && rest.size() > 18 && !rest.compare(rest.size() - 11, 11, ".charge.cgb"))
		return true;
	if (rest == "mbox/mbox_serial.txt")
	{
		// A serial per mailbox: 0000 when empty, else one its message
		// changes, so the game fetches it again after an edit.
		for (int i = 0; i < 16; ++i)
		{
			char file[32], line[8];
			snprintf(file, sizeof file, "mbox/mbox_%02d.cgb", i);
			const uint32_t sum = ByteSum(dir + file);
			snprintf(line, sizeof line, "%s%04u", i ? "\n" : "", sum ? 1 + sum % 9999 : 0);
			body += line;
		}
		return true;
	}
	return false;
}

// Mobile System GB bills a file whose name starts with a number, and asks for
// the GB00 login first (REON's download.php).
bool Billed(const std::string &name)
{
	const size_t slash = name.rfind('/');
	const std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
	size_t i = 0;
	while (i < base.size() && base[i] >= '0' && base[i] <= '9') ++i;
	return i > 0 && i < base.size() && base[i] == '.';
}

void HttpAnswer(State &st, Conn &c)
{
	const std::string &req = c.req;
	const size_t sp1 = req.find(' ');
	const size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
	if (sp2 == std::string::npos) { Respond(c, 400, "", ""); return; }
	const std::string method = req.substr(0, sp1);
	const std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
	const size_t q = target.find('?');
	const std::string path  = target.substr(0, q);
	const std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);
	Log("http %s %s", method.c_str(), target.c_str());

	if (path == "/cgb/upload")
	{
		Respond(c, 200, "", "");
		return;
	}

	std::string name, file;
	if (path == "/cgb/download" && !query.compare(0, 5, "name="))
	{
		name = UrlDecode(query.substr(5));
		file = g_root + "/download" + name;
	}
	else
	{
		name = UrlDecode(path);
		file = g_root + "/www" + name;
	}
	if (!SafeName(name)) { Respond(c, 404, "", ""); return; }

	std::string extra;
	if (Billed(name))
	{
		const std::string id = Header(req, "Gb-Auth-ID");
		const std::string auth = Header(req, "Authorization");
		if (id.empty() && auth.compare(0, 10, "GB00 name="))
		{
			uint8_t r[36];
			for (uint8_t &b : r) b = static_cast<uint8_t>(Rand(st));
			Respond(c, 401, "WWW-Authenticate: GB00 name=\"" + Base64(r, sizeof r) + "\"\r\n", "");
			return;
		}
		char sid[40];
		snprintf(sid, sizeof sid, "%08X%08X%08X%08X", Rand(st), Rand(st), Rand(st), Rand(st));
		extra = "Gb-Auth-ID: " + (id.empty() ? std::string(sid) : id) + "\r\n";
	}

	uint32_t size = 0;
	std::string body;
	const std::string alias = path == "/cgb/download" ? MapAlias(name) : std::string();
	if (!FileSize(file, size) && !alias.empty() && FileSize(g_root + "/download" + alias, size))
		file = g_root + "/download" + alias;
	if (FileSize(file, size))
		Respond(c, 200, extra, "", file, size);
	else if (path == "/cgb/download" && BuiltIn(name, body))
		Respond(c, 200, extra, body);
	else
		Respond(c, 404, "", "");
}

void HttpFeed(State &st, Conn &c, const uint8_t *p, size_t n)
{
	if (g_log)
	{
		std::string s;
		for (size_t i = 0; i < n; ++i)
		{
			char e[8];
			if (p[i] >= 0x20 && p[i] < 0x7F && p[i] != '\\') s += static_cast<char>(p[i]);
			else { snprintf(e, sizeof e, "\\x%02X", p[i]); s += e; }
		}
		Log("http< %s", s.c_str());
	}
	if (c.stage == 0)
	{
		c.req.append(reinterpret_cast<const char *>(p), n);
		const size_t end = c.req.find("\r\n\r\n");
		if (end == std::string::npos)
		{
			if (c.req.size() > kReqCap) Respond(c, 400, "", "");
			return;
		}
		const size_t got = c.req.size() - (end + 4);
		c.req.resize(end + 2);
		const long length = std::atol(Header(c.req, "Content-Length").c_str());
		c.body_left = length > 0 ? static_cast<uint32_t>(length) : 0;
		c.body_left -= std::min<uint32_t>(c.body_left, static_cast<uint32_t>(got));
		c.stage = 1;
	}
	else
		c.body_left -= std::min<uint32_t>(c.body_left, static_cast<uint32_t>(n));
	if (c.body_left == 0) HttpAnswer(st, c);
}

// POP3 with an empty mailbox; SMTP that takes mail and drops it.
void LineCommand(Conn &c, const std::string &line)
{
	const std::string verb = Lower(line.substr(0, line.find(' ')));
	if (c.kind == kPop)
	{
		if (verb == "quit") { c.out += "+OK\r\n"; c.closing = 1; }
		else if (verb == "stat") c.out += "+OK 0 0\r\n";
		else if (verb == "list" || verb == "uidl") c.out += line.size() > verb.size() ? "-ERR no such message\r\n" : "+OK\r\n.\r\n";
		else if (verb == "user" || verb == "pass" || verb == "noop" || verb == "rset" || verb == "apop") c.out += "+OK\r\n";
		else c.out += "-ERR\r\n";
		return;
	}
	if (verb == "quit") { c.out += "221 Bye\r\n"; c.closing = 1; }
	else if (verb == "data") { c.out += "354 End data with <CR><LF>.<CR><LF>\r\n"; c.stage = 1; c.req = "\r\n"; }
	else if (verb == "helo" || verb == "ehlo" || verb == "mail" || verb == "rcpt" || verb == "rset" || verb == "noop")
		c.out += "250 OK\r\n";
	else c.out += "502 Not implemented\r\n";
}

void LineFeed(Conn &c, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n && !c.closing; ++i)
	{
		c.req += static_cast<char>(p[i]);
		if (c.stage == 1)
		{
			// SMTP DATA: only the end mark matters (DATA's own line end
			// starts it, so a lone "." line ends an empty mail too).
			if (c.req.size() > 5) c.req.erase(0, c.req.size() - 5);
			if (c.req == "\r\n.\r\n")
			{
				c.out += "250 OK\r\n";
				c.stage = 0;
				c.req.clear();
			}
			continue;
		}
		if (p[i] != '\n') { if (c.req.size() > kReqCap) c.req.clear(); continue; }
		std::string line = c.req;
		c.req.clear();
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
		Log("%s %s", c.kind == kPop ? "pop" : "smtp", line.c_str());
		LineCommand(c, line);
	}
}

void Feed(State &st, Conn &c, const uint8_t *p, size_t n)
{
	if (c.closing || n == 0) return;
	if (c.kind == kHttp) HttpFeed(st, c, p, n);
	else LineFeed(c, p, n);
}

// Tops up the bytes waiting for the game from the file being served.
void Refill(Conn &c)
{
	if (!c.file_left || c.out.size() >= kOutCap) return;
	FILE *f = fopen(c.file.c_str(), "rb");
	if (!f) { c.file_left = 0; return; }
	char buf[kOutCap];
	const size_t want = std::min<size_t>(kOutCap - c.out.size(), c.file_left);
	fseek(f, static_cast<long>(c.file_pos), SEEK_SET);
	const size_t got = fread(buf, 1, want, f);
	fclose(f);
	c.out.append(buf, got);
	c.file_pos  += static_cast<uint32_t>(got);
	c.file_left  = got < want ? 0 : c.file_left - static_cast<uint32_t>(got);
}

// ----- Commands -----

bool IspNumber(const std::string &n)
{
	return n == "#9677" || n == "#9477" || n == "0077487751" || n == "0077487752" || n == "0755311973";
}

uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void     PutBe32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

void Data(State &st, uint8_t cmd, const uint8_t *d, size_t n)
{
	if (st.line != kOnline) { Fail(st, cmd, 1); return; }
	if (n < 1 || d[0] >= kConns || !st.conn[d[0]].open) { Fail(st, cmd, 0); return; }
	Conn &c = st.conn[d[0]];
	Refill(c);
	const size_t take = std::min(kChunk, c.out.size());
	if (take == 0 && c.closing && !c.file_left)
	{
		// The server hung up and everything it sent is in.
		const uint8_t id = d[0];
		Close(c);
		Reply(st, 0x1F, &id, 1);
		return;
	}
	uint8_t r[1 + kChunk];
	r[0] = d[0];
	std::memcpy(r + 1, c.out.data(), take);
	c.out.erase(0, take);
	Reply(st, cmd, r, 1 + take);
	// What the server says to these bytes goes back on the next DATA.
	Feed(st, c, d + 1, n - 1);
}

void Run(State &st)
{
	const uint8_t  cmd = st.hdr[0];
	const uint8_t *d   = st.body;
	const size_t   n   = st.len;
	Log("ma< %02X len %u", cmd, static_cast<unsigned>(n));
	switch (cmd)
	{
	case 0x0F:
		Reply(st, cmd, nullptr, 0);
		break;
	case 0x10:
		if (st.session) { Fail(st, cmd, 1); break; }
		if (n != 8 || std::memcmp(d, "NINTENDO", 8)) { Fail(st, cmd, 2); break; }
		st.session = 1;
		Reply(st, cmd, d, n);
		break;
	case 0x11:
		CloseAll(st);
		st.line = kOnHook;
		st.session = 0;
		Reply(st, cmd, nullptr, 0);
		break;
	case 0x16:
		CloseAll(st);
		st.line = kOnHook;
		Reply(st, cmd, nullptr, 0);
		break;
	case 0x17:
	{
		const uint8_t r[3] = { static_cast<uint8_t>(st.line == kOnHook ? 0 : 4), 0x4D, 0x00 };
		Reply(st, cmd, r, 3);
		break;
	}
	case 0x18:
		if (n != 1 || d[0] > 1) Fail(st, cmd, 2);
		else Reply(st, cmd, nullptr, 0);
		break;
	case 0x19:
	{
		if (n != 2 || d[1] > 0x80 || d[0] + d[1] > 0x100) { Fail(st, cmd, 2); break; }
		LoadConfig();
		uint8_t r[1 + 0x80];
		r[0] = d[0];
		std::memcpy(r + 1, g_config + d[0], d[1]);
		Reply(st, cmd, r, 1 + d[1]);
		break;
	}
	case 0x1A:
	{
		if (n < 1 || n - 1 > 0x80 || d[0] + (n - 1) > 0x100) { Fail(st, cmd, 2); break; }
		LoadConfig();
		std::memcpy(g_config + d[0], d + 1, n - 1);
		SaveConfig();
		const uint8_t r[2] = { d[0], static_cast<uint8_t>(n - 1) };
		Reply(st, cmd, r, 2);
		break;
	}
	case 0x12:
	{
		if (st.line != kOnHook) { Fail(st, cmd, 1); break; }
		if (n < 1 || d[0] != 0) { Fail(st, cmd, 2); break; }   // the blue adapter dials PDC
		std::string number;
		for (size_t i = 1; i < n && i <= 0x20; ++i)
			if ((d[i] >= '0' && d[i] <= '9') || d[i] == '#' || d[i] == '*') number += static_cast<char>(d[i]);
		Log("dial %s", number.c_str());
		// Only the ISP answers: there is no other adapter on this line.
		if (!IspNumber(number)) { Fail(st, cmd, 3); break; }
		st.line = kIsp;
		Reply(st, cmd, nullptr, 0);
		break;
	}
	case 0x13:
		if (st.line == kOnHook) { Fail(st, cmd, 1); break; }
		CloseAll(st);
		st.line = kOnHook;
		Reply(st, cmd, nullptr, 0);
		break;
	case 0x14:
		Fail(st, cmd, 0);
		break;
	case 0x15:
		Data(st, cmd, d, n);
		break;
	case 0x21:
	{
		if (st.line != kIsp) { Fail(st, cmd, 1); break; }
		size_t i = 0;
		if (i < n) i += 1 + d[i];
		if (i < n) i += 1 + d[i];
		if (i + 8 > n) { Fail(st, cmd, 2); break; }
		LoadConfig();
		for (int k = 0; k < 2; ++k)
		{
			const uint32_t want = Be32(d + i + 4 * k);
			st.dns[k] = want ? want : Be32(g_config + 4 + 4 * k);
		}
		st.line = kOnline;
		uint8_t r[12];
		PutBe32(r, kOwnIp);
		PutBe32(r + 4, st.dns[0]);
		PutBe32(r + 8, st.dns[1]);
		Reply(st, cmd, r, 12);
		break;
	}
	case 0x22:
		if (st.line != kOnline) { Fail(st, cmd, st.line == kOnHook ? 1 : 0); break; }
		CloseAll(st);
		st.line = kIsp;
		Reply(st, cmd, nullptr, 0);
		break;
	case 0x23:
	{
		if (st.line != kOnline) { Fail(st, cmd, 1); break; }
		if (n != 6) { Fail(st, cmd, 3); break; }
		int id = 0;
		while (id < kConns && st.conn[id].open) ++id;
		if (id == kConns) { Fail(st, cmd, 0); break; }
		const uint16_t port = static_cast<uint16_t>((d[4] << 8) | d[5]);
		Log("tcp %u.%u.%u.%u:%u", d[0], d[1], d[2], d[3], port);
		if (port != 80 && port != 110 && port != 25) { Fail(st, cmd, 3); break; }
		Conn &c = st.conn[id];
		Close(c);
		c.open = 1;
		c.kind = port == 80 ? kHttp : port == 110 ? kPop : kSmtp;
		if (c.kind == kPop)  c.out = "+OK POP3 ready\r\n";
		if (c.kind == kSmtp) c.out = "220 SMTP ready\r\n";
		const uint8_t r = static_cast<uint8_t>(id);
		Reply(st, cmd, &r, 1);
		break;
	}
	case 0x24:
		if (st.line != kOnline) { Fail(st, cmd, 1); break; }
		if (n != 1 || d[0] >= kConns || !st.conn[d[0]].open) { Fail(st, cmd, 0); break; }
		Close(st.conn[d[0]]);
		Reply(st, cmd, d, 1);
		break;
	case 0x25:
		Fail(st, cmd, st.line != kOnline ? 1 : 3);
		break;
	case 0x26:
		Fail(st, cmd, st.line != kOnline ? 1 : 0);
		break;
	case 0x28:
	{
		if (st.line != kOnline) { Fail(st, cmd, 1); break; }
		std::string host;
		for (size_t i = 0; i < n && d[i]; ++i) host += static_cast<char>(d[i]);
		unsigned a, b, c2, e;
		char tail;
		uint32_t ip = kHostIp;
		if (sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c2, &e, &tail) == 4 && a < 256 && b < 256 && c2 < 256 && e < 256)
			ip = (a << 24) | (b << 16) | (c2 << 8) | e;
		Log("dns %s", host.c_str());
		uint8_t r[4];
		PutBe32(r, ip);
		Reply(st, cmd, r, 4);
		break;
	}
	default:   // 0x3F: test mode, not on a retail adapter's menu
		Fail(st, cmd, 1);
		break;
	}
}

void Sleep(State &st)
{
	CloseAll(st);
	st.line    = kOnHook;
	st.session = 0;
	st.phase   = kMagic0;
	st.out     = kIdle;
}

// ----- Savestate -----

struct Io
{
	uint8_t       *w;
	const uint8_t *r;
	size_t         pos;
	void Bytes(void *p, size_t n)
	{
		if (w) std::memcpy(w + pos, p, n);
		else if (r) std::memcpy(p, r + pos, n);
		pos += n;
	}
	template <typename T> void Field(T &v) { Bytes(&v, sizeof v); }
	void Str(std::string &s, size_t cap)
	{
		uint16_t n = static_cast<uint16_t>(std::min(s.size(), cap));
		Field(n);
		std::vector<char> buf(cap, 0);
		if (w) std::memcpy(buf.data(), s.data(), n);
		Bytes(buf.data(), cap);
		if (r) s.assign(buf.data(), std::min<size_t>(n, cap));
	}
};

void Visit(Io &io, State &st)
{
	io.Field(st.phase); io.Field(st.out); io.Field(st.session); io.Field(st.line);
	io.Field(st.err); io.Field(st.retries); io.Field(st.pos); io.Field(st.len);
	io.Bytes(st.hdr, sizeof st.hdr); io.Bytes(st.sum, sizeof st.sum);
	io.Bytes(st.body, sizeof st.body); io.Bytes(st.pkt, sizeof st.pkt);
	io.Field(st.pkt_len); io.Field(st.pkt_pos); io.Field(st.idle);
	io.Field(st.dns[0]); io.Field(st.dns[1]); io.Field(st.seed);
	for (Conn &c : st.conn)
	{
		io.Field(c.open); io.Field(c.closing); io.Field(c.kind); io.Field(c.stage);
		io.Field(c.body_left);
		io.Str(c.req, kReqCap + kChunk);
		io.Str(c.out, kOutCap);
		io.Str(c.file, kPathCap);
		io.Field(c.file_pos); io.Field(c.file_left);
	}
}

} // namespace

void MobileConnect(bool on)
{
	if (on == g_connected) return;
	g_connected = on;
	Sleep(g_st);
	g_st.idle = 0;
	if (on) g_config_loaded = false;
}

bool MobileConnected() { return g_connected; }

void MobileSetPaths(const char *config, const char *root)
{
	g_config_path   = config ? config : "";
	g_root          = root ? root : "";
	g_config_loaded = false;
}

uint8_t MobileExchange(uint8_t in)
{
	State &st = g_st;
	const uint8_t back = st.out;
	st.idle = 0;
	st.out  = kIdle;
	switch (st.phase)
	{
	case kMagic0:
		if (in == 0x99) st.phase = kMagic1;
		break;
	case kMagic1:
		if (in == 0x66) { st.phase = kHeader; st.pos = 0; }
		else if (in != 0x99) st.phase = kMagic0;
		break;
	case kHeader:
		st.hdr[st.pos++] = in;
		if (st.pos < 4) break;
		st.len = static_cast<uint16_t>((st.hdr[2] << 8) | st.hdr[3]);
		st.pos = 0;
		if (st.len > kMaxBody) { st.phase = kMagic0; break; }   // too long: dropped
		st.phase = st.len ? kBody : kSum;
		break;
	case kBody:
		st.body[st.pos++] = in;
		if (st.pos == st.len) { st.phase = kSum; st.pos = 0; }
		break;
	case kSum:
		st.sum[st.pos++] = in;
		if (st.pos < 2) break;
		// Asleep, it hears nothing but a session start.
		if (!st.session && st.hdr[0] != 0x10) { st.phase = kMagic0; break; }
		st.phase = kAck0;
		st.out   = kDevice;
		break;
	case kAck0:
	{
		uint16_t sum = 0;
		for (int i = 0; i < 4; ++i) sum = static_cast<uint16_t>(sum + st.hdr[i]);
		for (int i = 0; i < st.len; ++i) sum = static_cast<uint16_t>(sum + st.body[i]);
		st.err   = sum != ((st.sum[0] << 8) | st.sum[1]) ? kBadSum : !Known(st.hdr[0]) ? kUnknown : 0;
		st.out   = st.err ? st.err : static_cast<uint8_t>(st.hdr[0] ^ 0x80);
		st.phase = kAck1;
		break;
	}
	case kAck1:
		st.phase = st.err ? kMagic0 : kWaitGb;
		break;
	case kWaitGb:
		// The Game Boy turned to listening: the command runs now.
		if (in != kGbIdle) break;
		Run(st);
		st.retries = 0;
		st.pkt_pos = 0;
		st.phase   = kSend;
		st.out     = st.pkt[0];
		break;
	case kSend:
		if (++st.pkt_pos < st.pkt_len) st.out = st.pkt[st.pkt_pos];
		else { st.phase = kSendAck0; st.out = kDevice; }
		break;
	case kSendAck0:
		st.phase = kSendAck1;
		st.out   = 0x00;
		break;
	case kSendAck1:
		// F0-F2: the game didn't take it; send it again.
		if (in >= 0xF0 && in <= 0xF2 && st.retries < 4)
		{
			++st.retries;
			st.pkt_pos = 0;
			st.phase   = kSend;
			st.out     = st.pkt[0];
			break;
		}
		st.phase = kMagic0;
		break;
	}
	return back;
}

uint8_t MobileReply() { return g_st.out; }

void MobileClock(int32_t cycles)
{
	State &st = g_st;
	if (!st.session && st.phase == kMagic0) return;
	st.idle += cycles;
	if (st.idle >= kSleep)
	{
		Log("asleep");
		Sleep(st);
		st.idle = 0;
	}
}

size_t MobileStateBytes()
{
	static size_t n = 0;
	if (!n)
	{
		State st;
		Io io{ nullptr, nullptr, 0 };
		Visit(io, st);
		n = io.pos;
	}
	return n;
}

void MobileStateSave(uint8_t *out)
{
	Io io{ out, nullptr, 0 };
	Visit(io, g_st);
}

void MobileStateLoad(const uint8_t *in)
{
	Io io{ nullptr, in, 0 };
	Visit(io, g_st);
	// Indexes a damaged state could push past their buffers.
	State &st = g_st;
	if (st.phase > kSendAck1 || st.len > kMaxBody || st.pos > kMaxBody ||
	    st.pkt_len > sizeof st.pkt || st.pkt_pos > st.pkt_len)
		Sleep(st);
}

void MobileSetLog(void (*log)(const char *line)) { g_log = log; }

} // namespace SGB
