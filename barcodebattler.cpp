/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <string.h>
#include <stdio.h>

#include "snes9x.h"
#include "memmap.h"
#include "display.h"
#include "barcodebattler.h"

// Digits are written by the frontend's thread and read on the emulation
// thread; the pending flag is published last and cleared once the game has
// clocked the digits out.
static uint8				bb_digits[13];
static char					bb_last[14];
static std::atomic<bool>	bb_pending{false};

void S9xBarcodeBattlerConnect (bool connect)
{
	Settings.BarcodeBattler = connect;
	if (!connect)
		bb_pending.store(false);
}

bool S9xBarcodeBattlerConnected (void)
{
	return Settings.BarcodeBattler;
}

// Strips spaces and dashes; returns the digit count, or -1 for any other
// character or more than 13 digits.
static int NormalizeCode (const char *code, char out[14])
{
	int	n = 0;

	if (!code)
		return (-1);

	for (; *code; code++)
	{
		if (*code == ' ' || *code == '-')
			continue;
		if (*code < '0' || *code > '9' || n == 13)
			return (-1);
		out[n++] = *code;
	}

	out[n] = 0;
	return (n);
}

bool S9xBarcodeBattlerScan (const char *code)
{
	char	digits[14], full[14];
	int		n = NormalizeCode(code, digits);

	if (n != 13 && n != 12 && n != 8)
		return (false);

	// Leading zeros make up the 13 digits the interface always sends.
	memset(full, '0', 13 - n);
	memcpy(full + 13 - n, digits, n + 1);

	bb_pending.store(false);
	for (int i = 0; i < 13; i++)
		bb_digits[i] = full[i] - '0';
	strcpy(bb_last, full);

	Settings.BarcodeBattler = TRUE;
	bb_pending.store(true, std::memory_order_release);
	return (true);
}

bool S9xBarcodeBattlerPending (void)
{
	return (bb_pending.load());
}

void S9xBarcodeBattlerClear (void)
{
	bb_pending.store(false);
}

const char *S9xBarcodeBattlerLastCode (void)
{
	return (bb_last);
}

// The Super Famicom games that read the Barcode Battler, by No-Intro CRC32.
static const uint32	bb_games[] =
{
	0x115233DB,	// Conveni Wars Barcode Battler Senki
	0xF409C139,	// Dragon Slayer: Eiyuu Densetsu II
	0x7A585763,	// Hatayama Hatch no Pro Yakyuu News! Jitsumei Ban
	0xD343B398,	// Doraemon 2: Nobita no Toys Land Daibouken
	0xD96B3936,	// J.League Excite Stage '94
	0xC9E33615,	// J.League Excite Stage '94 (Rev 1)
	0x560C5EAB,	// Doraemon 3: Nobita to Toki no Hougyoku
	0x74ADC6E2,	// Lupin Sansei: Densetsu no Hihou o Oe!
	0x82F7F710,	// The Amazing Spider-Man: Lethal Foes
	0xDD871511,	// J.League Excite Stage '95
	0x1ED5D2FA,	// Donald Duck no Mahou no Boushi
	0x05798DA8,	// Alice no Paint Adventure
	0x4097BC01	// Doraemon 4: Nobita to Tsuki no Oukoku (fullsnes only)
};

bool S9xBarcodeBattlerGameSupported (void)
{
	for (uint32 crc : bb_games)
		if (Memory.ROMCRC32 == crc)
			return (true);

	return (false);
}

uint8 S9xBarcodeBattlerReadBit (int idx)
{
	if (idx < 12)
		return (0);
	if (idx < 16)
		return ((0x0e >> (15 - idx)) & 1);
	if (idx < 24)
		return (0);	// extended id 00h

	bool	pending = bb_pending.load(std::memory_order_acquire);

	if (idx == 24)
		return (pending ? 1 : 0);
	if (idx < 28)
		return (0);
	if (idx < 80)
	{
		if (!pending)
			return (0);

		int		d = (idx - 28) >> 2;
		uint8	bit = (bb_digits[d] >> (3 - ((idx - 28) & 3))) & 1;

		// The last digit bit hands the code over: the interface has nothing
		// more to send until the next scan.
		if (idx == 79)
		{
			bb_pending.store(false);
			char	msg[64];
			snprintf(msg, sizeof(msg), "Barcode Battler: %s read by the game", bb_last);
			S9xSetInfoString(msg);
		}

		return (bit);
	}

	return (1);
}

uint16 S9xBarcodeBattlerAutoJoypad (void)
{
	return (0x000e);
}

bool S9xBarcodeChecksumValid (const char *code)
{
	char	digits[14];
	int		n = NormalizeCode(code, digits);

	if (n != 8 && n != 12 && n != 13)
		return (false);

	// From the check digit leftwards, weights alternate 1, 3, 1, 3...
	int	sum = 0;
	for (int i = 0; i < n; i++)
		sum += (digits[n - 1 - i] - '0') * ((i & 1) ? 3 : 1);

	return (sum % 10 == 0);
}

void S9xBarcodeRandom (char out[14])
{
	static std::mt19937	rng(std::random_device{}());
	std::uniform_int_distribution<int>	digit(0, 9);

	int	sum = 0;
	for (int i = 0; i < 12; i++)
	{
		out[i] = '0' + digit(rng);
		sum += (out[i] - '0') * ((i & 1) ? 3 : 1);
	}

	out[12] = '0' + (10 - sum % 10) % 10;
	out[13] = 0;
}

// ---------------------------------------------------------------------------
// Barcode decoding: EAN-13 (UPC-A being EAN-13 with a leading zero) and EAN-8.

// Element widths of the L codes in modules: space, bar, space, bar. G codes
// are the same read backwards; R codes have the L widths with the colours
// swapped, which run lengths don't see.
static const int	ean_l[10][4] =
{
	{ 3, 2, 1, 1 }, { 2, 2, 2, 1 }, { 2, 1, 2, 2 }, { 1, 4, 1, 1 }, { 1, 1, 3, 2 },
	{ 1, 2, 3, 1 }, { 1, 1, 1, 4 }, { 1, 3, 1, 2 }, { 1, 2, 1, 3 }, { 3, 1, 1, 2 }
};

// L/G parity of the six left digits (bit 5 = first, 1 = G) for each value of
// the implied 13th digit.
static const int	ean_first[10] = { 0x00, 0x0b, 0x0d, 0x0e, 0x13, 0x19, 0x1c, 0x15, 0x16, 0x1a };

// Best digit for four runs, or -1. *g set when it matched a G code.
static int MatchDigit (const int *r, bool allow_g, bool *g)
{
	int		total = r[0] + r[1] + r[2] + r[3];
	float	best = 1e9f;
	int		best_d = -1;
	bool	best_g = false;

	if (total < 7)
		return (-1);

	for (int parity = 0; parity < (allow_g ? 2 : 1); parity++)
	{
		for (int d = 0; d < 10; d++)
		{
			float	var = 0.0f, worst = 0.0f;

			for (int k = 0; k < 4; k++)
			{
				int		p = parity ? ean_l[d][3 - k] : ean_l[d][k];
				float	e = r[k] * 7.0f / total - p;
				if (e < 0.0f)
					e = -e;
				var += e;
				if (e > worst)
					worst = e;
			}

			if (worst < 0.75f && var < best)
			{
				best = var;
				best_d = d;
				best_g = parity != 0;
			}
		}
	}

	if (best > 1.9f)
		return (-1);

	if (g)
		*g = best_g;
	return (best_d);
}

// Runs of a guard all near one module of width m.
static bool GuardOK (const int *r, int count, float m)
{
	for (int k = 0; k < count; k++)
		if (r[k] < m * 0.4f || r[k] > m * 2.0f)
			return (false);

	return (true);
}

// Tries an EAN-13 or EAN-8 starting with the bar run r[0]. r[-1] is the
// quiet zone before it (callers guarantee it exists).
static bool DecodeAt (const int *r, int avail, char out[14])
{
	float	m = (r[0] + r[1] + r[2]) / 3.0f;

	if (m < 0.75f || r[-1] < m * 3.0f || !GuardOK(r, 3, m))
		return (false);

	// EAN-13: guard, 6 digits, middle guard, 6 digits, guard = 59 runs.
	if (avail >= 59 && GuardOK(r + 27, 5, m) && GuardOK(r + 56, 3, m))
	{
		char	d[14];
		int		parity = 0;
		bool	ok = true;

		for (int i = 0; i < 6 && ok; i++)
		{
			bool	g = false;
			int		v = MatchDigit(r + 3 + i * 4, true, &g);
			ok = v >= 0;
			d[1 + i] = '0' + v;
			parity = (parity << 1) | (g ? 1 : 0);
		}

		for (int i = 0; i < 6 && ok; i++)
		{
			int	v = MatchDigit(r + 32 + i * 4, false, NULL);
			ok = v >= 0;
			d[7 + i] = '0' + v;
		}

		if (ok)
		{
			int	first = -1;
			for (int i = 0; i < 10; i++)
				if (ean_first[i] == parity)
					first = i;

			d[0] = '0' + first;
			d[13] = 0;
			if (first >= 0 && S9xBarcodeChecksumValid(d))
			{
				strcpy(out, d);
				return (true);
			}
		}
	}

	// EAN-8: guard, 4 L digits, middle guard, 4 R digits, guard = 43 runs.
	if (avail >= 43 && GuardOK(r + 19, 5, m) && GuardOK(r + 40, 3, m))
	{
		char	d[9];
		bool	ok = true;

		for (int i = 0; i < 4 && ok; i++)
		{
			int	v = MatchDigit(r + 3 + i * 4, false, NULL);
			ok = v >= 0;
			d[i] = '0' + v;
		}

		for (int i = 0; i < 4 && ok; i++)
		{
			int	v = MatchDigit(r + 24 + i * 4, false, NULL);
			ok = v >= 0;
			d[4 + i] = '0' + v;
		}

		d[8] = 0;
		if (ok && S9xBarcodeChecksumValid(d))
		{
			strcpy(out, d);
			return (true);
		}
	}

	return (false);
}

// Runs of dark/light across one line of samples. Thresholds against the
// local contrast so uneven lighting across the picture doesn't matter.
static void ScanLine (const std::vector<uint8> &line, std::map<std::string, int> &votes)
{
	int	n = (int) line.size();
	if (n < 60)
		return;

	std::vector<int>	v(n);
	for (int x = 0; x < n; x++)
		v[x] = (line[x > 0 ? x - 1 : 0] + 2 * line[x] + line[x < n - 1 ? x + 1 : n - 1]) >> 2;

	int					block = n / 24 < 8 ? 8 : n / 24;
	int					blocks = (n + block - 1) / block;
	std::vector<int>	bmin(blocks, 255), bmax(blocks, 0);

	for (int x = 0; x < n; x++)
	{
		int	b = x / block;
		if (v[x] < bmin[b]) bmin[b] = v[x];
		if (v[x] > bmax[b]) bmax[b] = v[x];
	}

	std::vector<int>	runs;
	bool				dark_prev = false;
	bool				first_dark = false;
	int					len = 0;

	for (int x = 0; x < n; x++)
	{
		int	b = x / block, lo = 255, hi = 0;
		for (int k = b - 2; k <= b + 2; k++)
		{
			if (k < 0 || k >= blocks)
				continue;
			if (bmin[k] < lo) lo = bmin[k];
			if (bmax[k] > hi) hi = bmax[k];
		}

		bool	dark = (hi - lo >= 24) && v[x] < (lo + hi) / 2;

		if (x == 0)
		{
			first_dark = dark;
			dark_prev = dark;
		}

		if (dark != dark_prev)
		{
			runs.push_back(len);
			len = 0;
			dark_prev = dark;
		}

		len++;
	}

	runs.push_back(len);

	// Both reading directions: the barcode may be upside down or mirrored.
	for (int dir = 0; dir < 2; dir++)
	{
		// First dark run with a light run before it to serve as quiet zone.
		char	code[14];

		for (int i = first_dark ? 2 : 1; i < (int) runs.size(); i += 2)
		{
			if (DecodeAt(&runs[i], (int) runs.size() - i, code))
			{
				votes[code]++;
				break;
			}
		}

		// Reverse for the second pass. With an even run count the colours at
		// the two ends differ.
		std::vector<int>	rev(runs.rbegin(), runs.rend());
		runs.swap(rev);
		if (!(runs.size() & 1))
			first_dark = !first_dark;
	}
}

bool S9xBarcodeDecode (const uint8 *luma, int width, int height, int stride, char out[14])
{
	std::map<std::string, int>	votes;
	std::vector<uint8>			line;

	if (!luma || width <= 0 || height <= 0)
		return (false);

	int	row_step = height / 96 > 1 ? height / 96 : 1;
	int	col_step = width / 96 > 1 ? width / 96 : 1;

	line.resize(width);
	for (int y = 0; y < height; y += row_step)
	{
		memcpy(line.data(), luma + (size_t) y * stride, width);
		ScanLine(line, votes);
	}

	line.resize(height);
	for (int x = 0; x < width; x += col_step)
	{
		for (int y = 0; y < height; y++)
			line[y] = luma[(size_t) y * stride + x];
		ScanLine(line, votes);
	}

	// Two lines agreeing keeps a lucky misread on one line from winning.
	const std::string	*best = NULL;
	int					best_votes = 0;

	for (auto &v : votes)
	{
		if (v.second > best_votes)
		{
			best = &v.first;
			best_votes = v.second;
		}
	}

	if (!best || best_votes < 2)
		return (false);

	strcpy(out, best->c_str());
	return (true);
}
