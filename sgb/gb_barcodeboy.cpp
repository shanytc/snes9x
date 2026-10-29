/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Namcot's Barcode Boy. Protocol per Dan Docs (Barcode Boy) and Shonumi's
// hardware captures; the five games share the receive side: their serial
// ISR files each byte and re-arms the external clock ($80) at once. The
// cards' codes are read off scans of the printed cards.

#include "gb_barcodeboy.h"

#include <cstring>

namespace SGB {

namespace {

// Its clock is unmeasured; a byte per 8 bit periods at 8192 Hz is what GBE+
// sends, and Battle Space gives up on a card after ~5 ms between bytes.
constexpr int32_t kBytePeriod = 4096;
// A card waits this long after the game's last own byte (the knock) before
// it goes in, as a hand scanning after the prompt would: the game is still
// filing the knock's reply until then.
constexpr int32_t kSettle = 1 << 20;   // ~0.25 s
// The game stops listening mid-card: the rest of it is lost.
constexpr int32_t kStall = 1 << 18;    // ~62 ms
constexpr int kCardBytes = 30;
constexpr int kDigits    = 13;

constexpr uint8_t kKnock[4] = { 0x10, 0x07, 0x10, 0x07 };
constexpr uint8_t kStx = 0x02, kEtx = 0x03;

// The scanner's side of the wire; a savestate carries it.
struct State
{
	uint8_t  matched;          // knock bytes heard in a row, 0..3
	uint8_t  armed;            // knocked: the next card may go out
	uint8_t  reply;            // what the game's byte shifts back
	uint8_t  busy;             // a card is going out
	uint8_t  pos;              // its next byte, 0..29
	uint8_t  pending;          // a scanned card waits in the scanner
	char     next[kDigits];    // that card
	char     card[kDigits];    // the one going out
	int32_t  timer;            // until its clock may land another byte
	int32_t  quiet;            // since the game last clocked a byte itself
	int32_t  stall;            // the game not listening, mid-card
	uint32_t reads;            // cards the game took whole
};

constexpr size_t kStateBytes = 6 + 2 * kDigits + 4 * 4;

State g_state;
bool  g_connected = false;
char  g_last[kDigits + 1];

const BarcodeBoyCard kCards[] =
{
	{ "Battle Space", "Magic Soldier", "\xE3\x83\x9E\xE3\x83\x9B\xE3\x82\xA6\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xB7", "4902776809367", "battle_space/4902776809367" },
	{ "Battle Space", "Knight", "\xE3\x83\x8A\xE3\x82\xA4\xE3\x83\x88", "4905672306367", "battle_space/4902776809367" },
	{ "Battle Space", "Berserker", "\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\xBC\xE3\x82\xAB\xE3\x83\xBC", "4907981000301", "battle_space/4907981000301" },
	{ "Battle Space", "Valkyrie", "\xE3\x83\x90\xE3\x83\xAB\xE3\x82\xAD\xE3\x83\xAA\xE3\x83\xBC", "4908052808369", "battle_space/4907981000301" },
	{ "Battle Space", "Warrior", "\xE3\x82\xA6\xE3\x82\xA9\xE3\x83\xAA\xE3\x82\xA2\xE3\x83\xBC", "4909062206350", "battle_space/4909062206350" },
	{ "Battle Space", "Sorcerer", "\xE3\x82\xBD\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\xA9\xE3\x83\xBC", "4911826551347", "battle_space/4909062206350" },
	{ "Battle Space", "Wraith", "\xE3\x83\xAC\xE3\x82\xA4\xE3\x82\xB9", "4912713004366", "battle_space/4912713004366" },
	{ "Battle Space", "Shaman", "\xE3\x82\xB7\xE3\x83\xA3\xE3\x83\xBC\xE3\x83\x9E\xE3\x83\xB3", "4913508504399", "battle_space/4912713004366" },
	{ "Battle Space", "Grizzly Bear", "\xE3\x82\xB0\xE3\x83\xAA\xE3\x82\xBA\xE3\x83\xAA\xE3\x83\xBC", "4916911302309", "battle_space/4916911302309" },
	{ "Battle Space", "Thief", "\xE3\x82\xB7\xE3\x83\xBC\xE3\x83\x95", "4918156001351", "battle_space/4916911302309" },
	// Only A1, A4 and B1 make the horse printed on the card (Dan Docs).
	{ "Family Jockey 2", "Racehorse A", "\xE7\xAB\xB6\xE4\xBA\x89\xE9\xA6\xAC" "A", "5893713522816", "family_jockey_2/5893713522816" },
	{ "Family Jockey 2", "Racehorse B", "\xE7\xAB\xB6\xE4\xBA\x89\xE9\xA6\xAC" "B", "1509843019075", "family_jockey_2/5893713522816" },
	{ "Family Jockey 2", "Broodmare A", "\xE7\xB9\x81\xE6\xAE\x96\xE9\xA6\xAC" "A", "2378649896765", "family_jockey_2/2378649896765" },
	{ "Family Jockey 2", "Broodmare B", "\xE7\xB9\x81\xE6\xAE\x96\xE9\xA6\xAC" "B", "4232978865152", "family_jockey_2/2378649896765" },
	{ "Family Jockey 2", "Broodmare C", "\xE7\xB9\x81\xE6\xAE\x96\xE9\xA6\xAC" "C", "7164625542390", "family_jockey_2/7164625542390" },
	{ "Family Jockey 2", "Stallion A", "\xE7\xA8\xAE\xE9\xA6\xAC" "A", "9845554422318", "family_jockey_2/9845554422318" },
	{ "Family Jockey 2", "Stallion B", "\xE7\xA8\xAE\xE9\xA6\xAC" "B", "3572821107673", "family_jockey_2/9845554422318" },
	{ "Family Jockey 2", "Stallion C", "\xE7\xA8\xAE\xE9\xA6\xAC" "C", "6319537443513", "family_jockey_2/6319537443513" },
	{ "Famista 3", "Home-Run Batter", "\xE3\x83\x9B\xE3\x83\xBC\xE3\x83\xA0\xE3\x83\xA9\xE3\x83\xB3\xE3\x83\x90\xE3\x83\x83\xE3\x82\xBF\xE3\x83\xBC", "8357933639923", "famista_3/8357933639923" },
	{ "Famista 3", "High-Average Batter", "\xE9\xAB\x98\xE6\x89\x93\xE7\x8E\x87\xE3\x83\x90\xE3\x83\x83\xE3\x82\xBF\xE3\x83\xBC", "7814374127798", "famista_3/7814374127798" },
	{ "Famista 3", "Swift Batter", "\xE9\xA7\xBF\xE8\xB6\xB3\xE3\x83\x90\xE3\x83\x83\xE3\x82\xBF\xE3\x83\xBC", "9880692151263", "famista_3/9880692151263" },
	{ "Famista 3", "Pitcher", "\xE3\x83\x94\xE3\x83\x83\xE3\x83\x81\xE3\x83\xA3\xE3\x83\xBC", "1414213562177", "famista_3/1414213562177" },
	{ "Kattobi Road", "Racecar", "\xE3\x83\x8A\xE3\x82\xA4\xE3\x82\xBF\xE3\x83\xBC" "2000", "4903301160625", "kattobi_road/4903301160625" },
	{ "Kattobi Road", "F1-style Racecar", "\xE3\x83\xAD\xE3\x82\xAF\xE3\x82\xBF\xE3\x82\xB9\xE3\x83\xA8\xE3\x83\xB3\xE3\x83\xBC\xE3\x83\x88", "4987084410924", "kattobi_road/4987084410924" },
	{ "Kattobi Road", "Japanese Street Car", "\xE3\x83\x9F\xE3\x82\xA4\xE3\x83\xA9\xE3\x82\xBF\xE3\x83\xBC\xE3\x83\x9C", "4902888119101", "kattobi_road/4902888119101" },
	{ "Kattobi Road", "Sedan", "\xE3\x82\xAC\xE3\x82\xA6\xE3\x83\x87\xE3\x82\xA3", "4901121110004", "kattobi_road/4901121110004" },
	{ "Kattobi Road", "4x4 Jeep", "\xE3\x83\x8F\xE3\x82\xA4\xE3\x82\xB9\xE3\x83\xA9\xE3\x83\x83\xE3\x82\xAF\xE3\x82\xB9", "4901780161157", "kattobi_road/4901780161157" },
	{ "Kattobi Road", "Truck", "\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\xAB\xE3\x83\x89", "4902105002063", "kattobi_road/4901780161157" },
	// Cards 3-5 carry no barcode: the player sticks one on.
	{ "Monster Maker: Barcode Saga", "Mage Link", "\xE9\xAD\x94\xE9\x81\x93\xE5\xA3\xAB\xE3\x83\xAA\xE3\x83\xB3\xE3\x82\xAF", "9120153954577", "monster_maker/9120153954577" },
	{ "Monster Maker: Barcode Saga", "Mage Rufia", "\xE9\xAD\x94\xE9\x81\x93\xE5\xA3\xAB\xE3\x83\xAB\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xBC\xE3\x82\xA2", "9022038781998", "monster_maker/9120153954577" },
	{ "Monster Maker: Barcode Saga", "Warrior Diane", "\xE6\x88\xA6\xE5\xA3\xAB\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xA2\xE3\x83\xBC\xE3\x83\x8D", "9752412234900", "monster_maker/9120153954577" },
	{ "Monster Maker: Barcode Saga", "Warrior Tamron", "\xE6\x88\xA6\xE5\xA3\xAB\xE3\x82\xBF\xE3\x83\xA0\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\xB3", "9362462085911", "monster_maker/9120153954577" },
	{ "Monster Maker: Barcode Saga", "Knight Lauren", "\xE9\xA8\x8E\xE5\xA3\xAB\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\xA9\xE3\x83\xB3", "9052091324955", "monster_maker/9052091324955" },
	{ "Monster Maker: Barcode Saga", "Dragon Knight Hagen", "\xE7\xAB\x9C\xE9\xA8\x8E\xE5\xA3\xAB\xE3\x83\x8F\xE3\x83\xBC\xE3\x82\xB2\xE3\x83\xB3", "9322158686716", "monster_maker/9052091324955" },
	{ "Monster Maker: Barcode Saga", "Archer Lorian", "\xE5\xBC\x93\xE4\xBD\xBF\xE3\x81\x84\xE3\x83\xAD\xE3\x83\xAA\xE3\x82\xA8\xE3\x83\xBC\xE3\x83\xB3", "9998017308336", "monster_maker/9052091324955" },
	{ "Monster Maker: Barcode Saga", "Archer Elsais", "\xE5\xBC\x93\xE4\xBD\xBF\xE3\x81\x84\xE3\x82\xA8\xE3\x83\xAB\xE3\x82\xB5\xE3\x82\xA4\xE3\x82\xB9", "9447410810323", "monster_maker/9052091324955" },
};

// Header titles of the five games that read it.
const struct { const char *title, *game; } kGames[] =
{
	{ "BATTLE SPACE",   "Battle Space" },
	{ "FAMILY JOCKEY2", "Family Jockey 2" },
	{ "FAMISTA3",       "Famista 3" },
	{ "KATTOBI ROAD",   "Kattobi Road" },
	{ "MONSTER MAKER",  "Monster Maker: Barcode Saga" },
};

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

void BarcodeBoyConnect(bool on)
{
	if (on == g_connected) return;
	g_connected = on;
	if (!on)
	{
		// Unplugged: whatever was in flight or waiting is gone.
		const uint32_t reads = g_state.reads;
		g_state       = State();
		g_state.reads = reads;
	}
}

bool BarcodeBoyConnected() { return g_connected; }

bool BarcodeBoyScan(const char *code)
{
	char digits[kDigits];
	int  n = 0;
	for (const char *c = code; c && *c; ++c)
	{
		if (*c == ' ' || *c == '-') continue;
		if (*c < '0' || *c > '9' || n == kDigits) return false;
		digits[n++] = *c;
	}
	if (n != kDigits) return false;

	std::memcpy(g_state.next, digits, kDigits);
	g_state.pending = 1;
	std::memcpy(g_last, digits, kDigits);
	g_last[kDigits] = 0;
	g_connected = true;
	return true;
}

bool BarcodeBoyPending() { return g_state.pending || g_state.busy; }
const char *BarcodeBoyLastCode() { return g_last; }
uint32_t BarcodeBoyReadCount() { return g_state.reads; }

uint8_t BarcodeBoyExchange(uint8_t out)
{
	State &st = g_state;
	// Its answer is loaded before the byte arrives: the knock's second half
	// echoes the first.
	st.reply = st.matched == 2 ? kKnock[0] : st.matched == 3 ? kKnock[1] : 0xFF;
	if (out == kKnock[st.matched]) ++st.matched;
	else st.matched = (out == kKnock[0]) ? 1 : 0;
	if (st.matched == 4)
	{
		st.matched = 0;
		st.armed   = 1;
	}
	// The game is talking, so it is not taking a card.
	st.quiet = 0;
	st.busy  = 0;
	return st.reply;
}

uint8_t BarcodeBoyReply() { return g_state.reply; }

int BarcodeBoyClock(int32_t cycles, bool listening)
{
	State &st = g_state;
	if (st.quiet < kSettle) st.quiet += cycles;
	if (st.timer > 0) st.timer -= cycles;

	if (!st.busy)
	{
		if (!st.pending || !st.armed || !listening || st.quiet < kSettle) return -1;
		std::memcpy(st.card, st.next, kDigits);
		st.pending = 0;
		st.armed   = 0;
		st.busy    = 1;
		st.pos     = 0;
		st.timer   = 0;
		st.stall   = 0;
	}
	if (!listening)
	{
		st.stall += cycles;
		if (st.stall >= kStall) st.busy = 0;
		return -1;
	}
	st.stall = 0;
	if (st.timer > 0) return -1;

	const int i = st.pos % (kDigits + 2);
	const uint8_t b = i == 0 ? kStx : i == kDigits + 1 ? kEtx
	                : static_cast<uint8_t>(st.card[i - 1]);
	st.timer = kBytePeriod;
	if (++st.pos == kCardBytes)
	{
		st.busy = 0;
		++st.reads;
	}
	return b;
}

size_t BarcodeBoyStateBytes() { return kStateBytes; }

void BarcodeBoyStateSave(uint8_t *out)
{
	const State &st = g_state;
	uint8_t *p = out;
	*p++ = st.matched;
	*p++ = st.armed;
	*p++ = st.reply;
	*p++ = st.busy;
	*p++ = st.pos;
	*p++ = st.pending;
	std::memcpy(p, st.next, kDigits); p += kDigits;
	std::memcpy(p, st.card, kDigits); p += kDigits;
	Put32(p, static_cast<uint32_t>(st.timer));
	Put32(p, static_cast<uint32_t>(st.quiet));
	Put32(p, static_cast<uint32_t>(st.stall));
	Put32(p, st.reads);
}

void BarcodeBoyStateLoad(const uint8_t *in)
{
	State &st = g_state;
	const uint8_t *p = in;
	st.matched = static_cast<uint8_t>(*p++ & 3);
	st.armed   = *p++ ? 1 : 0;
	st.reply   = *p++;
	st.busy    = *p++ ? 1 : 0;
	st.pos     = *p++;
	st.pending = *p++ ? 1 : 0;
	std::memcpy(st.next, p, kDigits); p += kDigits;
	std::memcpy(st.card, p, kDigits); p += kDigits;
	st.timer = static_cast<int32_t>(Get32(p));
	st.quiet = static_cast<int32_t>(Get32(p));
	st.stall = static_cast<int32_t>(Get32(p));
	st.reads = Get32(p);
	if (st.pos >= kCardBytes) st.busy = 0;
}

const BarcodeBoyCard *BarcodeBoyCards(int *count)
{
	if (count) *count = static_cast<int>(sizeof kCards / sizeof kCards[0]);
	return kCards;
}

const char *BarcodeBoyGame(const char *title)
{
	if (!title) return nullptr;
	for (const auto &g : kGames)
		if (std::strcmp(title, g.title) == 0) return g.game;
	return nullptr;
}

} // namespace SGB
