/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Barcode Taisen Bardigun's card reader. Protocol per Dan Docs and the game's
// own reader (Rev 1 bank 6 $6D02): it waits for a white byte, then a black
// bit, and counts runs until one colour lasts 512 samples; 30 bars read as
// EAN-13, 22 as EAN-8. A swipe here is the card's bars at an even speed.

#include "gb_bardigun.h"

#include <cstring>

namespace SGB {

namespace {

// Samples per module: the thinnest bar reads as 14-16 on hardware (Dan Docs).
constexpr uint32_t kSamplesPerModule = 15;
// White before the bars and after: the game needs a whole white byte to start
// and a run of 128 samples (Rev 0) or 512 (Rev 1) to stop.
constexpr uint32_t kLead = 24 * 8;
constexpr uint32_t kTail = 80 * 8;
constexpr uint32_t kStop = 128;
// Game bytes this close are a stream; the first byte of one is not the scan
// screen yet, so the card waits for the second.
constexpr int32_t kStream = 1 << 15;
// The game stopped sampling mid-swipe: the rest of it is lost.
constexpr int32_t kStall = 1 << 18;
constexpr int kMaxDigits = 13;

// The reader's side of the wire; a savestate carries it.
struct State
{
	uint8_t  busy;               // a swipe is going out
	uint8_t  pending;            // a card waits in the reader
	uint8_t  len;                // digits of the card going out, 8 or 13
	uint8_t  next_len;           // and of the one waiting
	uint8_t  counted;            // the swipe went in far enough to read
	uint8_t  reply;              // what the game's byte shifts back
	char     next[kMaxDigits];
	char     card[kMaxDigits];
	uint32_t pos;                // next sample of the swipe
	int32_t  since;              // since the game last clocked a byte
	uint32_t reads;
};

constexpr size_t kStateBytes = 6 + 2 * kMaxDigits + 3 * 4;

State g_state = { 0, 0, 0, 0, 0, 0, {}, {}, 0, kStall, 0 };
bool  g_connected = false;
char  g_last[kMaxDigits + 1];

const BarcodeBoyCard kCards[] =
{
	// Bundled: each back is one barcode for the egg it hatches and the attack it gives.
	{ "Barcode Taisen Bardigun", "Ralph: Revive", "\xE3\x83\xA9\xE3\x83\xAB\xE3\x83\x95\xE3\x83\xBB\xE3\x82\xA4\xE3\x82\xAD\xE3\x82\xAB\xE3\x82\xA8\xE3\x83\xBC\xE3\x83\xAB", "4900758340136", "bardigun/4900758340136", "Cards" },
	{ "Barcode Taisen Bardigun", "Tamappi: Cure-All", "\xE3\x82\xBF\xE3\x83\x9E\xE3\x83\x83\xE3\x83\x94\xE3\x83\xBB\xE3\x83\x8A\xE3\x83\xB3\xE3\x83\x87\xE3\x83\xA2\xE3\x83\x8A\xE3\x82\xAA\xE3\x83\xBC\xE3\x83\xAB", "4900269429252", "bardigun/4900269429252", "Cards" },
	{ "Barcode Taisen Bardigun", "Chibisshii: Sword", "\xE3\x83\x81\xE3\x83\x93\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xBC\xE3\x83\xBB\xE3\x82\xBD\xE3\x83\xBC\xE3\x83\x89", "4900287167419", "bardigun/4900287167419", "Cards" },
	// Shop barcodes Dan Docs found: any JAN hatches something.
	{ "Barcode Taisen Bardigun", "Chakanan: High Healing", "\xE3\x83\x81\xE3\x83\xA3\xE3\x82\xAB\xE3\x83\x8A\xE3\x83\xB3\xE3\x83\xBB\xE3\x83\x8F\xE3\x82\xA4\xE3\x83\x92\xE3\x83\xBC\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xB0", "4580245316402", "bardigun/4580245316402", "Shop barcodes" },
	{ "Barcode Taisen Bardigun", "Chibisshii: Eye Beam", "\xE3\x83\x81\xE3\x83\x93\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xBC\xE3\x83\xBB\xE3\x82\x81\xE3\x81\x8B\xE3\x82\x89\xE3\x83\x93\xE3\x83\xBC\xE3\x83\xA0", "4901330547479", "bardigun/4901330547479", "Shop barcodes" },
	{ "Barcode Taisen Bardigun", "Chip: Lullaby", "\xE3\x83\x81\xE3\x83\x83\xE3\x83\x97\xE3\x83\xBB\xE3\x81\x93\xE3\x82\x82\xE3\x82\x8A\xE3\x81\x86\xE3\x81\x9F", "4983164730678", "bardigun/4983164730678", "Shop barcodes" },
	{ "Barcode Taisen Bardigun", "Noisy: Revive", "\xE3\x83\x8E\xE3\x82\xA4\xE3\x82\xB8\xE3\x83\xBC\xE3\x83\xBB\xE3\x82\xA4\xE3\x82\xAD\xE3\x82\xAB\xE3\x82\xA8\xE3\x83\xBC\xE3\x83\xAB", "4900753335663", "bardigun/4900753335663", "Shop barcodes" },
	{ "Barcode Taisen Bardigun", "Nyaari: Flamethrower", "\xE3\x83\x8B\xE3\x83\xA3\xE3\x83\xBC\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\xBB\xE3\x81\x8B\xE3\x81\x88\xE3\x82\x93\xE3\x81\xBB\xE3\x81\x86\xE3\x81\x97\xE3\x82\x83\xE3\x81\x8D", "4900363432103", "bardigun/4900363432103", "Shop barcodes" },
	{ "Barcode Taisen Bardigun", "Takora: Fire Bolt", "\xE3\x82\xBF\xE3\x82\xB3\xE3\x83\xA9\xE3\x83\xBB\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x82\xA2\xE3\x83\xBC\xE3\x83\x9C\xE3\x83\xAB\xE3\x83\x88", "4902370501445", "bardigun/4902370501445", "Shop barcodes" },
};

// EAN L codes, a bar = 1, first module in bit 6. R codes are their
// complements; G codes are the R codes backwards.
const uint8_t kEanL[10] = { 0x0D, 0x19, 0x13, 0x3D, 0x23, 0x31, 0x2F, 0x3B, 0x37, 0x0B };
// Which of an EAN-13's six left digits are G codes (bit 5 = first), per its first digit.
const uint8_t kEanParity[10] = { 0x00, 0x0B, 0x0D, 0x0E, 0x13, 0x19, 0x1C, 0x15, 0x16, 0x1A };

uint8_t Reverse7(uint8_t v)
{
	uint8_t r = 0;
	for (int i = 0; i < 7; ++i) r = static_cast<uint8_t>(r | (((v >> i) & 1) << (6 - i)));
	return r;
}

int Modules(int len) { return len == 13 ? 95 : 67; }

// Module m of a code's bars: true for a bar.
bool Bar(const char *code, int len, int m)
{
	const int half  = len == 13 ? 6 : 4;
	const int first = len == 13 ? 1 : 0;
	const int mid   = 3 + 7 * half;
	const int right = mid + 5;
	const int end   = right + 7 * half;

	if (m < 3)     return m != 1;
	if (m >= end)  return m - end != 1;
	if (m >= mid && m < right) return ((m - mid) & 1) != 0;

	const bool left = m < mid;
	const int  d    = (m - (left ? 3 : right)) / 7;
	const int  k    = (m - (left ? 3 : right)) % 7;
	const int  v    = code[first + (left ? 0 : half) + d] - '0';
	uint8_t pattern = kEanL[v];
	if (!left)
		pattern = static_cast<uint8_t>(~pattern & 0x7F);
	else if (len == 13 && ((kEanParity[code[0] - '0'] >> (half - 1 - d)) & 1))
		pattern = Reverse7(static_cast<uint8_t>(~pattern & 0x7F));
	return ((pattern >> (6 - k)) & 1) != 0;
}

uint32_t BarSamples(int len) { return static_cast<uint32_t>(Modules(len)) * kSamplesPerModule; }

// The whole swipe, padded to whole bytes.
uint32_t SwipeSamples(int len) { return (kLead + BarSamples(len) + kTail + 7) & ~7u; }

// What the sensor sees at sample p of the swipe: 1 white, 0 black.
int Sample(const State &st, uint32_t p)
{
	if (p < kLead) return 1;
	p -= kLead;
	if (p >= BarSamples(st.len)) return 1;
	return Bar(st.card, st.len, static_cast<int>(p / kSamplesPerModule)) ? 0 : 1;
}

void Put32(uint8_t *&p, uint32_t v)
{
	for (int i = 0; i < 4; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t Get32(const uint8_t *&p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(*p++) << (8 * i);
	return v;
}

} // anonymous

void BardigunConnect(bool on)
{
	if (on == g_connected) return;
	g_connected = on;
	if (!on)
	{
		// Unplugged: a swipe in flight or a card waiting is gone.
		const uint32_t reads = g_state.reads;
		g_state       = State();
		g_state.since = kStall;
		g_state.reads = reads;
	}
}

bool BardigunConnected() { return g_connected; }

bool BardigunScan(const char *code)
{
	char digits[kMaxDigits];
	int  n = 0;
	for (const char *c = code; c && *c; ++c)
	{
		if (*c == ' ' || *c == '-') continue;
		if (*c < '0' || *c > '9' || n == kMaxDigits) return false;
		digits[n++] = *c;
	}
	if (n != 13 && n != 8) return false;

	std::memcpy(g_state.next, digits, n);
	g_state.next_len = static_cast<uint8_t>(n);
	g_state.pending  = 1;
	std::memcpy(g_last, digits, n);
	g_last[n] = 0;
	g_connected = true;
	return true;
}

bool BardigunPending() { return g_state.pending || g_state.busy; }
const char *BardigunLastCode() { return g_last; }
uint32_t BardigunReadCount() { return g_state.reads; }

uint8_t BardigunExchange()
{
	State &st = g_state;
	const bool streaming = st.since < kStream;
	st.since = 0;

	if (!st.busy && st.pending && streaming)
	{
		std::memcpy(st.card, st.next, kMaxDigits);
		st.len     = st.next_len;
		st.pending = 0;
		st.busy    = 1;
		st.pos     = 0;
		st.counted = 0;
	}
	if (!st.busy)
	{
		st.reply = 0x00;
		return st.reply;
	}

	uint8_t b = 0;
	for (int i = 0; i < 8; ++i) b = static_cast<uint8_t>((b << 1) | Sample(st, st.pos + i));
	st.pos += 8;
	if (!st.counted && st.pos >= kLead + BarSamples(st.len) + kStop)
	{
		st.counted = 1;
		++st.reads;
	}
	if (st.pos >= SwipeSamples(st.len)) st.busy = 0;
	st.reply = b;
	return b;
}

uint8_t BardigunReply() { return g_state.reply; }

void BardigunClock(int32_t cycles)
{
	State &st = g_state;
	if (st.since >= kStall) return;
	st.since += cycles;
	if (st.since >= kStall)
	{
		st.since = kStall;
		st.busy  = 0;
	}
}

size_t BardigunStateBytes() { return kStateBytes; }

void BardigunStateSave(uint8_t *out)
{
	const State &st = g_state;
	uint8_t *p = out;
	*p++ = st.busy;
	*p++ = st.pending;
	*p++ = st.len;
	*p++ = st.next_len;
	*p++ = st.counted;
	*p++ = st.reply;
	std::memcpy(p, st.next, kMaxDigits); p += kMaxDigits;
	std::memcpy(p, st.card, kMaxDigits); p += kMaxDigits;
	Put32(p, st.pos);
	Put32(p, static_cast<uint32_t>(st.since));
	Put32(p, st.reads);
}

void BardigunStateLoad(const uint8_t *in)
{
	State &st = g_state;
	const uint8_t *p = in;
	st.busy     = *p++ ? 1 : 0;
	st.pending  = *p++ ? 1 : 0;
	st.len      = *p++;
	st.next_len = *p++;
	st.counted  = *p++ ? 1 : 0;
	st.reply    = *p++;
	std::memcpy(st.next, p, kMaxDigits); p += kMaxDigits;
	std::memcpy(st.card, p, kMaxDigits); p += kMaxDigits;
	st.pos   = Get32(p);
	st.since = static_cast<int32_t>(Get32(p));
	st.reads = Get32(p);

	// A state from before a scan, or a damaged one, leaves the reader idle.
	if (st.len != 13 && st.len != 8) st.busy = 0;
	if (st.next_len != 13 && st.next_len != 8) st.pending = 0;
	for (int i = 0; i < kMaxDigits; ++i)
	{
		if (i < st.len && (st.card[i] < '0' || st.card[i] > '9')) st.busy = 0;
		if (i < st.next_len && (st.next[i] < '0' || st.next[i] > '9')) st.pending = 0;
	}
	if (st.busy && st.pos >= SwipeSamples(st.len)) st.busy = 0;
	if (st.since < 0 || st.since > kStall) st.since = kStall;
}

const BarcodeBoyCard *BardigunCards(int *count)
{
	if (count) *count = static_cast<int>(sizeof kCards / sizeof kCards[0]);
	return kCards;
}

int EanBars(const char *code, bool *bars)
{
	const int len = code ? static_cast<int>(std::strlen(code)) : 0;
	if (len != 13 && len != 8) return 0;
	for (int i = 0; i < len; ++i)
		if (code[i] < '0' || code[i] > '9') return 0;
	const int n = Modules(len);
	for (int m = 0; m < n; ++m) bars[m] = Bar(code, len, m);
	return n;
}

const char *BardigunGame(const char *title)
{
	return title && std::strcmp(title, "BARDIGUN") == 0 ? "Barcode Taisen Bardigun" : nullptr;
}

} // namespace SGB
