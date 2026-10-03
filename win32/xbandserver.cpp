/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// XBAND local server, a port of s9x-harness/xbserver (xbserver.py, reply_xband.py, reply_news.py,
// switchboard.py, adsp.py, games.py). Box side of the protocol: xband.cpp.

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define close_sock(s) closesocket(s)
  #define SEND_FLAGS 0
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define close_sock(s) close(s)
  #ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL
  #else
  #define SEND_FLAGS 0
  #endif
#endif

#include "xbandserver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

typedef std::vector<uint8_t> Bytes;

static const uint16_t SRV_CONN = 0x0539;
static const uint16_t WINDOW   = 0x0400;
static const size_t   CHUNK    = 100;		// ServerTalk bytes per data frame
static const double   IDLE     = 1.5;		// seconds of upload silence that end the console's turn
static const double   RETRY    = 2.0;		// seconds without an ack before resending
static const double   RATE     = 240.0;		// reply bytes/s like a real 2400 bps line; unpaced, big patches stall
static const int      TIMEOUT_MINUTES = 10;

static double now_s (void)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Server state, log
// ---------------------------------------------------------------------------

static std::atomic<bool> s_running(false);
static sock_t            s_listen = BAD_SOCK;
static std::thread       s_accept;
static int               s_port = 0;
static std::string       s_dir;
static double            s_t0 = 0;
static std::mutex        s_socks_lock;
static std::set<sock_t>  s_socks;		// open connections, closed on stop
static std::mutex        s_log_lock;
static FILE             *s_log = NULL;

static void logf (const char *fmt, ...)
{
	std::lock_guard<std::mutex> g(s_log_lock);
	if (!s_log)
		return;
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(s_log, "[%7.2f] %s\n", now_s() - s_t0, msg);
	fflush(s_log);
}

static void track (sock_t s, bool add)
{
	std::lock_guard<std::mutex> g(s_socks_lock);
	if (add)
		s_socks.insert(s);
	else
		s_socks.erase(s);
}

static bool send_all (sock_t s, const uint8_t *p, size_t n)
{
	while (n)
	{
		const int k = send(s, (const char *) p, (int) n, SEND_FLAGS);
		if (k <= 0)
			return false;
		p += k;
		n -= k;
	}
	return true;
}

static bool send_all (sock_t s, const Bytes &b)       { return send_all(s, b.data(), b.size()); }
static bool send_str (sock_t s, const char *text)     { return send_all(s, (const uint8_t *) text, strlen(text)); }

// Readable within the timeout (1), timeout (0), error (-1).
static int wait_readable (sock_t s, double seconds)
{
	fd_set r;
	FD_ZERO(&r);
	FD_SET(s, &r);
	timeval tv = { (long) seconds, (long) ((seconds - (long) seconds) * 1e6) };
	return select((int) s + 1, &r, NULL, NULL, &tv);
}

// ---------------------------------------------------------------------------
// ADSP framing (adsp.py): wire = 00 + stuff(body + crc16) + 10 03, body = 13-byte header + payload
// ---------------------------------------------------------------------------

static uint16_t crc_false (const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < n; i++)
	{
		crc ^= p[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
	}
	return crc ^ 0xFFFF;
}

static void put16 (Bytes &b, uint32_t v) { b.push_back(v >> 8); b.push_back(v & 0xFF); }
static void put32 (Bytes &b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
static void append (Bytes &b, const Bytes &x) { b.insert(b.end(), x.begin(), x.end()); }
static uint32_t get32 (const uint8_t *p) { return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static Bytes adsp_build (uint16_t conn, uint32_t seq, uint32_t recv, uint16_t window, uint8_t desc,
                         const uint8_t *payload = NULL, size_t len = 0)
{
	Bytes body;
	put16(body, conn);
	put32(body, seq);
	put32(body, recv);
	put16(body, window);
	body.push_back(desc);
	body.insert(body.end(), payload, payload + len);
	Bytes crc_in(1, 0);
	append(crc_in, body);
	put16(body, crc_false(crc_in.data(), crc_in.size()));
	Bytes wire(1, 0);
	for (uint8_t c : body)
	{
		wire.push_back(c);
		if (c == 0x10)
			wire.push_back(0x10);
	}
	wire.push_back(0x10);
	wire.push_back(0x03);
	return wire;
}

struct Frame { uint16_t conn; uint32_t seq, recv; uint16_t window; uint8_t desc; Bytes payload; bool crc_ok; };

// Pulls frame bodies out of the byte stream; stray bytes between frames are skipped.
class Deframer
{
public:
	std::vector<Bytes> feed (const uint8_t *data, size_t n)
	{
		buf.insert(buf.end(), data, data + n);
		std::vector<Bytes> out;
		for (;;)
		{
			size_t start = 0;
			while (start < buf.size() && buf[start] != 0)
				start++;
			if (start == buf.size())
			{
				buf.clear();
				return out;
			}
			size_t i = start + 1;
			Bytes body;
			bool done = false;
			while (i < buf.size())
			{
				const uint8_t b = buf[i];
				if (b == 0x10)
				{
					if (i + 1 >= buf.size())
						break;
					const uint8_t nx = buf[i + 1];
					if (nx == 0x10) { body.push_back(0x10); i += 2; continue; }
					if (nx == 0x03) { done = true; i += 2; break; }
				}
				body.push_back(b);
				i++;
			}
			if (!done)
			{
				buf.erase(buf.begin(), buf.begin() + start);
				return out;
			}
			buf.erase(buf.begin(), buf.begin() + i);
			out.push_back(body);
		}
	}

private:
	Bytes buf;
};

static bool adsp_parse (const Bytes &body, Frame &f)
{
	if (body.size() < 15)
		return false;
	const uint8_t *p = body.data();
	f.conn   = p[0] << 8 | p[1];
	f.seq    = get32(p + 2);
	f.recv   = get32(p + 6);
	f.window = p[10] << 8 | p[11];
	f.desc   = p[12];
	f.payload.assign(body.begin() + 13, body.end() - 2);
	Bytes crc_in(1, 0);
	crc_in.insert(crc_in.end(), body.begin(), body.end() - 2);
	f.crc_ok = (uint16_t) (p[body.size() - 2] << 8 | p[body.size() - 1]) == crc_false(crc_in.data(), crc_in.size());
	return true;
}

// ---------------------------------------------------------------------------
// Switchboard (switchboard.py): the matchmaking queue and the phone lines
// ---------------------------------------------------------------------------

struct Event
{
	std::mutex m;
	std::condition_variable cv;
	bool set_ = false;
	void set (void) { { std::lock_guard<std::mutex> g(m); set_ = true; } cv.notify_all(); }
	bool is_set (void) { std::lock_guard<std::mutex> g(m); return set_; }
	void wait (double seconds = -1)
	{
		std::unique_lock<std::mutex> g(m);
		if (seconds < 0)
			cv.wait(g, [this] { return set_; });
		else
			cv.wait_for(g, std::chrono::duration<double>(seconds), [this] { return set_; });
	}
};

struct RingLine { sock_t conn; Event bridged, released, done; };
struct Waiter { uint32_t line, cookie, rnd; double until; };

static std::mutex                                     sb_lock;
static std::map<uint32_t, Waiter>                     sb_waiting;		// gameID -> waiting line
static std::map<uint32_t, uint32_t>                   sb_pending;		// dialer line -> waiter line
static std::map<uint32_t, std::shared_ptr<RingLine>>  sb_ring_lines;	// line -> idle line

// ---------------------------------------------------------------------------
// Games (games.py): the name the box shows; $A8/$AA are the box font's (R)/(TM)
// ---------------------------------------------------------------------------

static const struct { uint32_t id; const char *name; } kGames[] =
{
	{ 0x94b564b5, "DOOM\xaa" },
	{ 0xa8973c8c, "Ken Griffey Baseball\xaa" },
	{ 0xdf5aa2e2, "Ken Griffey Baseball-A\xaa" },
	{ 0x2d17c045, "Killer Instinct\xaa" },
	{ 0x83e627ef, "Kirby's Avalanche\xa8" },
	{ 0xb8958396, "Madden '95\xa8" },
	{ 0x085d3cdb, "Madden '96\xa8" },
	{ 0xb11f972e, "Mortal Kombat\xa8 II" },
	{ 0xc0432172, "New Mortal Kombat\xa8 II" },
	{ 0x05484971, "Mortal Kombat\xa8 3" },
	{ 0x1969d2af, "NBA\xa8 JAM\xaa TE" },
	{ 0x127e8181, "NHL\xa8 '95" },
	{ 0x25f372a5, "NHL\xa8 '96" },
	{ 0x3d1c44eb, "Super Mario Kart\xaa" },
	{ 0xef120a61, "Super Street Fighter II\xa8" },
	{ 0x0572a585, "WeaponLord\xaa" },
	{ 0x0572dd87, "WeaponLord v2\xaa" },
	{ 0x972404cc, "FIFA\xa8 Int'l Soccer\xaa" },
	{ 0x19a2c936, "NBA\xa8 Live '95\xaa" },
	{ 0x539fdaa0, "Cheez Chat\xa8" },
	{ 0xd8222103, "Super Street Fighter II" },
	{ 0x0a2c238a, "Super Mario Kart" },
	{ 0x8442c640, "Super Puyo Puyo" },
	{ 0xb4217c39, "Super Puyo Puyo v2" },
	{ 0xa4003228, "Super Puyo Puyo v3" },
	{ 0x925b41fc, "Super Fire ProWrestling X" },
	{ 0x1e884202, "Super Fire ProWrestling X Premium" },
	{ 0xa1063a90, "Super Puyo Puyo 2" },
	{ 0x88e60cd8, "Super Puyo Puyo 2 Remix" },
	{ 0x6fc3bf9a, "Super Famista 5" },
	{ 0xd249ca77, "Othello World" },
	{ 0x72aff4ce, "Panel de Pon" },
	{ 0xff338f83, "Super Street Fighter II\xa8" },
	{ 0xff336888, "Super Street Fighter II\xa8" },
	{ 0x0cdaac59, "Super Street Fighter II\xa8" },
	{ 0x0cda855e, "Super Street Fighter II\xa8" },
	{ 0xd822fa07, "Super Street Fighter II" },
	{ 0x644cb852, "Super Mario Kart\xaa" },
	{ 0x3d367a75, "Killer Instinct\xaa" },
	{ 0xe5417c89, "WeaponLord\xaa" },
	{ 0x1c0b8d96, "Madden '95\xa8" },
};

#include "xbandgames.h"

static std::string game_name (uint32_t id)
{
	for (const auto &g : kGames)
		if (g.id == id)
			return g.name;
	const auto t = std::lower_bound(std::begin(kGameTitles), std::end(kGameTitles), id,
	                                [](const decltype(kGameTitles[0]) &g, uint32_t v) { return g.id < v; });
	if (t != std::end(kGameTitles) && t->id == id)
		return t->name;
	char unknown[32];
	snprintf(unknown, sizeof(unknown), "Unknown game 0x%08x", (unsigned) id);
	return unknown;
}

static std::string plain (const std::string &s)
{
	std::string out;
	for (char c : s)
		if (!((uint8_t) c & 0x80))
			out += c;
	return out;
}

// ---------------------------------------------------------------------------
// Patches: patches/<anything>, each a preformed msGamePatch message; newest version per game
// ---------------------------------------------------------------------------

struct Patch { int32_t version; std::string file; Bytes msg; };

static std::vector<std::string> list_dir (const std::string &dir)
{
	std::vector<std::string> out;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			out.push_back(fd.cFileName);
	while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	if (DIR *d = opendir(dir.c_str()))
	{
		while (dirent *e = readdir(d))
			if (e->d_name[0] != '.')
				out.push_back(e->d_name);
		closedir(d);
	}
#endif
	return out;
}

static bool read_file (const std::string &path, Bytes &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t chunk[4096];
	size_t k;
	while ((k = fread(chunk, 1, sizeof(chunk), f)) > 0)
		out.insert(out.end(), chunk, chunk + k);
	fclose(f);
	return true;
}

static bool write_file (const std::string &path, const Bytes &data)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	return fclose(f) == 0 && ok;
}

static std::map<uint32_t, Patch> game_patches (const std::string &dir)
{
	std::map<uint32_t, Patch> out;
	const std::string pdir = dir + "/patches";
	for (const std::string &name : list_dir(pdir))
	{
		Bytes blob;
		if (!read_file(pdir + "/" + name, blob))
			continue;
		if (blob.size() < 17 || blob[0] != 0x03)	// msGamePatch
			continue;
		const uint32_t id = get32(&blob[1]), size = get32(&blob[13]);
		const int32_t version = (int32_t) get32(&blob[5]);
		if (blob.size() < 17 + (size_t) size)
			continue;
		auto it = out.find(id);
		if (it == out.end() || version > it->second.version)
			out[id] = Patch { version, name, Bytes(blob.begin(), blob.begin() + 17 + size) };
	}
	return out;
}

int XBandServerPatchCount (const std::string &dir)
{
	return (int) game_patches(dir).size();
}

// ---------------------------------------------------------------------------
// Reply (reply_xband.py, reply_news.py): news, game name, patch, date, then wait or dial
// ---------------------------------------------------------------------------

enum
{
	msEndOfStream = 0x02, msSetDateAndTime = 0x04, msServerMiscControl = 0x05, msRegisterPlayer = 0x0E,
	msWaitForOpponent = 0x1C, msOpponentPhoneNumber = 0x1D, msReceiveMail = 0x1E, msNewsHeader = 0x1F,
	msNewsPage = 0x20, msQDefDialog = 0x22,
	msReceiveWriteableString = 0x32
};
static const int kCurrentGameNameString = 139, kOpponentName = 172;
static const uint8_t boxGameIDAndPatchVersion = 0x0C, boxSystemVersion = 0x0F;

static uint32_t sega_date (void)
{
	time_t t = time(NULL);
	tm *d = localtime(&t);
	return (uint32_t) (d->tm_year + 1900) << 16 | (uint32_t) d->tm_mon << 12 | (uint32_t) d->tm_mday << 7;
}

static Bytes date_and_time (void)
{
	Bytes b(1, msSetDateAndTime);
	put32(b, sega_date());
	put32(b, 0);
	return b;
}

static Bytes text_object (const std::string &text, int l, int t, int r, int b, int font = 1, int just = 0,
                          uint16_t color = 0x7FFF, uint16_t aa = 0x3DEF)
{
	Bytes o = { 0x01, 0x00, (uint8_t) (font << 4 | just), 0x00, 0x00 };
	put16(o, color);
	put16(o, aa);
	o.insert(o.end(), { (uint8_t) l, (uint8_t) t, (uint8_t) r, (uint8_t) b });
	o.insert(o.end(), text.begin(), text.end());
	o.push_back(0);
	return o;
}

static Bytes news_page (int num, bool last, int form, const std::vector<Bytes> &objects)
{
	Bytes info = { (uint8_t) (num | (last ? 0x80 : 0)), (uint8_t) form, 0, 0, (uint8_t) objects.size() };
	for (size_t i = 1; i <= objects.size(); i++)
		info.push_back((uint8_t) i);
	Bytes offs, body;
	size_t pos = 2 + 2 * objects.size();
	for (const Bytes &o : objects)
	{
		put16(offs, (uint32_t) pos);
		append(body, o);
		pos += o.size();
	}
	Bytes data;
	put16(data, (uint32_t) objects.size());
	append(data, offs);
	append(data, body);
	Bytes block = { 0, 0 };
	put16(block, (uint32_t) data.size());
	block.insert(block.end(), { 0, 0 });
	append(block, data);
	Bytes out = { msNewsPage, 9 };		// kBandwidthNews
	put32(out, (uint32_t) info.size());
	append(out, info);
	put32(out, (uint32_t) block.size());
	append(out, block);
	return out;
}

static std::vector<Bytes> masthead (int num, int total, const std::string &date, int top, int bottom,
                                    uint16_t color, uint16_t aa, uint16_t page_aa)
{
	char page[32];
	snprintf(page, sizeof(page), "Page %d of %d", num, total);
	return { text_object("Daily News", 17, top, 90, bottom, 1, 1, color, aa),
	         text_object(date, 166, top, 239, bottom, 1, 1, color, aa),
	         text_object(page, 144, 190, 219, 201, 1, 0, color, page_aa) };
}

// Today's BANDWIDTH paper, laid out like the last real one (see reply_news.py).
static Bytes news (void)
{
	static const char *kMonths[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
	                                 "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };
	time_t t = time(NULL);
	tm *d = localtime(&t);
	char shortd[32], longd[48];
	snprintf(shortd, sizeof(shortd), "%d.%d.%02d", d->tm_mon + 1, d->tm_mday, d->tm_year % 100);
	snprintf(longd, sizeof(longd), "%s %d, %d", kMonths[d->tm_mon], d->tm_mday, d->tm_year + 1900);
	Bytes out = { msNewsHeader, 9, 0x01 };		// kBandwidthNews, kNewDaysNewsFlag
	std::vector<Bytes> p1 = masthead(1, 2, shortd, 41, 54, 0x17E4, 0x1224, 0x1A66);
	p1.push_back(text_object("XBAND IS BACK ONLINE", 23, 61, 167, 101, 4, 0, 0x7C82, 0x3C41));
	p1.push_back(text_object(longd, 23, 110, 233, 130, 2, 0, 0x185F, 0x0C2F));
	p1.push_back(text_object("Fresh news from your local XBAND server.", 23, 140, 233, 179));
	append(out, news_page(1, false, 0, p1));
	std::vector<Bytes> p2 = masthead(2, 2, shortd, 19, 30, 0x7FFF, 0x4631, 0x4E73);
	p2.push_back(text_object("This paper was written today by a local server that speaks the same protocol as "
	                         "the public XBAND server.", 18, 34, 125, 179));
	p2.push_back(text_object("Pick a game, hit Challenge and the server will look for an opponent.",
	                         129, 35, 233, 95));
	append(out, news_page(2, true, 1, p2));
	return out;
}

static Bytes writable_string (int id, const std::string &text)
{
	Bytes b = { msReceiveWriteableString, (uint8_t) id };
	put32(b, (uint32_t) text.size() + 1);
	b.insert(b.end(), text.begin(), text.end());
	b.push_back(0);
	return b;
}

static Bytes dialog (const std::string &text, uint8_t templat, uint16_t min_ticks, uint16_t max_ticks)
{
	Bytes b = { msQDefDialog };
	put16(b, 0x19);		// kDDaASAFP: main event loop | server connect | connect done
	b.push_back(templat);
	put16(b, min_ticks);
	put16(b, max_ticks);
	put32(b, (uint32_t) text.size() + 1);
	b.insert(b.end(), text.begin(), text.end());
	b.push_back(0);
	return b;
}

static Bytes large_dialog (const std::string &text, uint16_t min_ticks = 120, uint16_t max_ticks = 3600)
{
	return dialog(text, 2, min_ticks, max_ticks);		// kLargeDialog
}

static Bytes medium_dialog (const std::string &text)
{
	return dialog(text, 1, 120, 300);		// kMediumDialog, 2-5 seconds like Server_SendDialog
}

// (gameID, patchVersion) from msGameIDAndPatchVersion, sent right before msSystemVersion.
static bool game_id (const Bytes &up, uint32_t &id, int32_t &version)
{
	for (size_t i = 0; i + 10 <= up.size(); i++)
		if (up[i] == boxGameIDAndPatchVersion && up[i + 9] == boxSystemVersion)
		{
			id = get32(&up[i + 1]);
			version = (int32_t) get32(&up[i + 5]);
			return true;
		}
	return false;
}

static std::mt19937 &rng (void)
{
	static std::mt19937 r((unsigned) std::chrono::high_resolution_clock::now().time_since_epoch().count());
	return r;
}

// ---------------------------------------------------------------------------
// X-Mail (Catapult Server_ReceiveMail, Server_Mail.c, Server_SendMail.c): outboxes come up in the
// upload; mail waits in <dir>/mail until a box connects as its addressee. Bodies stay compressed.
// ---------------------------------------------------------------------------

static const uint8_t  boxLogin = 0x0B, boxChallengeRequest = 0x0E, boxSendOutgoingMail = 0x1D, boxBoxType = 0x1F;
static const uint32_t kDeleteAllOutBoxMailFlag = 0x04;
static const int      kChallengeTypeMailOnly = 4, kMaxInBoxEntries = 10, kMaxOutBoxEntries = 8;
static const size_t   kIdentSize = 79;			// userIdentification: box serial, user, color, icon, town[34], name[34]
static const size_t   kBoxMailHeader = 120;		// the box's Mail struct up to compressedMessage

struct Login { Bytes ident; std::string phone, name; uint8_t user; int inbox; };
struct OutMail { uint8_t user; std::string to, title; Bytes body; };

static std::mutex s_mail_lock;		// <dir>/mail: players.dat and the queued *.xmail

static std::string cstr (const uint8_t *p, size_t max)
{
	size_t n = 0;
	while (n < max && p[n])
		n++;
	return std::string((const char *) p, n);
}

// DataBaseUtil_CompareStrings: names match ignoring case and spaces.
static std::string name_key (const std::string &name)
{
	std::string k;
	for (char c : name)
		if (c != ' ')
			k += (c >= 'a' && c <= 'z') ? (char) (c - 32) : c;
	return k;
}

// msBoxType and msLogin open every upload.
static bool parse_login (const Bytes &up, Login &l)
{
	const size_t at = 6 + 16 + 26;		// opcodes + type, os/db free, flags, last state, phoneNumber
	if (up.size() < at + kIdentSize + 2 || up[0] != boxBoxType || up[5] != boxLogin)
		return false;
	l.phone = cstr(&up[24], 24);
	l.ident.assign(up.begin() + at, up.begin() + at + kIdentSize);
	l.user = l.ident[8];
	l.name = cstr(&l.ident[45], 34);
	l.inbox = up[at + kIdentSize] << 8 | up[at + kIdentSize + 1];
	return true;
}

// msChallengeRequest follows msSystemVersion (length 4); its first byte is the challenge type.
static int challenge_type (const Bytes &up)
{
	for (size_t i = 0; i + 9 <= up.size(); i++)
		if (up[i] == boxSystemVersion && up[i + 1] == 0 && up[i + 2] == 4 && up[i + 7] == boxChallengeRequest)
			return up[i + 8];
	return 0;
}

// DoSendOutgoingMail at i: count, then per mail its local user, the target box + user, name, title, body.
static bool parse_mail_at (const Bytes &up, size_t i, std::vector<OutMail> &out)
{
	size_t p = i + 1;
	auto have = [&] (size_t n) { return p + n <= up.size(); };
	auto u16 = [&] (void) { const int v = up[p] << 8 | up[p + 1]; p += 2; return v; };
	if (!have(2))
		return false;
	const int count = u16();
	if (count < 1 || count > 4 * kMaxOutBoxEntries)
		return false;
	for (int m = 0; m < count; m++)
	{
		OutMail mail;
		if (!have(1 + 8 + 1) || up[p] > 3)
			return false;
		mail.user = up[p];
		p += 1 + 8 + 1;
		for (std::string *field : { &mail.to, &mail.title })
		{
			if (!have(2))
				return false;
			const int n = u16();
			if (n < 1 || n > 128 || !have(n) || up[p + n - 1] != 0)
				return false;
			*field = cstr(&up[p], n);
			p += n;
		}
		if (!have(2))
			return false;
		const int n = u16();
		// MegaPack header: pad, method, BE expanded size, CRC
		if (n < 6 || !have(n) || up[p] != 0 || !(up[p + 2] << 8 | up[p + 3]) || (up[p + 2] << 8 | up[p + 3]) > 4096)
			return false;
		mail.body.assign(up.begin() + p, up.begin() + p + n);
		p += n;
		out.push_back(mail);
	}
	return true;
}

static void outgoing_mail (const Bytes &up, std::vector<OutMail> &out)
{
	for (size_t i = 0; i < up.size(); i++)
	{
		out.clear();
		if (up[i] == boxSendOutgoingMail && parse_mail_at(up, i, out))
			return;
	}
	out.clear();
}

static void make_dir (const std::string &path)
{
#ifdef _WIN32
	CreateDirectoryA(path.c_str(), NULL);
#else
	mkdir(path.c_str(), 0755);
#endif
}

// players.dat: (phone/box serial/user) -> userIdentification of every login, to find senders and addressees.
static std::map<std::string, Bytes> load_players (void)
{
	std::map<std::string, Bytes> out;
	Bytes f;
	if (!read_file(s_dir + "/mail/players.dat", f))
		return out;
	for (size_t p = 0; p < f.size() && p + 1 + f[p] + kIdentSize <= f.size(); p += 1 + f[p] + kIdentSize)
		out[std::string((const char *) &f[p + 1], f[p])] =
			Bytes(f.begin() + p + 1 + f[p], f.begin() + p + 1 + f[p] + kIdentSize);
	return out;
}

static void save_players (const std::map<std::string, Bytes> &players)
{
	Bytes f;
	for (const auto &e : players)
	{
		f.push_back((uint8_t) e.first.size());
		f.insert(f.end(), e.first.begin(), e.first.end());
		append(f, e.second);
	}
	write_file(s_dir + "/mail/players.dat", f);
}

static std::string player_key (const Login &l, uint8_t user)
{
	char box[24];
	snprintf(box, sizeof(box), "/%08X%08X/%d", (unsigned) get32(&l.ident[0]), (unsigned) get32(&l.ident[4]), user);
	return (l.phone + box).substr(0, 255);
}

// An .xmail file: to name, sender's userIdentification, serial, date, title, compressed body.
struct StoredMail { std::string to, title; Bytes from, body; uint16_t serial; uint32_t date; };

static Bytes pack_mail (const StoredMail &m)
{
	Bytes f;
	f.push_back((uint8_t) m.to.size());
	f.insert(f.end(), m.to.begin(), m.to.end());
	append(f, m.from);
	put16(f, m.serial);
	put32(f, m.date);
	f.push_back((uint8_t) m.title.size());
	f.insert(f.end(), m.title.begin(), m.title.end());
	put16(f, (uint32_t) m.body.size());
	append(f, m.body);
	return f;
}

static bool unpack_mail (const Bytes &f, StoredMail &m)
{
	size_t p = 0;
	auto have = [&] (size_t n) { return p + n <= f.size(); };
	if (!have(1) || !have(1 + f[0]))
		return false;
	m.to.assign((const char *) &f[1], f[0]);
	p = 1 + f[0];
	if (!have(kIdentSize + 6 + 1))
		return false;
	m.from.assign(f.begin() + p, f.begin() + p + kIdentSize);
	p += kIdentSize;
	m.serial = f[p] << 8 | f[p + 1];
	m.date = get32(&f[p + 2]);
	p += 6;
	const size_t t = f[p++];
	if (!have(t + 2))
		return false;
	m.title.assign((const char *) &f[p], t);
	p += t;
	const size_t n = f[p] << 8 | f[p + 1];
	p += 2;
	if (!have(n))
		return false;
	m.body.assign(f.begin() + p, f.begin() + p + n);
	return true;
}

// ReceiveUserIdentification's layout: fixed fields, then town and name as length-prefixed C strings.
static void put_ident (Bytes &b, const Bytes &ident)
{
	b.insert(b.end(), ident.begin(), ident.begin() + 11);
	for (size_t at : { (size_t) 11, (size_t) 45 })
	{
		const std::string s = cstr(&ident[at], 33);
		b.push_back((uint8_t) (s.size() + 1));
		b.insert(b.end(), s.begin(), s.end());
		b.push_back(0);
	}
}

// Takes this box's outbox, then hands it the mail waiting for its current player.
static Bytes xmail (const Bytes &up, const Login &login, bool mail_only)
{
	std::lock_guard<std::mutex> g(s_mail_lock);
	Bytes out;
	make_dir(s_dir + "/mail");
	auto players = load_players();
	players[player_key(login, login.user)] = login.ident;
	save_players(players);

	std::vector<OutMail> mails;
	outgoing_mail(up, mails);
	if (!mails.empty())
	{
		int sent = 0;
		for (const OutMail &m : mails)
		{
			auto from = players.find(player_key(login, m.user));
			char msg[160];
			if (from == players.end())
			{
				logf("         mail: player %d of this box never connected; dropped mail to %s", m.user + 1, m.to.c_str());
				snprintf(msg, sizeof(msg), "Player %d has not connected to XBAND.  Mail could not be sent.", m.user + 1);
				append(out, medium_dialog(msg));
				continue;
			}
			bool known = false;
			for (const auto &p : players)
				known |= name_key(cstr(&p.second[45], 34)) == name_key(m.to);
			if (!known)
			{
				logf("         mail: no player named %s; dropped", m.to.c_str());
				snprintf(msg, sizeof(msg), "Sorry, we can't find \"%s\" on XBAND.  Mail not sent.", m.to.c_str());
				append(out, medium_dialog(msg));
				continue;
			}
			StoredMail s { m.to, m.title, from->second, m.body, (uint16_t) (rng()() % 0x7FFF + 1), sega_date() };
			char file[64];
			snprintf(file, sizeof(file), "/mail/%010lld-%04X.xmail", (long long) time(NULL), s.serial);
			if (write_file(s_dir + file, pack_mail(s)))
			{
				logf("         mail: %s -> %s \"%s\" (%d bytes)", cstr(&s.from[45], 34).c_str(), m.to.c_str(),
				     m.title.c_str(), (int) m.body.size());
				sent++;
			}
		}
		Bytes clear = { msServerMiscControl };
		put32(clear, kDeleteAllOutBoxMailFlag);
		append(out, clear);
		if (sent)
			append(out, medium_dialog(sent == 1 ? std::string("One outgoing mail message was sent.") :
			                          std::to_string(sent) + " outgoing mail messages were sent."));
	}

	std::vector<std::string> files = list_dir(s_dir + "/mail");
	std::sort(files.begin(), files.end());
	Bytes in = { msReceiveMail, 0, 0 };
	int delivered = 0, left = 0;
	for (const std::string &name : files)
	{
		Bytes f;
		StoredMail m;
		if (name.size() < 6 || name.compare(name.size() - 6, 6, ".xmail") || !read_file(s_dir + "/mail/" + name, f) ||
		    !unpack_mail(f, m) || name_key(m.to) != name_key(login.name))
			continue;
		if (login.inbox + delivered >= kMaxInBoxEntries)
		{
			left++;
			continue;
		}
		put16(in, (uint32_t) (kBoxMailHeader + m.body.size()));
		put_ident(in, m.from);
		put16(in, m.serial);
		put32(in, m.date);
		in.push_back((uint8_t) (m.title.size() + 1));
		in.insert(in.end(), m.title.begin(), m.title.end());
		in.push_back(0);
		put16(in, (uint32_t) m.body.size());
		append(in, m.body);
		remove((s_dir + "/mail/" + name).c_str());
		logf("         mail: delivered \"%s\" from %s to %s", m.title.c_str(), cstr(&m.from[45], 34).c_str(),
		     login.name.c_str());
		delivered++;
	}
	std::string note;
	if (delivered)
	{
		in[1] = (uint8_t) (delivered >> 8);
		in[2] = (uint8_t) delivered;
		append(out, in);
		note = delivered == 1 ? "One new mail message has been added to your mailbox." :
		       std::to_string(delivered) + " new mail messages have been added to your mailbox.";
	}
	if (left)
		note += (note.empty() ? "" : "  ") + std::string("You have ") + std::to_string(left) +
		        " more, but your mailbox is full.  Please delete some messages.";
	if (!note.empty())
		append(out, left ? large_dialog(note, 120, 300) : medium_dialog(note));
	else if (mail_only)
		append(out, dialog("You have no new mail.", 1, 120, 0));		// sticky until dismissed
	return out;
}

// True with the waiter's cookie/seed when someone waits for this game; else queues this line.
static bool match (bool have_game, uint32_t id, bool have_line, uint32_t line, uint32_t &cookie, uint32_t &rnd)
{
	std::lock_guard<std::mutex> g(sb_lock);
	if (have_game)
	{
		auto w = sb_waiting.find(id);
		if (w != sb_waiting.end() && (!have_line || w->second.line != line) && now_s() < w->second.until)
		{
			cookie = w->second.cookie;
			rnd = w->second.rnd;
			if (have_line)
				sb_pending[line] = w->second.line;
			sb_waiting.erase(w);
			return true;
		}
	}
	cookie = (rng()() & 0x7FFFFFFF) | 1;
	rnd = rng()();
	if (have_game && have_line)
		sb_waiting[id] = Waiter { line, cookie, rnd, now_s() + TIMEOUT_MINUTES * 60 };
	return false;
}

static Bytes reply (const Bytes &upload, bool have_line, uint32_t line)
{
	Bytes out = news();
	Login login;
	const bool mail_only = challenge_type(upload) == kChallengeTypeMailOnly;
	if (parse_login(upload, login))
	{
		logf("         reply: player %s (player %d)%s, %d mails in the box", login.name.c_str(), login.user + 1,
		     mail_only ? ", mail only" : "", login.inbox);
		append(out, xmail(upload, login, mail_only));
	}
	if (mail_only)
	{
		append(out, date_and_time());
		out.push_back(msEndOfStream);
		return out;
	}
	uint32_t id = 0;
	int32_t box_version = 0;
	const bool have_game = game_id(upload, id, box_version);
	std::string name;
	if (have_game)
	{
		name = game_name(id);
		const auto patches = game_patches(s_dir);
		const auto patch = patches.find(id);
		logf("         reply: box game %08X, patch version %d -> %s", (unsigned) id, (int) box_version, plain(name).c_str());
		append(out, writable_string(kCurrentGameNameString, name));
		if (patch != patches.end() && box_version < patch->second.version)
		{
			logf("         reply: sending %s v%d (%d bytes)", patch->second.file.c_str(), (int) patch->second.version,
			     (int) patch->second.msg.size());
			append(out, patch->second.msg);
		}
		else if (patch == patches.end() && box_version < 0)
		{
			logf("         reply: no patch for %s here or in the box: not queued", plain(name).c_str());
			append(out, date_and_time());
			append(out, large_dialog("This XBAND server has no game patch for " + name +
			                         " yet, so it cannot be played online.  Try a different game."));
			out.push_back(msEndOfStream);
			return out;
		}
	}
	append(out, date_and_time());
	uint32_t cookie, rnd;
	if (match(have_game, id, have_line, line, cookie, rnd))
	{
		logf("         reply: line %08X: opponent found, dial it (cookie %08X)", (unsigned) line, (unsigned) cookie);
		append(out, writable_string(kOpponentName, "Opponent"));
		static const char kNumber[] = "5551212";	// any digits: the switchboard routes by line
		Bytes ph = { msOpponentPhoneNumber, 0, (uint8_t) sizeof(kNumber) };
		ph.insert(ph.end(), kNumber, kNumber + sizeof(kNumber));
		put32(ph, cookie);
		put32(ph, rnd);
		append(out, ph);
		out.push_back(msEndOfStream);
		return out;
	}
	if (have_line)
		logf("         reply: line %08X: waiting for an opponent (cookie %08X)", (unsigned) line, (unsigned) cookie);
	Bytes ch = { msWaitForOpponent };
	put32(ch, cookie);
	put32(ch, rnd);
	ch.push_back(msRegisterPlayer);
	put32(ch, TIMEOUT_MINUTES * 60 * 60);
	append(out, ch);
	if (!name.empty())
	{
		char tail[96];
		snprintf(tail, sizeof(tail), " opponent for you.  We should find one within %d minutes.  Please be patient...",
		         TIMEOUT_MINUTES);
		append(out, large_dialog("We are searching for a worthy " + name + tail));
	}
	out.push_back(msEndOfStream);
	return out;
}

// ---------------------------------------------------------------------------
// A server call (xbserver.py serve): HELO, ADSP open, the upload, then the paced reply
// ---------------------------------------------------------------------------

static void serve (sock_t conn, bool have_line, uint32_t line, Bytes data, const char *tag)
{
	Deframer deframer;
	bool have_box = false;
	uint32_t box_next = 0, srv_seq = 0, box_acked = 0;
	uint16_t box_window = WINDOW;
	Bytes upload, stream;
	double last_data = 0, last_progress = now_s(), budget = 0, budget_t = now_s();
	bool replied = false, acked_all = false;

	auto send_frame = [&] (uint8_t desc, const uint8_t *p = NULL, size_t n = 0) {
		send_all(conn, adsp_build(SRV_CONN, srv_seq, box_next, WINDOW, desc, p, n));
	};

	// HELO once a second until the console speaks; the BIOS waits for line traffic.
	send_str(conn, "HELO\n");
	double next_helo = now_s() + 1.0;

	auto pump = [&] (void) {
		const double t = now_s();
		budget = std::min(budget + (t - budget_t) * RATE, (double) CHUNK * 2);
		budget_t = t;
		if (box_acked < stream.size() && t - last_progress > RETRY)
		{
			if (box_window == 0)
				send_frame(0xC0);
			else if (box_acked < srv_seq)
			{
				logf("%s no ack past %u for %.1fs, resending from there", tag, (unsigned) box_acked, RETRY);
				srv_seq = box_acked;
			}
			last_progress = t;
		}
		while (srv_seq < stream.size() && srv_seq - box_acked < box_window)
		{
			const size_t room = std::min<size_t>(CHUNK, box_window - (srv_seq - box_acked));
			const size_t len = std::min<size_t>(room, stream.size() - srv_seq);
			if (budget < len)
				break;
			budget -= len;
			const bool last = srv_seq + len >= stream.size();
			send_frame(last ? 0x60 : 0x40, &stream[srv_seq], len);
			srv_seq += (uint32_t) len;
		}
	};

	for (;;)
	{
		if (data.empty())
		{
			const int r = wait_readable(conn, 0.1);
			if (r < 0 || !s_running)
				return;
			if (r > 0)
			{
				uint8_t buf[4096];
				const int k = recv(conn, (char *) buf, sizeof(buf), 0);
				if (k <= 0)
				{
					logf("%s console hung up; upload %d bytes", tag, (int) upload.size());
					return;
				}
				data.assign(buf, buf + k);
			}
		}
		// Stray bytes before a frame land in its checksum on the console, so HELO stops once it talks.
		if (!have_box && now_s() >= next_helo)
		{
			send_str(conn, "HELO\n");
			next_helo = now_s() + 1.0;
		}
		for (const Bytes &body : deframer.feed(data.data(), data.size()))
		{
			Frame f;
			if (!adsp_parse(body, f))
				continue;
			if (f.recv > box_acked)
			{
				box_acked = f.recv;
				last_progress = now_s();
				// An ack can pass a resend's rewind; never send below it (and keep the window math unsigned-safe).
				if (srv_seq < box_acked)
					srv_seq = box_acked;
			}
			box_window = f.window;		// 0 = its buffer is full: stop
			if (f.desc & 0x80)
			{
				const int code = f.desc & 0x0F;
				if (code == 1)		// open request
				{
					have_box = true;
					logf("%s open request from %04X", tag, f.conn);
					static const uint8_t kOpenAck[] = { 0x00, 0x1E, 0x0F, 0x10, 0x03 };
					send_all(conn, kOpenAck, sizeof(kOpenAck));
					const uint8_t open[] = { 0x01, 0x00, (uint8_t) (f.conn >> 8), (uint8_t) f.conn, 0, 0, 0, 0 };
					send_frame(0x83, open, sizeof(open));
				}
				else if (code == 2)
					logf("%s connection open", tag);
				else if (code == 8)	// retransmit advice
				{
					if (f.recv < srv_seq)
					{
						srv_seq = f.recv;
						last_progress = now_s();
					}
				}
				else if (code == 0)
				{
					if (f.desc & 0x40)
						send_frame(0x80);
				}
				else
					logf("%s control %02X", tag, f.desc);
				continue;
			}
			if (f.seq == box_next && !f.payload.empty())
			{
				if (replied)
				{
					char hex[64] = "";
					for (size_t i = 0; i < f.payload.size() && i < 16; i++)
						snprintf(hex + strlen(hex), sizeof(hex) - strlen(hex), "%02x ", f.payload[i]);
					logf("%s console answered: %s", tag, hex);
				}
				append(upload, f.payload);
				box_next += (uint32_t) f.payload.size();
				last_data = now_s();
			}
			send_frame(0x80);
		}
		data.clear();
		if (!replied && last_data && !upload.empty() && now_s() - last_data > IDLE)
		{
			replied = true;
			logf("%s upload done: %d bytes", tag, (int) upload.size());
			stream = reply(upload, have_line, line);
			logf("%s replying %d ServerTalk bytes", tag, (int) stream.size());
			last_progress = now_s();
		}
		if (replied)
		{
			pump();
			if (!stream.empty() && box_acked >= stream.size() && !acked_all)
			{
				acked_all = true;
				logf("%s console acked the whole reply", tag);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Phone lines (xbserver.py serve_ring, serve_dial, wait_answer, relay)
// ---------------------------------------------------------------------------

// An idle line: wait for a caller, or for the window to hang it up.
static void serve_ring (sock_t conn, uint32_t line, const char *tag)
{
	auto entry = std::make_shared<RingLine>();
	entry->conn = conn;
	{
		std::lock_guard<std::mutex> g(sb_lock);
		sb_ring_lines[line] = entry;
	}
	logf("%s idle, can be rung", tag);
	while (!entry->bridged.is_set() && s_running)
	{
		const int r = wait_readable(conn, 0.5);
		if (r < 0)
			break;
		if (r > 0 && !entry->bridged.is_set())
		{
			char c[64];
			if (recv(conn, c, sizeof(c), 0) <= 0)
				break;
		}
	}
	entry->released.set();
	if (entry->bridged.is_set())
		entry->done.wait();
	{
		std::lock_guard<std::mutex> g(sb_lock);
		auto it = sb_ring_lines.find(line);
		if (it != sb_ring_lines.end() && it->second == entry)
			sb_ring_lines.erase(it);
		if (!entry->bridged.is_set())	// a window that hung up its idle line can no longer be rung
			for (auto w = sb_waiting.begin(); w != sb_waiting.end(); )
				w = (w->second.line == line) ? sb_waiting.erase(w) : std::next(w);
	}
	logf("%s line closed", tag);
}

static void relay (sock_t a, sock_t b, const char *tag)
{
	size_t counts[2] = { 0, 0 };
	while (s_running)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(a, &r);
		FD_SET(b, &r);
		timeval tv = { 1, 0 };
		const int n = select((int) std::max(a, b) + 1, &r, NULL, NULL, &tv);
		if (n < 0)
			break;
		for (int side = 0; side < 2; side++)
		{
			const sock_t s = side ? b : a, other = side ? a : b;
			if (!FD_ISSET(s, &r))
				continue;
			uint8_t buf[4096];
			const int k = recv(s, (char *) buf, sizeof(buf), 0);
			if (k <= 0)
			{
				logf("%s call ended: caller sent %d bytes, answerer %d", tag, (int) counts[0], (int) counts[1]);
				return;
			}
			counts[side] += k;
			send_all(other, buf, k);
		}
	}
}

// Ring until the window answers ("ANSWER"); anything the caller sends meanwhile is held.
static bool wait_answer (sock_t caller, sock_t ring, Bytes &held, const char *tag)
{
	std::string got;
	while (s_running)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(caller, &r);
		FD_SET(ring, &r);
		timeval tv = { 1, 0 };
		if (select((int) std::max(caller, ring) + 1, &r, NULL, NULL, &tv) < 0)
			return false;
		if (FD_ISSET(caller, &r))
		{
			uint8_t buf[4096];
			const int k = recv(caller, (char *) buf, sizeof(buf), 0);
			if (k <= 0)
			{
				logf("%s caller hung up while it rang", tag);
				return false;
			}
			held.insert(held.end(), buf, buf + k);
		}
		if (FD_ISSET(ring, &r))
		{
			char c;
			if (recv(ring, &c, 1, 0) <= 0)
			{
				logf("%s opponent line closed while it rang", tag);
				return false;
			}
			got += c;
			if (c == '\n')
			{
				if (got == "ANSWER\n")
					return true;
				got.clear();
			}
		}
	}
	return false;
}

static void serve_dial (sock_t conn, uint32_t waiter, const char *tag)
{
	std::shared_ptr<RingLine> ring;
	{
		std::lock_guard<std::mutex> g(sb_lock);
		auto it = sb_ring_lines.find(waiter);
		if (it != sb_ring_lines.end())
			ring = it->second;
	}
	if (!ring)
	{
		logf("%s opponent line %08X is not idle; nobody answers", tag, (unsigned) waiter);
		return;
	}
	ring->bridged.set();
	ring->released.wait(2.0);
	logf("%s ringing line %08X", tag, (unsigned) waiter);
	send_str(ring->conn, "RING\n");
	Bytes held;
	if (wait_answer(conn, ring->conn, held, tag))
	{
		logf("%s answered; %d caller bytes held", tag, (int) held.size());
		send_str(conn, "HELO\n");		// the caller's modem hears the answer tone
		if (!held.empty())
			send_all(ring->conn, held);
		relay(conn, ring->conn, tag);
	}
	ring->done.set();
}

// The identity line, then an optional "LINE <id>" / "RING <id>" line (emulator windows).
static bool read_intro (sock_t conn, std::string &kind, uint32_t &line, Bytes &pending)
{
	std::string ident;
	const double until = now_s() + 10;
	while (ident.empty() || ident.back() != '\n')
	{
		if (now_s() > until || wait_readable(conn, until - now_s()) <= 0)
			return false;
		char c;
		if (recv(conn, &c, 1, 0) <= 0)
			return false;
		ident += c;
	}
	std::string buf;
	const double more = now_s() + 0.3;
	while ((buf.empty() || buf.back() != '\n') && buf.size() < 32 && now_s() < more)
	{
		if (wait_readable(conn, more - now_s()) <= 0)
			break;
		char c;
		if (recv(conn, &c, 1, 0) <= 0)
			break;
		buf += c;
		const std::string head = buf.substr(0, 5);
		if (std::string("LINE ").compare(0, head.size(), head) && std::string("RING ").compare(0, head.size(), head))
			break;
	}
	if (buf.size() > 5 && (buf.compare(0, 5, "LINE ") == 0 || buf.compare(0, 5, "RING ") == 0) && buf.back() == '\n')
	{
		kind = buf.substr(0, 4);
		line = (uint32_t) strtoul(buf.c_str() + 5, NULL, 16);
		return true;
	}
	kind.clear();
	pending.assign(buf.begin(), buf.end());
	return true;
}

static void handle (sock_t conn, std::string addr)
{
	std::string kind;
	uint32_t line = 0;
	Bytes pending;
	if (read_intro(conn, kind, line, pending))
	{
		const bool have_line = !kind.empty();
		char tag[16];
		snprintf(tag, sizeof(tag), have_line ? "[%08X]" : "[--------]", (unsigned) line);
		if (kind == "RING")
			serve_ring(conn, line, tag);
		else
		{
			bool dial = false;
			uint32_t waiter = 0;
			if (have_line)
			{
				std::lock_guard<std::mutex> g(sb_lock);
				auto p = sb_pending.find(line);
				if (p != sb_pending.end())
				{
					waiter = p->second;
					sb_pending.erase(p);
					dial = true;
				}
			}
			if (dial)
			{
				logf("%s call from %s dialing the opponent", tag, addr.c_str());
				serve_dial(conn, waiter, tag);
			}
			else
			{
				logf("%s call from %s", tag, addr.c_str());
				serve(conn, have_line, line, pending, tag);
			}
		}
	}
	track(conn, false);
	close_sock(conn);
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

static void accept_loop (void)
{
	while (s_running)
	{
		sockaddr_in from;
		socklen_t len = sizeof(from);
		const sock_t c = accept(s_listen, (sockaddr *) &from, &len);
		if (c == BAD_SOCK)
			break;
		if (!s_running)
		{
			close_sock(c);
			break;
		}
		int one = 1;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
		char ip[64];
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
		track(c, true);
		std::thread(handle, c, std::string(ip)).detach();
	}
}

bool XBandServerStart (int port, const std::string &dir, std::string &why)
{
	if (s_running)
		return true;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		why = "Winsock didn't start";
		return false;
	}
#endif
	const sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == BAD_SOCK)
	{
		why = "no socket";
		return false;
	}
	int one = 1;
#ifdef _WIN32
	// Windows lets a second SO_REUSEADDR listener steal the port; claim it outright.
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &one, sizeof(one));
#else
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#endif
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);		// other players reach it by this PC's address
	a.sin_port = htons((uint16_t) port);
	if (bind(s, (sockaddr *) &a, sizeof(a)) != 0 || listen(s, 16) != 0)
	{
		close_sock(s);
		why = "port " + std::to_string(port) + " is already in use or not allowed";
		return false;
	}
	s_dir = dir;
	s_port = port;
	s_listen = s;
	s_t0 = now_s();
	{
		std::lock_guard<std::mutex> g(s_log_lock);
		s_log = fopen((dir + "/server.log").c_str(), "a");
	}
	logf("listening on %d, %d game patches in %s/patches", port, XBandServerPatchCount(dir), dir.c_str());
	s_running = true;
	s_accept = std::thread(accept_loop);
	return true;
}

void XBandServerStop (void)
{
	if (!s_running)
		return;
	s_running = false;
	close_sock(s_listen);
	s_listen = BAD_SOCK;
	if (s_accept.joinable())
		s_accept.join();
	{
		std::lock_guard<std::mutex> g(s_socks_lock);
		for (sock_t c : s_socks)
		{
#ifdef _WIN32
			shutdown(c, SD_BOTH);
#else
			shutdown(c, SHUT_RDWR);
#endif
		}
	}
	{
		std::lock_guard<std::mutex> g(sb_lock);
		sb_waiting.clear();
		sb_pending.clear();
	}
	logf("stopped");
	std::lock_guard<std::mutex> g(s_log_lock);
	if (s_log)
		fclose(s_log);
	s_log = NULL;
}

bool XBandServerRunning (void)
{
	return s_running;
}

int XBandServerPort (void)
{
	return s_port;
}
