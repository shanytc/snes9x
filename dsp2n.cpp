/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native DSP-2 (Dungeon Master), on the kit in upd7725np.h.

#include <string.h>
#include "snes9x.h"
#include "upd7725n.h"

namespace
{

#define NT_IDLE_WAIT	2
#include "upd7725np.h"

uint16	rom[1024];		// the data ROM

// A command's working RAM starts at 0 (the wait clears A, and DP takes A), stepping within a row of 16.
inline uint8 Step (uint8 dp, int d)
{
	return ((uint8) ((dp & 0xf0) | ((dp + d) & 0x0f)));
}

inline uint16 Swap (uint16 v)
{
	return ((uint16) (v << 8 | v >> 8));
}

// the ALU's two-bit shift, which shifts in ones
inline uint16 Shl2 (uint16 v)
{
	return ((uint16) (v << 2 | 3));
}

// Four bytes' bit pairs as one word: their table entries folded in two bits at a time.
uint16 Fold (uint16 a, uint16 b, uint16 c, uint16 d)
{
	return ((uint16) (Shl2((uint16) (Shl2((uint16) (Shl2((uint16) (3 ^ a)) ^ b)) ^ c)) ^ d));
}

// From four bytes: the low table (bits 0, 1, 4, 5) folded into one word, the high table
// (bits 2, 3, 6, 7) into another; rows $00-$30 keep the high table's entries.
void Pairs (const int16 *in, uint16 &lo, uint16 &hi)
{
	uint16	t[2][4];
	for (int i = 0; i < 4; i++)
	{
		t[0][i] = rom[(uint8) in[i]];
		t[1][i] = rom[0x200 + (uint8) in[i]];
		ram[i << 4] = t[1][i];
	}
	lo = Fold(t[0][0], t[0][1], t[0][2], t[0][3]);
	hi = Fold(t[1][0], t[1][1], t[1][2], t[1][3]);
}

// $01's waits between its 32 results: two loops that take turns
const uint8	PlaneGaps[32] =
{
	14, 3, 3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 3, 4, 4, 4,
	 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4, 0
};

// $02's waits before a tile's 32 results: the first tile's, then the others'. The last tile's
// last two differ: it skips the count check.
const uint8	TileGaps[2][32] =
{
	{ 12, 4, 4, 4, 5, 5, 4, 4, 5, 5, 4, 4, 5, 5, 3, 5, 5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 5, 5 },
	{  5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 4, 4, 5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 4, 5, 5, 5, 5, 5 }
};

// Two-bit spreading tables, regular enough to build, and a handful of constants.
void BuildDataROM (void)
{
	for (int i = 0; i < 0x400; i++)
		rom[i] = 0xffff;
	for (int i = 0; i < 0x100; i++)
	{
		rom[i] = (uint16) (((i >> 1 & 1) | (i >> 4 & 2)) << 8 | (~i & 1) | (~i >> 3 & 2));
		rom[0x200 + i] = (uint16) (((i >> 3 & 1) | (i >> 6 & 2)) << 8 | (~i >> 2 & 1) | (~i >> 5 & 2));
	}
	static const uint16	tail[16] =
	{
		0x0000, 0x0000, 0x0000, 0x0000, 0x1010, 0x0000, 0x008f, 0x7f81,
		0x00f0, 0x000f, 0x7f80, 0x00f0, 0x0050, 0x0880, 0x000f, 0x00ff
	};
	memcpy(rom + 0x3f0, tail, sizeof(tail));
}

// ---- power-on and the command wait

bool Boot (void)			// $000: 8-bit mode
{
	NT_BEGIN
	NT_SR(1, 0x0400);
	NT_END
}

bool Idle (void)			// $001-$004: DR = the last command's A (then A = 0), wait for a command
{
	NT_BEGIN
	NT_BURN(1); NT_WRITE(1, s.acc);
	NT_WAIT(2); NT_READ(3, s.in[0]);
	s.cmd = (uint8) s.in[0];
	NT_BURN(9);				// the dispatch: a SHR1 and a jump a bit, then the CALL
	NT_END
}

bool Planes (void)			// $00: four bytes' bit pairs, as two words and their byte swaps
{
	NT_BEGIN
	NT_BURN(3); NT_WAIT(1); NT_READ(2, s.in[0]);
	NT_BURN(5); NT_WAIT(3); NT_READ(4, s.in[1]);
	NT_BURN(5); NT_WAIT(5); NT_READ(6, s.in[2]);
	NT_BURN(5); NT_WAIT(7); NT_READNF(8, s.in[3]);
	{
		uint16	lo, hi;
		Pairs(s.in, lo, hi);
		s.out[0] = (int16) lo;
		s.out[1] = (int16) hi;
	}
	NT_BURN(4); NT_WRITE(9, s.out[0]);
	NT_BURN(5); NT_WAIT(10); NT_WRITE(11, Swap((uint16) s.out[0]));
	NT_BURN(1); NT_WAIT(12); NT_WRITE(13, s.out[1]);
	NT_WAIT(14); NT_WRITE(15, Swap((uint16) s.out[1]));
	NT_WAIT(16); NT_BURN(2);
	s.acc = 0;
	NT_END
}

bool Planes32 (void)		// $01: 32 bytes of 4-bit pixels to bitplanes: eight $00s, low words first
{
	NT_BEGIN
	// byte 4g + q: its tables' entries in column 8 + g, rows q and 4 + q
	NT_BURN(4);
	for (s.i = 0; s.i < 32; s.i++)
	{
		if (s.i)
			NT_BURN((s.i & 3) == 2 ? 4 : 5);
		NT_WAIT(1); NT_READ(2, s.in[0]);
		ram[(s.i & 3) << 4 | (8 + (s.i >> 2))] = rom[(uint8) s.in[0]];
		ram[(4 + (s.i & 3)) << 4 | (8 + (s.i >> 2))] = rom[0x200 + (uint8) s.in[0]];
	}
	for (s.i = 0; s.i < 32; s.i++)
	{
		{
			const int	col = 8 + (s.i >> 1 & 7), row = s.i < 16 ? 0 : 4;
			const uint16	v = Fold(ram[row << 4 | col], ram[(row + 1) << 4 | col], ram[(row + 2) << 4 | col], ram[(row + 3) << 4 | col]);
			s.out[0] = (int16) ((s.i & 1) ? Swap(v) : v);
		}
		if (!s.i)
		{
			NT_BURN(PlaneGaps[0]); NT_WRITE(3, s.out[0]);
		}
		else
		{
			NT_BURN(PlaneGaps[s.i]); NT_WAIT(4); NT_WRITE(5, s.out[0]);
		}
	}
	NT_WAIT(6); NT_BURN(2);
	s.acc = 3;
	NT_END
}

// $02's tile t, row q (0-3) of 8 pixels in 4 bytes: column 7 - q or 8 + q, rows 0-7 or 8-15 of RAM
inline int TileColumn (int t, int q)
{
	return ((t & 1) ? 8 + q : 7 - q);
}

bool Tiles (void)			// $02: up to four 8x8 tiles of 4-bit pixels (4 bytes a row) to 4bpp planar
{
	NT_BEGIN
	NT_WAIT(1); NT_READ(2, s.in[0]);
	// 1-3 tiles, anything else 4
	s.in[1] = (uint8) s.in[0] >= 1 && (uint8) s.in[0] <= 3 ? (uint8) s.in[0] : 4;
	for (s.i = 0; s.i < 32 * s.in[1]; s.i++)
	{
		NT_BURN(5); NT_WAIT(3);
		if (s.i < 32 * s.in[1] - 1)
		{
			NT_READ(4, s.in[2]);
		}
		else
		{
			NT_READNF(5, s.in[2]);
		}
		{
			const int	t = s.i >> 5, rows = (t & 2) << 2, col = TileColumn(t, s.i >> 2 & 7), q = s.i & 3;
			ram[(rows + q) << 4 | col] = rom[(uint8) s.in[2]];
			ram[(rows + 4 + q) << 4 | col] = rom[0x200 + (uint8) s.in[2]];
		}
	}
	// per row, two bytes for planes 0-1 (as a word, then its swap), then planes 2-3
	for (s.i = 0; s.i < 32 * s.in[1]; s.i++)
	{
		{
			const int	t = s.i >> 5, k = s.i & 31, rows = ((t & 2) << 2) + (k & 16 ? 4 : 0), col = TileColumn(t, k >> 1 & 7);
			const uint16	v = Fold(ram[rows << 4 | col], ram[(rows + 1) << 4 | col], ram[(rows + 2) << 4 | col], ram[(rows + 3) << 4 | col]);
			s.out[0] = (int16) ((k & 1) ? Swap(v) : v);
			s.in[3] = TileGaps[t ? 1 : 0][k];
			if (s.i >= 32 * s.in[1] - 2)
				s.in[3] = (k & 1) ? 0 : (t == 3 ? 2 : 3);	// past the count check
		}
		if (!s.i)
		{
			NT_BURN(s.in[3]); NT_WRITE(6, s.out[0]);
		}
		else
		{
			NT_BURN(s.in[3]); NT_WAIT(7); NT_WRITE(8, s.out[0]);
		}
	}
	NT_WAIT(9); NT_BURN(2);
	s.acc = 0;
	NT_END
}

bool Transparent (void)		// $03: the transparent colour, in both nibbles of TRB (x $11 by the multiplier)
{
	NT_BEGIN
	NT_BURN(2); NT_WAIT(1); NT_READNF(2, s.in[0]);
	*trb = (uint16) Mul((int16) Swap(s.in[0] & rom[0x3fe]), (int16) rom[0x3fd]);
	s.acc = 0;
	NT_BURN(5);
	NT_END
}

bool Blend (void)			// $04: one byte, nibble by nibble: the second's unless it's transparent
{
	NT_BEGIN
	NT_WAIT(1); NT_READ(2, s.in[0]);
	NT_BURN(4); NT_WAIT(3); NT_READNF(4, s.in[1]);
	{
		const uint8		dp = 0;
		const uint16	a = s.in[0] & 0xff, b = s.in[1] & 0xff, t = b ^ *trb;
		const uint16	lo = (t & rom[0x3f9]) ? rom[0x3f9] : 0, hi = (t & rom[0x3f8]) ? rom[0x3f8] : 0;
		ram[dp] = a;
		ram[Step(dp, 1)] = b;
		ram[Step(dp, 2)] = lo;
		s.out[0] = (int16) ((~(hi | lo) & a) | ((hi | lo) & b));
		s.i = 13 + (lo ? 1 : 0) + (hi ? 1 : 0);
	}
	NT_BURN(s.i - 1); NT_WRITE(5, s.out[0]);
	NT_WAIT(6); NT_BURN(2);
	s.acc = (uint16) s.out[0];
	NT_END
}

bool Multiply (void)		// $09/$0A: a signed 16 x 16 product as four bytes
{
	NT_BEGIN
	ram[0] = 0x7fff;		// a mask
	NT_BURN(1); NT_WAIT(1); NT_READ(2, s.in[0]);
	NT_BURN(2); NT_WAIT(3); NT_READ(4, s.in[1]);
	NT_BURN(3); NT_WAIT(5); NT_READ(6, s.in[2]);
	NT_BURN(2); NT_WAIT(7); NT_READNF(8, s.in[3]);
	{
		const int16		w1 = (int16) ((s.in[0] & 0xff) | (s.in[1] & 0xff) << 8);
		const int16		w2 = (int16) ((s.in[2] & 0xff) | (s.in[3] & 0xff) << 8);
		const int32		p = (int32) w1 * w2;
		// the multiplier's halves, each shifted back by one: the low word's bit 15 repeats bit 14
		const uint16	n = (uint16) (p << 1), m = (uint16) (p >> 15);
		const uint16	lo = (uint16) ((n >> 1) | (n & 0x8000)), hi = (uint16) ((int16) m >> 1 & 0x7fff);
		s.out[0] = (int16) lo;
		s.out[1] = (int16) Swap(lo);
		s.out[2] = (int16) hi;
		s.out[3] = (int16) Swap(hi);
	}
	NT_BURN(6); NT_WRITE(9, s.out[0]);
	NT_WAIT(10); NT_WRITE(11, s.out[1]);
	NT_BURN(3); NT_WAIT(12); NT_WRITE(13, s.out[2]);
	NT_WAIT(14); NT_WRITE(15, s.out[3]);
	NT_WAIT(16); NT_BURN(2);
	s.acc = (uint16) s.out[3];
	NT_END
}

// $05 keeps position j's bottom byte at j, its top at $50 + j and the top's mask at $A0 + j.
inline void OverlayTop (int j, uint16 top, uint16 lead)
{
	ram[0x50 + j] = top;
	ram[0xa0 + j] = (uint16) ((uint16) (lead + top) ^ *trb);
}

// one position of $05's mask pass: the top's nibbles that aren't transparent, cleared from the bottom
inline uint32 OverlayMask (int j)
{
	const uint16	t = ram[0xa0 + j];
	const uint16	m = (uint16) ((t & 0x0f ? 0x0f : 0) | (t & 0xf0 ? 0xf0 : 0));
	ram[0xa0 + j] = m;
	ram[j] &= (uint16) ~m;
	return (10 + (t & 0x0f ? 1 : 0) + (t & 0xf0 ? 1 : 0));
}

// The pass runs from the last position down; a last one that starts a row of 16 takes in that
// whole row (15 positions of stale RAM) and itself twice. Returns its instructions.
uint32 OverlayMasks (int n)
{
	uint32	k = 0;
	for (int j = 0; j < n; j++)
		k += OverlayMask(j);
	if (n % 16 == 1)
		for (int j = n - 1; j < n + 15; j++)
			k += OverlayMask(j);
	return (k - 1);			// position 0's has no jump back
}

bool Overlay (void)			// $05: n bytes (80 at most) under n more, nibble by nibble: the top's unless transparent
{
	NT_BEGIN
	NT_WAIT(1); NT_READ(2, s.in[0]);
	s.in[1] = s.in[0] & 0xff;
	if (!s.in[1])
	{
		NT_BURN(3); NT_SR(3, 0x4400);
		s.acc = 0;
		NT_BURN(2);
	}
	else
	{
		// past 80, the rest of the count lands on the first top byte's mask
		s.in[2] = s.in[1] > 80 ? 80 : s.in[1];
		NT_BURN(4);
		for (s.i = 0; s.i < s.in[2]; s.i++)
		{
			if (s.i)
				NT_BURN((s.i & 15) ? 3 : 4);
			NT_WAIT(4); NT_READ(5, s.in[3]);
			ram[s.i] = (uint16) s.in[3];
			ram[0xa0 + s.i] = *trb;
		}
		NT_BURN(s.in[1] > 80 ? 5 : 4);
		for (s.i = 0; s.i < s.in[2] - 1; s.i++)
		{
			if (s.i)
				NT_BURN((s.i & 15) ? 5 : 4);
			NT_WAIT(6); NT_READ(7, s.in[3]);
			OverlayTop(s.i, (uint16) s.in[3], (uint16) (s.i ? 0 : s.in[1] - s.in[2]));
		}
		if (s.i)
			NT_BURN(s.in[1] > 80 ? 5 : 4);
		NT_WAIT(8);
		if (s.in[2] > 64)
		{
			NT_SR(9, 0x2400); NT_READNF(10, s.in[3]);
		}
		else
		{
			NT_READNF(11, s.in[3]); NT_SR(12, 0x2400);
		}
		OverlayTop(s.i, (uint16) s.in[3], (uint16) (s.i ? 0 : s.in[1] - s.in[2]));
		NT_BURN(OverlayMasks(s.in[2]) + 5);
		NT_SR(13, 0x0400);
		for (s.i = 0; s.i < s.in[2]; s.i++)
		{
			if (s.i)
			{
				NT_BURN(s.i == 1 || !(s.i & 15) ? 4 : 5); NT_WAIT(14);
			}
			s.out[0] = (int16) ((ram[0x50 + s.i] & ram[0xa0 + s.i]) | ram[s.i]);
			NT_WRITE(15, s.out[0]);
		}
		NT_BURN(s.in[2] == 80 ? 0 : 1);
		NT_WAIT(16); NT_BURN(2);
		s.acc = (uint16) s.out[0];
	}
	NT_END
}

// a byte with its nibbles swapped, by the multiplier ($0808 x 2), in DR's high byte too
inline int16 NibbleSwap (uint16 b)
{
	return ((int16) Swap((uint16) (b * 0x1010)));
}

bool Reverse (void)			// $06: n bytes back last first, nibbles swapped
{
	NT_BEGIN
	NT_BURN(2); NT_WAIT(1); NT_READ(2, s.in[0]);
	s.i = s.in[0] & 0xff;
	if (!s.i)
	{
		// none: an odd status, which also leaves it in 16-bit mode
		NT_BURN(1); NT_SR(3, 0x1130);
		s.acc = 0;
		NT_BURN(2);
	}
	else
	{
		// all but the last byte wait in RAM from 0
		NT_BURN(5);
		for (s.in[1] = 0; s.in[1] < s.i - 1; s.in[1]++)
		{
			NT_WAIT(4); NT_READ(5, s.in[2]);
			ram[s.in[1]] = s.in[2] & 0xff;
			NT_BURN(5);
		}
		NT_WAIT(6); NT_READNF(7, s.in[2]);
		s.out[0] = NibbleSwap(s.in[2] & 0xff);
		if (s.i == 1)
		{
			NT_BURN(6); NT_WAIT(8); NT_WRITE(9, s.out[0]);
		}
		else
		{
			NT_BURN(7); NT_WRITE(10, s.out[0]);
			for (s.in[1] = s.i - 2; s.in[1] >= 0; s.in[1]--)
			{
				s.out[0] = NibbleSwap(ram[s.in[1]]);
				NT_BURN(s.in[1] ? 5 : 4); NT_WAIT(11); NT_WRITE(12, s.out[0]);
			}
		}
		NT_WAIT(13); NT_BURN(2);
		s.acc = 0xffff;
	}
	NT_END
}

// $07/$08: two 32-bit words from four bytes each; out[0-3] the sum's (or difference's) words and their
// byte swaps, out[4] its signed overflow. The operands stay in RAM: this row, then the next.
void AddSub (bool sub)
{
	const uint8		dp = 0;
	uint16	w[4];
	for (int i = 0; i < 4; i++)
		w[i] = (uint16) ((s.in[2 * i] & 0xff) | (s.in[2 * i + 1] & 0xff) << 8);
	const uint32	lo = sub ? (uint32) w[0] - w[2] : (uint32) w[0] + w[2];
	const uint16	c = (uint16) (lo >> 16 & 1);
	const uint16	hi = (uint16) (sub ? w[1] - w[3] - c : w[1] + w[3] + c);
	const uint16	q = sub ? w[1] : w[3], p = sub ? w[3] : w[1];
	s.out[0] = (int16) (uint16) lo;
	s.out[1] = (int16) Swap((uint16) lo);
	s.out[2] = (int16) hi;
	s.out[3] = (int16) Swap(hi);
	s.out[4] = (int16) ((sub ? ((q ^ hi) & (q ^ p)) : ((q ^ hi) & (p ^ hi))) >> 15);
	ram[dp] = w[0];
	ram[Step(dp, 1)] = w[1];
	ram[dp ^ 0x10] = s.in[4] & 0xff;
	ram[Step(dp, 1) ^ 0x10] = sub ? w[3] : (uint16) (s.in[6] & 0xff);
}

bool Add (void)				// $07: 32-bit add
{
	NT_BEGIN
	NT_SR(1, 0x0400);
	NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(2); NT_WAIT(4); NT_READ(5, s.in[1]);
	NT_BURN(4); NT_WAIT(6); NT_READ(7, s.in[2]);
	NT_BURN(2); NT_WAIT(8); NT_READ(9, s.in[3]);
	NT_BURN(4); NT_WAIT(10); NT_READ(11, s.in[4]);
	NT_BURN(2); NT_WAIT(12); NT_READ(13, s.in[5]);
	NT_BURN(4); NT_WAIT(14); NT_READ(15, s.in[6]);
	NT_BURN(2); NT_WAIT(16); NT_READNF(17, s.in[7]);
	AddSub(false);
	NT_BURN(3); NT_WRITE(18, s.out[0]);
	NT_BURN(1); NT_WAIT(19); NT_WRITE(20, s.out[1]);
	NT_BURN(1);
	if (s.out[4])
	{
		NT_SR(21, 0x4400);
	}
	NT_WAIT(22); NT_WRITE(23, s.out[2]);
	NT_BURN(1); NT_WAIT(24); NT_WRITE(25, s.out[3]);
	NT_WAIT(26); NT_BURN(2);
	s.acc = (uint16) s.out[1];
	NT_END
}

bool Subtract (void)		// $08: 32-bit subtract
{
	NT_BEGIN
	NT_SR(1, 0x0400);
	NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(3); NT_WAIT(4); NT_READ(5, s.in[1]);
	NT_BURN(4); NT_WAIT(6); NT_READ(7, s.in[2]);
	NT_BURN(2); NT_WAIT(8); NT_READ(9, s.in[3]);
	NT_BURN(4); NT_WAIT(10); NT_READ(11, s.in[4]);
	NT_BURN(2); NT_WAIT(12); NT_READ(13, s.in[5]);
	NT_BURN(4); NT_WAIT(14); NT_READ(15, s.in[6]);
	NT_BURN(2); NT_WAIT(16); NT_READNF(17, s.in[7]);
	AddSub(true);
	NT_BURN(4); NT_WRITE(18, s.out[0]);
	NT_BURN(2); NT_WAIT(19); NT_WRITE(20, s.out[1]);
	NT_BURN(1);
	if (s.out[4])
	{
		NT_SR(21, 0x4400);
	}
	NT_WAIT(22); NT_WRITE(23, s.out[2]);
	NT_BURN(1); NT_WAIT(24); NT_WRITE(25, s.out[3]);
	NT_WAIT(26); NT_BURN(2);
	s.acc = (uint16) s.out[1];
	NT_END
}

// $0B/$0C keep the quotient at RAM 0-1, the dividend at 2-3, the remainder at 4-5, the divisor at 6-7.
inline uint32 Long (int a)
{
	return ((uint32) ram[a] | (uint32) ram[a + 1] << 16);
}

inline void SetLong (int a, uint32 v)
{
	ram[a] = (uint16) v;
	ram[a + 1] = (uint16) (v >> 16);
}

// a two's complement negation through the ALU: 9 instructions, one more when the low word carries
inline uint32 Negate (int a)
{
	const uint32	v = Long(a);
	SetLong(a, 0 - v);
	return (9 + ((uint16) v == 0 ? 1 : 0));
}

// The chip's restoring division: remainder and dividend shift as one 64-bit value (the remainder's
// top bit falls off), cin feeding its bottom, then the quotient's old top bit. A round subtracts
// when the remainder is at least the divisor (unsigned), or the difference isn't negative (signed).
// Returns the instructions; the rounds count in RAM $12 against $13.
uint32 DivideRounds (int rounds, uint32 cin, bool sgn)
{
	uint32	q = Long(0), d = Long(2), r = Long(4), k = 0;
	const uint32	v = Long(6);
	for (int i = 1; i <= rounds; i++)
	{
		r = r << 1 | d >> 31;
		d = d << 1 | cin;
		const uint32	t = r - v;
		const bool		take = sgn ? !(t >> 31) : r >= v;
		if (take)
		{
			r = t;
			q |= 1;
		}
		if (i < rounds)
		{
			cin = q >> 31;
			q <<= 1;
			k += take ? 25 : 23;
		}
		else
			k += take ? 17 : 11;
	}
	SetLong(0, q);
	SetLong(2, d);
	SetLong(4, r);
	ram[0x12] = (uint16) (rounds - 1);
	ram[0x13] = (uint16) (rounds - 1);
	return (k);
}

// The eight bytes: the dividend's two words, then the divisor's; zero-filled quotient and remainder.
void DivideInputs (void)
{
	for (int i = 0; i < 4; i++)
		ram[i < 2 ? 2 + i : 4 + i] = (uint16) ((s.in[2 * i] & 0xff) | (s.in[2 * i + 1] & 0xff) << 8);
	SetLong(0, 0);
	SetLong(4, 0);
}

// From the last byte's status to the quotient's first result; out[0-3] the quotient and remainder.
uint32 Divide (bool sgn)
{
	uint32	k = 10;
	if (!sgn)
		k += DivideRounds(32, 0, false);
	else
	{
		// the signs (quotient: bit 15, remainder: bit 0), then magnitudes; the last negation's
		// carry feeds the dividend's first shift
		const uint32	sd = ram[3] >> 15, sv = ram[7] >> 15;
		uint32	cin = 0;
		if (sd)
		{
			cin = (uint16) Long(2) == 0;
			k += Negate(2);
		}
		k += 4;
		if (sv)
		{
			cin = (uint16) Long(6) == 0;
			k += 1 + Negate(6);
		}
		k += 9;
		const uint32	d = Long(2);
		SetLong(2, d << 1 | cin);
		k += DivideRounds(31, d >> 31, true);
		k += 3;
		if (sd ^ sv)
			k += Negate(0);
		k += 3;
		if (sd)
			k += Negate(4);
	}
	s.out[0] = (int16) ram[0];
	s.out[1] = (int16) ram[1];
	s.out[2] = (int16) ram[4];
	s.out[3] = (int16) ram[5];
	return (k + 2);
}

bool Division (void)		// $0B/$0C: a 32-bit quotient and remainder, unsigned/signed (truncated)
{
	NT_BEGIN
	NT_SR(1, 0x0400);
	NT_BURN(1); NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(4); NT_WAIT(4); NT_READ(5, s.in[1]);
	NT_BURN(4); NT_WAIT(6); NT_READ(7, s.in[2]);
	NT_BURN(4); NT_WAIT(8); NT_READ(9, s.in[3]);
	NT_BURN(4); NT_WAIT(10); NT_READ(11, s.in[4]);
	NT_BURN(4); NT_WAIT(12); NT_READ(13, s.in[5]);
	NT_BURN(4); NT_WAIT(14); NT_READ(15, s.in[6]);
	// a nonzero divisor low word sets a flag in passing
	NT_BURN((s.in[4] & 0xff) | (s.in[5] & 0xff) ? 4 : 3); NT_WAIT(16); NT_READNF(17, s.in[7]);
	NT_SR(18, 0x2400);
	DivideInputs();
	if (!(ram[6] | ram[7]))
	{
		NT_BURN(5); NT_SR(19, 0x4400);
		s.acc = 0;
		NT_BURN(2);
	}
	else
	{
		NT_BURN(Divide(s.cmd & 1 ? false : true) - 1); NT_WRITE(20, s.out[0]);
		NT_SR(21, 0x0400);
		NT_BURN(2); NT_WAIT(22); NT_WRITE(23, Swap((uint16) s.out[0]));
		NT_WAIT(24); NT_WRITE(25, s.out[1]);
		NT_BURN(2); NT_WAIT(26); NT_WRITE(27, Swap((uint16) s.out[1]));
		NT_BURN(1); NT_WAIT(28); NT_WRITE(29, s.out[2]);
		NT_WAIT(30); NT_WRITE(31, Swap((uint16) s.out[2]));
		NT_WAIT(32); NT_WRITE(33, s.out[3]);
		NT_WAIT(34); NT_WRITE(35, Swap((uint16) s.out[3]));
		NT_WAIT(36); NT_BURN(2);
		s.acc = Swap((uint16) s.out[2]);
	}
	NT_END
}

// $0D's step: in << 10 over out + 1 (RAM 1) in 18 rounds of shift-and-subtract. The quotient's low
// word builds in RAM 3; each carry out of it shifts a one into RAM 4. Returns the instructions.
uint32 ScaleStep (uint16 in)
{
	uint32	k = 0;
	uint16	b = (uint16) (in << 8);
	ram[2] = ram[3] = ram[4] = 0;
	for (int i = 0; i < 18; i++)
	{
		const bool	c = (ram[3] >> 15) != 0;
		ram[3] = (uint16) (ram[3] << 1);
		if (c)
		{
			const uint16	t = (uint16) (ram[4] << 2 | 3);
			ram[4] = (uint16) (t >> 1 | (t & 0x8000));
			k += 5;
		}
		ram[2] = (uint16) (ram[2] << 1 | b >> 15);
		b = (uint16) (b << 1);
		k += 15;
		if (ram[2] >= ram[1])
		{
			ram[2] -= ram[1];
			ram[3] |= 1;
			k += 4;
		}
	}
	return (k + 2);
}

// $0D from its last byte through its first result: the step, the row cleared, then out pixels,
// each from the source pixel at the position before it moves on. RAM: 0 the row's bytes, 1 the
// pixels left, 2/3 the source/row nibble ($80: low), 4-5 the step, 6-7 the position (16.16),
// 8/9 the source/row byte. Returns the instructions, or 0 for a step of 0, which never ends.
uint32 ScaleRow (uint16 in)
{
	uint32	k = 7 + ScaleStep(in);
	// the quotient's two words shifted up 6, each carry crossing into the other
	uint16	lo = ram[3], hi = ram[4];
	bool	ca = false;
	for (int i = 0; i < 6; i++)
	{
		const bool	cb = (lo >> 15) != 0;
		lo = (uint16) (lo << 1 | ca);
		ca = (hi >> 15) != 0;
		hi = (uint16) (hi << 1 | cb);
	}
	ram[2] = ram[3] = 0;
	ram[4] = hi;
	ram[5] = lo;
	ram[6] = ram[7] = 0;
	ram[8] = 0x10;
	ram[9] = 0x78;
	k += 27;
	const uint32	n = ram[0] ? ram[0] : 0x10000;
	for (uint32 i = 0; i < n && i < 0x100; i++)
		ram[(0x78 + i) & 0xff] = 0;
	k += 5 * n + 1;
	for (;;)
	{
		k += 5;
		if (!--ram[1])
			break;
		const uint16	src = ram[(uint8) ram[8]];
		uint16	a = src;
		if (ram[2] == ram[3])
			k += 9;
		else
		{
			a = (uint16) ((src * 0x1010u) >> 8 & 0xff);		// its nibbles swapped, by the multiplier
			k += 12;
		}
		uint16	&d = ram[(uint8) ram[9]];
		if (ram[3])
		{
			d = (uint16) ((a & 0x0f) | (d & 0xf0));
			k += 4 + 8 + 7;
		}
		else
		{
			d = (uint16) ((a & 0xf0) | (d & 0x0f));
			k += 4 + 9 + 7;
		}
		ram[3] ^= 0x80;
		if (!ram[3])
		{
			ram[9]++;
			k += 4;
		}
		// the step, added until the position's whole part moves
		const uint32	step = (uint32) ram[4] << 16 | ram[5];
		if (!step)
			return (0);
		const uint16	old = ram[6];
		uint32	pos = (uint32) ram[6] << 16 | ram[7];
		do
		{
			pos += step;
			k += 11;
		} while ((uint16) (pos >> 16) == old);
		ram[6] = (uint16) (pos >> 16);
		ram[7] = (uint16) pos;
		// half the move in bytes (SHR1 keeps the sign), the odd pixel in the nibble
		const uint16	move = (uint16) (ram[6] - old);
		ram[8] += (uint16) (move >> 1 | (move & 0x8000));
		k += 6;
		if (move & 1)
		{
			ram[2] ^= 0x80;
			k += 5;
			if (!ram[2])
			{
				ram[8]++;
				k += 5;
			}
		}
	}
	return (k + 4);
}

bool Scale (void)			// $0D: a row of in 4-bit pixels resized to out pixels
{
	NT_BEGIN
	NT_BURN(2); NT_WAIT(1); NT_READ(2, s.in[0]);
	NT_BURN(5); NT_WAIT(3); NT_READ(4, s.in[1]);
	ram[1] = (uint16) ((s.in[1] & 0xff) + 1);
	ram[0] = (uint16) (ram[1] >> 1);
	// (in + 1) / 2 bytes from RAM $10, counted in 16 bits; a DP that wraps lets bit 8 through
	s.i = (uint16) ((((s.in[0] & 0xff) + 1) >> 1) - 1);
	if (!s.i)
		s.i = 0x10000;
	s.in[2] = 0x10;
	s.in[3] = 0xff;
	for (; s.i; s.i--)
	{
		NT_BURN(5); NT_WAIT(5); NT_READ(6, s.in[4]);
		ram[(uint8) s.in[2]] = (uint16) (s.in[4] & s.in[3]);
		s.in[2] = (uint8) (s.in[2] + 1);
		s.in[3] = s.in[2] ? 0xff : 0x1ff;
	}
	NT_BURN(5); NT_WAIT(7); NT_READNF(8, s.in[4]);
	ram[(uint8) s.in[2]] = (uint16) (s.in[4] & s.in[3]);
	NT_SR(9, 0x4400);
	s.i = (int32) ScaleRow((uint16) (s.in[0] & 0xff));
	if (!s.i)
	{
		for (;;)
		{
			NT_BURN(0x10000); NT_AT(10);
		}
	}
	NT_BURN(s.i - 1); NT_WRITE(11, ram[0x78]);
	NT_SR(12, 0x0400);
	// the row's bytes, counted in 16 bits
	s.in[5] = 0x78;
	s.in[6] = 2;
	s.i = ram[0] ? ram[0] : 0x10000;
	while (--s.i)
	{
		s.in[5] = (uint8) (s.in[5] + 1);
		NT_BURN(s.in[6]); NT_WAIT(13); NT_WRITE(14, ram[(uint8) s.in[5]]);
		s.in[6] = 4;
	}
	NT_BURN(s.in[6]); NT_WAIT(15); NT_BURN(2);
	s.acc = 0;
	NT_END
}

bool Diagnostic (void)		// $0E: by the command's bits 4-5, a RAM test, the data ROM, the version, or nothing
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	s.i = s.cmd >> 4 & 3;
	if (s.i == 3)
	{
		NT_BURN(4); NT_SR(2, 0x0400);
		s.acc = 0xff;
	}
	else
	{
		if (!s.i)
		{
			// both checkerboards written and read back; a word from the CPU lets it start checking
			NT_BURN(776); NT_WAIT(3);
			NT_BURN(3335); NT_WRITE(4, 0);
			for (int a = 0; a < 256; a++)
				ram[a] = (a & 1) ? 0x5555 : 0xaaaa;
			*trb = 0xaaaa;
			s.acc = 0xffff;
		}
		else if (s.i == 1)
		{
			NT_BURN(8);
			for (s.i = 0; s.i < 0x400; s.i++)
			{
				if (s.i)
					NT_BURN(2);
				NT_WAIT(5); NT_WRITE(6, rom[s.i]);
			}
			NT_BURN(1);
			s.acc = 0x400;
		}
		else
		{
			NT_BURN(4); NT_WAIT(7); NT_WRITE(8, 0x0100);
			s.acc = 0xff;
		}
		NT_WAIT(9); NT_BURN(2); NT_SR(10, 0x0400);
	}
	NT_BURN(2);
	NT_END
}

bool Mode (void)			// $0F: back to 8-bit mode
{
	NT_BEGIN
	NT_SR(1, 0x0400);
	NT_BURN(2);
	s.acc = rom[0x3ff];
	NT_END
}

bool Unknown (void)			// not yet native
{
	NT_BEGIN
	NT_BURN(0);
	NT_END
}

bool (*Program (uint8 cmd)) (void)
{
	switch (cmd & 0x0f)
	{
		case 0x00:	return (Planes);
		case 0x01:	return (Planes32);
		case 0x02:	return (Tiles);
		case 0x03:	return (Transparent);
		case 0x04:	return (Blend);
		case 0x05:	return (Overlay);
		case 0x06:	return (Reverse);
		case 0x07:	return (Add);
		case 0x08:	return (Subtract);
		case 0x09:
		case 0x0a:	return (Multiply);
		case 0x0b:
		case 0x0c:	return (Division);
		case 0x0d:	return (Scale);
		case 0x0e:	return (Diagnostic);
		case 0x0f:	return (Mode);
		default:	return (Unknown);
	}
}

void ChipReset (bool variant)
{
	(void) variant;
	BuildDataROM();
}

void ChipLoaded (void)
{
	BuildDataROM();
}

}	// namespace

const S9xUPD7725Native	S9xDSP2Native = NT_CHIP;

#ifdef UPD7725N_LAB
const uint16 *S9xDSP2NLabROM (void)
{
	return (rom);
}
#endif
