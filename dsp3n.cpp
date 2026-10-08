/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native DSP-3 (SD Gundam GX), on the kit in upd7725np.h.

#include <string.h>
#include "snes9x.h"
#include "dsp.h"
#include "upd7725n.h"

namespace
{

#define NT_IDLE_WAIT	1
#include "upd7725np.h"

const uint16	*rom = DSP3_DataROM;

inline uint16 Swap (uint16 v)
{
	return ((uint16) (v << 8 | v >> 8));
}

// the multiplier's low word
inline uint16 MulLo (uint16 a, uint16 b)
{
	return ((uint16) ((int32) (int16) a * (int16) b << 1));
}

// back through $000: wait for the last result to be read, then 8-bit mode and DR = $80
#define NT_EPILOGUE(id)		do { NT_BURN(1); NT_WAIT(id); NT_BURN(1); NT_SR((id) + 1, 0x0400); NT_WRITE((id) + 2, 0x0080); } while (0)

// ---- the commands; each starts at $008, 16-bit mode, and its dispatch

// RAM: $FA/$FC the last cell's x and y, $FB/$FD the map's width and height

bool Cell (void)			// $03: (x, y) to its cell, x + width * y
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(10); NT_WAIT(2); NT_READ(3, s.in[0]);
	{
		const uint16	x = (uint16) s.in[0] & 0xff, y = (uint16) s.in[0] >> 8;
		ram[0xfa] = x;
		ram[0xfc] = y;
		s.out[0] = (int16) ((int16) (uint16) (2 * x + MulLo(ram[0xfb], y)) >> 1);
		*tr = 0x00ff;
	}
	NT_BURN(6); NT_WRITE(4, s.out[0]);
	NT_EPILOGUE(5);
	NT_END
}

bool Step (void)			// $07: one step from (x, y) in direction d, wrapping at the map's edges: (x', y'), its cell
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(8); NT_SR(2, 0x0400);
	NT_BURN(4); NT_WAIT(3); NT_READ(4, s.in[0]);		// d, a byte
	NT_BURN(5); NT_SR(5, 0x0000);
	NT_WAIT(6); NT_READ(7, s.in[1]);					// x | y << 8
	{
		const uint16	rp = (uint16) (0x3b3 + 2 * ((uint16) s.in[0] & 0xff)) & 0x3ff;
		const uint16	dx = rom[rp], dy = rom[(rp - 1) & 0x3ff], w = ram[0xfb], h = ram[0xfd];
		const uint16	x = (uint16) s.in[1] & 0xff, y = (uint16) s.in[1] >> 8;
		ram[0xfa] = x;
		ram[0xfc] = y;
		// an odd column steps one row further when the direction says so
		const uint16	y1 = (uint16) (y + dy + (x & dx & 1)), x1 = (uint16) (dx + x);
		uint16	y2, x2;
		uint32	cost;
		if (y1 < h)
			{ y2 = y1; cost = 10; }
		else if (!((y1 - h) & 0x8000))
			{ y2 = (uint16) (y1 - h); cost = 9; }
		else
			{ y2 = (uint16) (y1 + h); cost = 11; }
		if (x1 < w)
			{ x2 = x1; cost += 7; s.i = 0; }
		else if (!((x1 - w) & 0x8000))
			{ x2 = (uint16) (x1 - w); cost += 8; s.i = 1; }
		else
			{ x2 = (uint16) (x1 + w); cost += 10; s.i = 1; }
		s.out[0] = (int16) (Swap(y2) | x2);
		*tr = x2;
		s.out[1] = (int16) ((int16) (uint16) (2 * x2 + MulLo(w, y2)) >> 1);
		s.out[2] = (int16) cost;
	}
	NT_BURN(s.out[2] - 1); NT_WRITE(8, s.out[0]);
	NT_BURN(s.i); NT_WAIT(9); NT_BURN(3); NT_WRITE(10, s.out[1]);
	NT_EPILOGUE(11);
	NT_END
}

bool Map (void)				// $06: the map's width and height; nothing to read back, so back through $001
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(12); NT_WAIT(2); NT_READ(3, s.in[0]);
	ram[0xfb] = (uint16) s.in[0] & 0xff;
	ram[0xfd] = (uint16) s.in[0] >> 8;
	*tr = 0x00ff;
	NT_BURN(5); NT_SR(4, 0x0400); NT_WRITE(5, 0x0080);
	NT_END
}

// Lists in RAM: rows of two-word entries, an id (bit 15 ends the list, bit 14 marks it in play) and its word
// in the mirrored row ($80 apart). DP steps within a row of 16.
inline uint8 Inc (uint8 dp)
{
	return ((uint8) ((dp & 0xf0) | ((dp + 1) & 0x0f)));
}

bool Members (void)			// $0C: list n's entries with a mask: the id, then the cells its bits pick, then 0; 0 at the end
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(13); NT_WAIT(2); NT_READNF(3, s.in[0]);
	{
		const uint16	rp = (uint16) (0x27 - (uint16) s.in[0]) & 0x3ff;
		const uint8		d = (uint8) rom[rp];
		s.in[2] = (int16) ram[d ^ 0x80];						// the cells count down from here
		s.in[1] = (int16) ram[(uint8) rom[(rp - 1) & 0x3ff]];	// the first entry
		s.i = ram[d] == ram[d ^ 0x80];
		*trb = (uint16) s.in[2];
		if (!s.i)
			*tr = (uint16) s.in[1];
	}
	if (s.i)
		NT_BURN(5);
	else
	{
		NT_BURN(9);
		for (s.i = 0; ; )
		{
			if (s.i++ > 256)
				NT_FOREVER(14);
			{
				const uint8	dp = (uint8) s.in[1];
				s.in[1] = (int16) (s.in[1] + rom[0x28]);
				*tr = (uint16) s.in[1];
				s.out[0] = (int16) ram[dp];
				s.out[1] = (int16) ram[Inc(dp)];
			}
			if (s.out[0] < 0)
			{
				NT_BURN(6);
				break;
			}
			if (!s.out[1])
			{
				NT_BURN(8);
				continue;
			}
			NT_BURN(9); NT_WRITE(4, (uint16) s.out[0] & 0x1fff);
			s.in[3] = s.in[2];
			s.out[2] = 0;
			NT_BURN(1); NT_WAIT(5);
			for (;;)
			{
				s.in[3] = (int16) (s.in[3] - rom[0x28]);
				s.out[2] = s.out[1] & 1;
				s.out[1] = (int16) (s.out[1] >> 1);
				if (s.out[2])
				{
					NT_BURN(7); NT_WRITE(6, ram[(uint8) s.in[3]] & 0x1fff);
					NT_WAIT(7); NT_BURN(1);
					continue;
				}
				if (!s.out[1])
					break;
				NT_BURN(6);
			}
			NT_BURN(5); NT_WRITE(8, 0);
			NT_WAIT(9); NT_BURN(1);
			s.i = 0;
		}
	}
	NT_WRITE(10, 0);
	NT_EPILOGUE(11);
	NT_END
}

bool Nearest (void)			// $1C: list n's entry in play nearest (x, y): its id (or the distance doubled), the distance
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(13); NT_WAIT(2); NT_READ(3, s.in[0]);						// n
	s.in[3] = (int16) ram[(uint8) (((int16) s.in[0] >> 1) + 0x7b)];
	NT_BURN(4); NT_WAIT(4); NT_READ(5, s.in[1]);						// x
	NT_WAIT(6); NT_READNF(7, s.in[2]);									// y
	{
		const uint16	x = (uint16) s.in[1], y = (uint16) s.in[2];
		uint16	best = 0xffff, p = (uint16) s.in[3];
		uint8	at = 0;
		uint32	cost = 0;
		bool	cb = false;		// B's carry, which the doubling shifts in
		s.i = 0;
		for (int n = 0; ; n++)
		{
			const uint8		dp = (uint8) p, m = dp ^ 0x80;
			const uint16	e = ram[dp];
			if (n > 256)
			{
				s.i = -1;
				break;
			}
			if (e & 0x8000)
				break;
			p = (uint16) (p + 2);
			if (!(e & 0x4000))
			{
				cost += 10;
				continue;
			}
			uint16	b = (uint16) (x - ram[m]), a = (uint16) (y - ram[Inc(m)]);
			cb = x < ram[m];
			cost += 18;
			if (b & 0x8000)
			{
				b = (uint16) ~b;
				cb = false;
				cost++;
			}
			if (a & 0x8000)
			{
				a = (uint16) ~a;
				cost++;
			}
			const uint16	d = (uint16) (a + b);
			if (d < best)
			{
				best = d;
				at = dp;
				cost += 2;
			}
		}
		*tr = p;
		*trb = best;
		const uint16	twice = (uint16) (best << 1 | cb);
		s.out[0] = (int16) ((twice & 0x8000) ? twice : ram[at] & 0x1fff);
		s.out[1] = (int16) best;
		if (!s.i)
			s.i = (int32) (cost + 7 + ((twice & 0x8000) ? 0 : 2));
	}
	if (s.i < 0)
		NT_FOREVER(14);
	NT_BURN(s.i); NT_WRITE(8, s.out[0]);
	NT_WAIT(9); NT_WRITE(10, s.out[1]);
	NT_EPILOGUE(11);
	NT_END
}

// $02: the scroll, then objects (list n, x, y) to a word with bit 15. On screen ($140 x $120, a $40 margin up and left)
// the CPU gets SR << 1 (bit 0: y on), x', y', $8000 and gives its id for list n; off (SR bit 14), x' wrapped, $8000.
bool Objects (void)
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(7); NT_SR(2, 0x0000);
	NT_BURN(1); NT_WAIT(3); NT_READ(4, s.in[0]);			// dx
	for (int i = 0; i < 4; i++)
		ram[0xfb + i] = ram[0x7b + i];						// the lists' ends start at their heads
	NT_BURN(8); NT_WAIT(5); NT_READ(6, s.in[1]);			// dy
	for (;;)
	{
		NT_WAIT(7); NT_READ(8, s.in[2]);					// n
		if (s.in[2] < 0)
			break;
		s.in[3] = (int16) (uint16) ((uint16) ((int16) s.in[2] >> 1) + 0xfb);	// its end's slot
		s.out[0] = (int16) (uint8) ram[(uint8) s.in[3]];		// where it goes
		*tr = (uint16) s.in[3];
		NT_BURN(8); NT_SR(9, 0x0000);
		NT_WAIT(10); NT_READ(11, s.in[4]);					// x
		{
			const uint8		m = (uint8) s.out[0] ^ 0x80;
			const uint16	x = (uint16) ((uint16) s.in[4] - (uint16) s.in[0]);
			uint32	at = 0;			// the instruction setting SR bit 14, if
			ram[m] = x;
			s.out[1] = (int16) x;
			*trb = x;
			if (!(x & 0x8000))
			{
				if (x >= 0x140)
				{
					if (x < 0x200)
						at = 12;
					else
					{
						ram[m] = (uint16) (x - 0x400);
						at = 15;
					}
				}
			}
			else if (x < 0xffc0)
			{
				if (x >= 0xfe00)
					at = 11;
				else
				{
					ram[m] = (uint16) (x + 0x400);
					at = 14;
				}
			}
			s.i = (int32) at;
		}
		if (s.i)
		{
			NT_BURN(s.i - 1); NT_SR(12, 0x4000);
		}
		else
			NT_BURN(7);
		NT_WAIT(13); NT_READ(14, s.in[5]);					// y
		{
			const uint8		m = Inc((uint8) s.out[0] ^ 0x80);
			const uint16	y = (uint16) ((uint16) s.in[5] - (uint16) s.in[1]);
			ram[m] = y;
			bool	on;
			if (!(y & 0x8000))
			{
				on = y < 0x120;
				s.i = on ? 0 : 8;
			}
			else
			{
				on = y >= 0xffc0;
				s.i = on ? 0 : 7;
			}
			s.out[2] = on;
		}
		if (s.i)
		{
			NT_BURN(s.i - 1); NT_SR(15, 0x4000);
		}
		else
			NT_BURN(6);
		NT_AT(16);
		s.out[3] = (int16) (uint16) (*sr << 1 | s.out[2]);		// SR as it reads it
		NT_BURN(1); NT_WRITE(17, s.out[3]);
		NT_WAIT(18);
		if (s.out[3] >= 0)
		{
			NT_BURN(1); NT_WRITE(19, s.out[1]);
			ram[(uint8) s.out[0] ^ 0x80] = (uint16) s.out[1];
			NT_BURN(1); NT_WAIT(20); NT_WRITE(21, ram[Inc((uint8) s.out[0] ^ 0x80)]);
			NT_WAIT(22); NT_WRITE(23, 0x8000);
			NT_WAIT(24); NT_READ(25, s.in[6]);				// its id
			{
				const uint8	t = (uint8) s.out[0];
				ram[t] = (uint16) s.in[6];
				ram[(uint8) s.in[3]] = (uint16) (Inc(t) + 1);
			}
			NT_BURN(4);
		}
		else
		{
			NT_BURN(1); NT_WRITE(26, ram[(uint8) s.out[0] ^ 0x80]);
			NT_WAIT(27); NT_WRITE(28, 0x8000);
			NT_BURN(1);
		}
	}
	NT_BURN(4); NT_SR(29, 0x0400); NT_WRITE(30, 0x0080);
	NT_END
}

bool Start (void)			// $3E: (x, y): its cell, and the searches' start ($E8-$ED)
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(18); NT_WAIT(2); NT_READ(3, s.in[0]);
	{
		const uint16	x = (uint16) s.in[0] & 0xff, y = (uint16) s.in[0] >> 8;
		s.out[0] = (int16) ((int16) (uint16) (2 * x + MulLo(ram[0xfb], y)) >> 1);
	}
	NT_BURN(7); NT_WRITE(4, s.out[0]);
	NT_WAIT(5);
	for (int i = 0; i < 6; i++)
		ram[0xe8 + i] = (uint16) s.in[0];
	*tr = 0x00ff;
	*trb = (uint16) s.in[0];
	NT_BURN(8); NT_SR(6, 0x0400); NT_WRITE(7, 0x0080);
	NT_END
}

// A list walk's next entry doesn't hang on the RAM, which its end test only reads: 256 entries without
// an end and it never ends.
const uint32	Forever = 0xffffffffu;

// $04's pass for one pair of lists: each entry of the first gets the mask of the second's entries within t
// of it on both axes (bit 0 the last); the instructions it took, or Forever
uint32 NearMasks (uint8 headp, uint8 otherp, uint16 t)
{
	uint32	cost = 4;
	uint8	d1 = (uint8) ram[headp];
	for (int n = 0; ; n++)
	{
		if (n > 256)
			return (Forever);
		*trb = Inc(d1);
		if (ram[d1] & 0x8000)
			return (cost + 4);
		const uint16	x = ram[d1 ^ 0x80], y = ram[Inc(d1) ^ 0x80];
		uint16	mask = 0;
		cost += 8;
		uint8	d2 = (uint8) ram[otherp];
		for (int m = 0; ; m++)
		{
			if (m > 256)
				return (Forever);
			if (ram[d2] & 0x8000)
				break;
			uint16	dx = (uint16) (x - ram[d2 ^ 0x80]);
			cost += 13;
			if (dx & 0x8000)
			{
				dx = (uint16) ~dx;
				cost++;
			}
			bool	hit = false;
			if (dx < t)
			{
				uint16	dy = (uint16) (y - ram[Inc(d2) ^ 0x80]);
				cost += 4;
				if (dy & 0x8000)
				{
					dy = (uint16) ~dy;
					cost++;
				}
				hit = dy < t;
			}
			mask = (uint16) (mask << 1 | hit);
			d2 = (uint8) (Inc(d2) + 1);
		}
		cost += 8;
		ram[Inc(d1)] = mask;
		d1 = (uint8) (Inc(d1) + 1);
	}
}

bool Proximity (void)		// $04: the four lists' ends marked, then which entries are near which; SR bit 14 while at it
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(9); NT_SR(2, 0x4000);
	{
		for (int p = 0xfb; p <= 0xfe; p++)
			ram[(uint8) ram[p]] = 0xffff;
		*tr = 0xffff;
		uint32	cost = 2 + 16 + 4 * 3;
		static const uint8	pass[4][3] = { { 0x26, 0x27, 0x2a }, { 0x24, 0x25, 0x2a }, { 0x22, 0x23, 0x2a }, { 0x20, 0x21, 0x29 } };
		for (int k = 0; k < 4 && cost != Forever; k++)
		{
			*tr = rom[pass[k][1]];
			const uint32	c = NearMasks((uint8) rom[pass[k][0]], (uint8) rom[pass[k][1]], rom[pass[k][2]]);
			cost = c == Forever ? Forever : cost + c;
		}
		s.i = cost == Forever ? -1 : (int32) cost;
	}
	if (s.i < 0)
		NT_FOREVER(6);
	NT_BURN(s.i); NT_SR(3, 0x0000);
	NT_BURN(2); NT_SR(4, 0x0400); NT_WRITE(5, 0x0080);
	NT_END
}

// $18: n rows (0: 65536) of 8 pixels, a byte each in 8-bit mode, to 4 words of bitplane pairs. RAM 0-7 keep
// the planes of the row's first 7 pixels, shifted in; the eighth's bits go straight to the words.
bool Planar (void)
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(14); NT_WAIT(2); NT_READ(3, s.in[0]);			// n
	*tr = (uint16) s.in[0];
	for (;;)
	{
		NT_SR(4, 0x0400); NT_WRITE(5, 0x0000);
		for (s.i = 0; s.i < 7; s.i++)
		{
			NT_BURN(1); NT_WAIT(6); NT_READ(7, s.in[1]);
			for (int j = 0; j < 8; j++)
				ram[j] = (uint16) (ram[j] << 1 | ((uint16) s.in[1] >> j & 1));
			NT_BURN(24);
		}
		NT_WAIT(8); NT_READ(9, s.in[1]);
		NT_SR(10, 0x0000);
		for (s.i = 0; s.i < 4; s.i++)
		{
			{
				const uint16	v = (uint16) s.in[1] >> (2 * s.i);
				const uint16	lo = (uint16) ((ram[2 * s.i] << 1 | (v & 1)) & 0xff), hi = (uint16) ((ram[2 * s.i + 1] << 1 | (v >> 1 & 1)) & 0xff);
				s.out[0] = (int16) (hi << 8 | lo);
				*trb = lo;
			}
			if (s.i)
			{
				NT_BURN(1); NT_WAIT(11); NT_BURN(9);
			}
			else
				NT_BURN(10);
			NT_WRITE(12, s.out[0]);
		}
		NT_WAIT(13);
		s.in[0] = (int16) (s.in[0] - 1);
		*tr = (uint16) s.in[0];
		NT_BURN(4);
		if (!s.in[0])
			break;
	}
	NT_EPILOGUE(14);
	NT_END
}

bool Multiply (void)		// $00: (a/2 * b/2) << 2 as a double word, low first
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(3); NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(2); NT_WAIT(4); NT_READNF(5, s.in[1]);
	{
		const int32		p = (int32) (s.in[0] >> 1) * (s.in[1] >> 1);
		const uint16	m = (uint16) (p >> 15), n = (uint16) (p << 1);
		s.out[0] = (int16) (uint16) (n << 1);
		s.out[1] = (int16) (uint16) (m << 1 | n >> 15);
	}
	NT_BURN(4); NT_WRITE(6, s.out[0]);
	NT_WAIT(7); NT_BURN(1); NT_WRITE(8, s.out[1]);
	NT_EPILOGUE(9);
	NT_END
}

// the middle word of k * v's double word, as the multiplier and the masks at $0AE/$0AF give it
inline uint16 MulMid (uint16 k, uint16 v)
{
	const int32		p = (int32) (int16) k * (int16) v;
	const uint16	m = (uint16) (p >> 15), n = (uint16) (p << 1);
	return (Swap((uint16) ((m & rom[0xae]) | (n & rom[0xaf]))));
}

bool Turn (void)			// $x1, $x5: (x, y) turned by angle a, from the sine table
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(7); NT_WAIT(2); NT_READ(3, s.in[0]);					// a
	NT_BURN(8); NT_WAIT(4); NT_READ(5, s.in[1]);					// x
	NT_BURN(11); NT_WAIT(6); NT_READNF(7, s.in[2]);				// y
	{
		const uint16	a = (uint16) s.in[0];
		const uint16	sn = rom[(uint16) (a + rom[0xab]) & 0x3ff];
		const uint16	cs = rom[(uint16) (((uint16) (a + rom[0xad]) & rom[0xac]) + rom[0xab]) & 0x3ff];
		const uint16	x = (uint16) (s.in[1] >> 1), y = (uint16) (s.in[2] >> 1);
		const uint16	sy = MulMid(sn, y);
		*tr = sn;
		*trb = cs;
		ram[0] = x;
		ram[2] = y;
		ram[1] = sy;
		s.out[0] = (int16) (uint16) (MulMid(cs, y) + MulMid(sn, x));
		s.out[1] = (int16) (uint16) (MulMid(cs, x) - sy);
	}
	NT_BURN(11); NT_WRITE(8, s.out[0]);
	NT_BURN(22); NT_WAIT(9); NT_WRITE(10, s.out[1]);
	NT_EPILOGUE(11);
	NT_END
}

bool Heads (void)			// $10/$30: the lists' heads ($7B on), $80 and on by the counts given, until a word with bit 15
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(11); NT_SR(2, 0x0000);
	s.in[1] = 0x80;
	s.i = 0x7b;
	NT_BURN(2);
	for (;;)
	{
		ram[(uint8) s.i] = (uint16) s.in[1];
		s.i = Inc((uint8) s.i);
		NT_BURN(1); NT_WAIT(3); NT_READ(4, s.in[0]);
		s.in[1] = (int16) (s.in[1] + s.in[0]);
		if (s.in[0] < 0)
			break;
		NT_BURN(2);
	}
	NT_BURN(4); NT_SR(5, 0x0400); NT_WRITE(6, 0x0080);
	NT_END
}

bool MemTest (void)			// $0F: fills RAM, waits on RQM, checks; twice, the pattern inverted; working RAM reports 0
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(783); NT_WAIT(2);
	NT_BURN(2052); NT_WAIT(3);
	for (int i = 0; i < 256; i++)
		ram[i] = (i & 1) ? 0x5555 : 0xaaaa;		// what the second pass leaves
	*trb = 0xaaaa;
	NT_BURN(1282); NT_WRITE(4, 0);
	NT_EPILOGUE(5);
	NT_END
}

bool RomDump (void)			// $1F/$3F: the data ROM's 1024 words
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(13);
	for (s.i = 0; ; )
	{
		NT_WAIT(2); NT_WRITE(3, rom[s.i]);
		if (++s.i == 1024)
			break;
		NT_BURN(2);
	}
	NT_BURN(1);
	NT_EPILOGUE(4);
	NT_END
}

bool Version (void)			// $2F: once the CPU touches DR, $0300
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(12); NT_WAIT(2); NT_WRITE(3, 0x0300);
	NT_EPILOGUE(4);
	NT_END
}

bool Hang (void)			// $20: the dispatch falls through to a jump to itself
{
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_FOREVER(2);
	NT_END
}

// The bit reader at $03F: n + 1 bits, MSB first, into $0E ($0D counts them down). $1D holds the data word,
// and $1E's marker bit shifts out after its 16th bit: then SR bit 14 and DR = the marker ask the CPU for the next.

// the bits up to one that needs a new word (true, its first four instructions owed) or to the last
bool Bits (void)
{
	uint16	c = ram[0x0d], r = ram[0x0e], d = ram[0x1d], m = ram[0x1e];
	uint32	n = 0;
	bool	refill;
	for (;;)
	{
		refill = (m & 0x8000) != 0;
		m = (uint16) (m << 1);
		if (refill)
		{
			n += 4;
			break;
		}
		r = (uint16) (r << 1 | d >> 15);
		d = (uint16) (d << 1);
		c--;
		n += 10;
		if (c & 0x8000)
			break;
	}
	ram[0x0d] = c;
	ram[0x0e] = r;
	ram[0x1d] = d;
	ram[0x1e] = m;
	NT_BURN(n);
	return (refill);
}

// the new word's first bit; the marker restarts
inline void Refill (uint16 w)
{
	ram[0x1e] = 1;
	ram[0x1d] = (uint16) (w << 1 | 1);
	ram[0x0e] = (uint16) (ram[0x0e] << 1 | w >> 15);
	ram[0x0d]--;
}

// a call to it (the CALL itself the caller's); ids id to id + 4
#define NT_BITS(id, n)	do {													\
	NT_BURN(5); ram[0x0d] = (uint16) (n); ram[0x0e] = 0;						\
	while (Bits())																\
	{																			\
		NT_SR(id, 0x4000); NT_WRITE((id) + 1, ram[0x1e]);						\
		NT_WAIT((id) + 2); NT_BURN(1); NT_READNF((id) + 3, s.in[15]);			\
		Refill((uint16) s.in[15]);												\
		NT_SR((id) + 4, 0x0000); NT_BURN(6);									\
		if (ram[0x0d] & 0x8000)													\
			break;																\
	}																			\
	NT_BURN(1); } while (0)

// $38, $08/$28: n 9-bit symbols (a literal or a step up), bytes from $20 and bit 8 in words from $E0; 4 or 8 ranges;
// m words out, a symbol each by range and offset: its byte, or $8002 + it and a 12/8-bit field ($08: 13/9)
bool Decode (void)
{
	uint16	&l = s.reg[0], &k = s.reg[1], &a = s.reg[2], &dp = s.reg[3];
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(s.cmd == 0x38 ? 13 : 11); NT_WRITE(2, 0x0080);
	ram[0x1e] = 0x8000;
	NT_BURN(2); NT_WAIT(3); NT_READ(4, s.in[0]);					// n
	*trb = (uint16) (s.in[0] + 0x40);
	l = 0x40;
	k = 0xe0;
	NT_BURN(4); NT_WAIT(5); NT_READNF(6, s.in[1]);				// m
	ram[0x1f] = (uint16) s.in[1];
	do
	{
		NT_BURN(2); NT_BITS(10, 1);
		NT_BURN(2);
		if (ram[0x0e] == 1)
		{
			(*tr)++;
			NT_BURN(3);
		}
		else if (ram[0x0e] == 0)
		{
			NT_BURN(3); NT_BITS(15, 8);
			*tr = ram[0x0e];
			NT_BURN(2);
		}
		else if (ram[0x0e] == 2)
		{
			NT_BURN(5); NT_BITS(20, 0);
			*tr = (uint16) (*tr + ram[0x0e] + 2);
			NT_BURN(5);
		}
		else
		{
			NT_BURN(5); NT_BITS(25, 3);
			*tr = (uint16) (*tr + ram[0x0e] + 4);
			NT_BURN(6);
		}
		{
			const uint16	x = Swap(*tr);
			const uint8		at = (uint8) (l >> 1);
			ram[(uint8) k] = (uint16) (ram[(uint8) k] << 1 | (x & 1));
			if (!(l & 1))
			{
				ram[at] = (uint16) (x & 0xfffe);
				s.i = !(x & 0xfffe);			// A's Z flag, which the padding below tests
				NT_BURN(14);
			}
			else
			{
				ram[at] |= (uint16) (*tr & 0xfeff);
				s.i = 0;
				if ((l + 1) & 0x0f)
					NT_BURN(15);
				else
				{
					s.i = !++k;
					NT_BURN(18);
				}
			}
		}
		l++;
	} while (l != *trb);
	NT_BURN(3);
	if (!s.i)
	{
		const uint32	sh = 16 - (l & 0x0f);
		ram[(uint8) k] = (uint16) ((uint32) ram[(uint8) k] << sh);
		NT_BURN(5 + 3 * sh);
	}
	NT_BURN(3); NT_BITS(30, 0);
	*trb = (uint16) (ram[0x0e] + 1);
	*tr = (uint16) (4 * *trb);
	ram[0x10] = 0;
	NT_BURN(5);
	for (dp = 0; ; )
	{
		NT_BURN(2); NT_BITS(35, 2);
		{
			const uint16	c = ram[0x0e];
			const uint8		next = (uint8) ((dp + 1) & 0x0f);
			ram[dp] = c;
			ram[0x10 | next] = (uint16) (rom[(0x10 + c) & 0x3ff] + ram[0x10 | dp]);
			dp = next;
		}
		NT_BURN(8);
		if (!--*tr)
			break;
	}
	k = 0xe0;
	*tr = ram[0x1f];
	NT_BURN(5);
	do
	{
		NT_BURN(1); NT_BITS(40, *trb);
		dp = (uint8) ram[0x0e];
		NT_BURN(3); NT_BITS(45, ram[dp]);
		{
			const uint16	v = (uint16) (ram[0x0e] + ram[(uint8) (dp ^ 0x10)]);
			const uint16	w = ram[(uint8) (0x20 + (uint16) ((int16) v >> 1))];
			const uint16	mask = rom[((v & rom[0x19]) + rom[0x18]) & 0x3ff];
			a = (uint16) ((v & 1) ? w & 0xff : w >> 8);
			s.i = (mask & ram[(uint8) ((uint16) ((int16) v >> 4) + k)]) != 0;
			NT_BURN((v & 1) ? 18 : 19);
		}
		if (s.i)
		{
			NT_BURN(2); NT_WRITE(50, 0x8002 + a);
			NT_WAIT(51); NT_BURN(2); NT_BITS(52, 0);
			s.i = (s.cmd == 0x38 ? 7 : 8) + 4 * ram[0x0e];
			NT_BURN(5); NT_BITS(57, s.i);
			a = ram[0x0e];
		}
		NT_WRITE(62, a);
		(*tr)--;
		NT_BURN(3); NT_WAIT(63); NT_BURN(1);
	} while (*tr);
	NT_BURN(2); NT_SR(64, 0x0400); NT_WRITE(65, 0x0080);
	NT_END
}

// ---- $1E, $0E/$2E: the movement range, radii min | max << 8 around $E8-$ED's starts. Per hex ring cell the CPU gives
// terrain (a bit at $CC on) and step cost (a byte a slot from $00, bits 3-7 the distance); they settle, then go back.

const uint32	WrapY[3] = { 2, 1, 3 }, OutX[3] = { 1, 3, 5 }, AlongX[3] = { 5, 6, 9 };	// a step's wraps, in instructions

// the hex step from (x, y) by the vector at rom[p], rom[p - 1], wrapping at the map's edges as $07 does: x' and
// y' to $FA/$FC, the cell back; which wraps it took (0: none, 1: back a map, 2: on a map)
uint16 HexStep (uint16 x, uint16 y, uint16 p, uint32 &yw, uint32 &xw)
{
	const uint16	w = ram[0xfb], h = ram[0xfd], dx = rom[p & 0x3ff], dy = rom[(p - 1) & 0x3ff];
	const uint16	y1 = (uint16) (y + dy + (x & dx & 1)), x1 = (uint16) (dx + x);
	uint16	x2, y2;
	if (y1 < h)
		{ y2 = y1; yw = 0; }
	else if (!((y1 - h) & 0x8000))
		{ y2 = (uint16) (y1 - h); yw = 1; }
	else
		{ y2 = (uint16) (y1 + h); yw = 2; }
	if (x1 < w)
		{ x2 = x1; xw = 0; }
	else if (!((x1 - w) & 0x8000))
		{ x2 = (uint16) (x1 - w); xw = 1; }
	else
		{ x2 = (uint16) (x1 + w); xw = 2; }
	ram[0xfa] = x2;
	ram[0xfc] = y2;
	return ((uint16) ((int16) (uint16) (2 * x2 + MulLo(w, y2)) >> 1));
}

uint32 StepOut (void)		// $3BB: from the ring's start (TRB) out in the sextant's direction ($F2); the start moves
{
	uint32	yw, xw;
	*tr = HexStep(*trb & 0xff, *trb >> 8, ram[0xf2], yw, xw);
	*trb = (uint16) (Swap(ram[0xfc]) | ram[0xfa]);
	return (26 + WrapY[yw] + OutX[xw]);
}

uint32 StepAlong (void)		// $3E0: from the last cell along the ring ($F3)
{
	uint32	yw, xw;
	*tr = HexStep(ram[0xfa], ram[0xfc], ram[0xf3], yw, xw);
	return (17 + WrapY[yw] + AlongX[xw]);
}

// the map word holding slot v's bit (rom[v & 15] its mask)
inline uint8 MapWord (uint16 v)
{
	return ((uint8) (0xcc + ((int16) v >> 4)));
}

// a ring cell's slot: the sextant's base ($F6) and the table's entry for (pos $FE, ring $FF)
inline uint16 SlotOf (void)
{
	return ((uint16) (ram[0xf6] + rom[(uint16) (0xb1 + ram[0xfe] + MulLo(0x0a, ram[0xff])) & 0x3ff]));
}

void Radii (uint16 v)
{
	ram[0xf4] = v & 0xff;
	ram[0xf5] = v >> 8;
	*tr = 0x00ff;
}

// $44F/$49F's start of sextant d: its base slot, its start, its two directions; the rings from the inner radius
void Sextant (uint16 d)
{
	ram[0xf6] = rom[(0x3cc + d) & 0x3ff];
	*trb = ram[(uint8) (0xe8 + d)];
	ram[0xf2] = (uint16) (0x3b3 + 2 * d);
	ram[0xf3] = (uint16) (0x3b3 + 2 * (d + 2));
	ram[0xff] = ram[0xf4];
}

// $519 ($2D3, up: the other way along): ring $F5's cells in sextant r against their neighbours, each side taking a
// shorter distance through the other; returns the instructions
uint32 Relax (uint16 r, bool up)
{
	uint32	cost = up ? 6 : 8;
	ram[0xf4] = up ? 1 : (uint16) (ram[0xf5] + 1);
	ram[0xf0] = rom[r & 0x3ff];
	ram[0xf1] = rom[(r - 1) & 0x3ff];
	for (;;)
	{
		ram[0xf3] = 0x3b1;
		uint16	slot = (uint16) (ram[0xf1] + rom[(uint16) (ram[0xf4] + ram[0xf0] + MulLo(0x0a, ram[0xf5])) & 0x3ff]);
		cost += 20;
		if (slot & 0x8000)
		{
			slot = (uint16) (slot + ram[0xf2]);
			cost++;
		}
		if (!(rom[slot & 0x0f] & ram[MapWord(slot)]))
		{
			const bool		odd = (slot & 1) != 0;
			const uint16	h = (uint16) ((int16) slot >> 1);
			ram[0xf7] = 0;
			ram[0xf8] = odd ? 0x38d : 0x394;
			ram[0xf9] = h;
			const uint16	sc = (uint16) (rom[odd ? 0x391 : 0x398] & ram[(uint8) h]);
			cost += odd ? 14 : 12;
			if (sc)
			{
				// its distance and step cost in both bytes: $EE/$EF the high one, $FE/$FF the low
				const uint16	sd = (uint16) (rom[odd ? 0x390 : 0x397] & ram[(uint8) h]), n = MulLo(4, sc);
				if (odd)
					{ ram[0xff] = n; ram[0xfe] = sd; ram[0xef] = Swap(n); ram[0xee] = Swap(sd); }
				else
					{ ram[0xef] = n; ram[0xee] = sd; ram[0xff] = Swap(n); ram[0xfe] = Swap(sd); }
				ram[0xfa] = 6;
				cost += 10;
				for (;;)
				{
					const uint16	p = ram[0xf3] & 0x3ff;
					const uint16	a = (uint16) (rom[p] + ram[0xf5]);
					const uint16	b = (uint16) (rom[(p - 1) & 0x3ff] + ram[0xf4] + ram[0xf0] + MulLo(0x0a, a));
					ram[0xf3] = (p - 2) & 0x3ff;
					uint16	ns = (uint16) (ram[0xf1] + rom[b & 0x3ff]);
					cost += 14;
					if (ns & 0x8000)
					{
						ns = (uint16) (ns + ram[0xf2]);
						cost++;
					}
					const bool		nodd = (ns & 1) != 0;
					const uint8		h2 = (uint8) ((int16) ns >> 1), lane = nodd ? 0xfe : 0xee;
					const uint16	nc = (uint16) (rom[nodd ? 0x391 : 0x398] & ram[h2]);
					cost += 9 + nodd;
					if (nc)
					{
						const uint32	nd = (uint32) ram[lane] + MulLo(4, nc);
						const uint16	od = (uint16) (rom[nodd ? 0x390 : 0x397] & ram[h2]);
						uint16	gap;
						cost += 4;
						if (nd > 0xffff)
						{
							gap = 0xffff;
							cost++;
						}
						else if ((uint16) nd < od)
						{
							ram[h2] = (uint16) ((ram[h2] & rom[nodd ? 0x38d : 0x394]) | nd);
							cost += 8;
							goto next;
						}
						else
						{
							gap = (uint16) (nd - od);
							cost += 2;
						}
						cost += 3;
						if (gap >= rom[nodd ? 0x38e : 0x395])
						{
							const uint32	t = (uint32) od + ram[lane | 1];
							cost += 2;
							if (t <= 0xffff)
							{
								cost += 2;
								if ((uint16) t < ram[lane])
								{
									cost += 10;
									if (!(rom[ns & 0x0f] & ram[MapWord(ns)]))
									{
										ram[lane] = (uint16) t;
										ram[lane ^ 0x10] = Swap((uint16) t);
										ram[0xf7] = Swap((uint16) t);
										ram[0xfa] = 5;
										cost += 8;
										continue;
									}
								}
							}
						}
					}
				next:
					cost += 5;
					if (!--ram[0xfa])
						break;
				}
				cost += 3;
				if (ram[0xf7])
				{
					const uint16	rp = ram[0xf8] & 0x3ff;
					const uint8		at = (uint8) ram[0xf9];
					ram[at] = (uint16) ((ram[at] & rom[rp]) | ram[(uint8) rom[(rp - 1) & 0x3ff]]);
					cost += 9;
				}
			}
		}
		if (up)
		{
			const uint16	left = (uint16) (ram[0xf5] - ram[0xf4] + 1);
			ram[0xf4]++;
			cost += 6;
			if (!left)
				break;
		}
		else
		{
			cost += 5;
			if (!--ram[0xf4])
				break;
		}
	}
	return (cost);
}

// $5C8 ($4E2): the inner ring's first cells take their step cost as their distance from 0; the rings to settle
uint32 Settle (void)
{
	uint32	cost = 6;
	ram[0xf2] = 0x198;
	s.reg[1] = ram[0xf4] != 0;
	if (ram[0xf4])
	{
		const uint16	n = (uint16) (ram[0xf5] - 1);
		ram[0xf5] = ram[0xf4];
		ram[0xf6] = n;
		cost += 5;
	}
	else
	{
		cost += 3 + 5;
		for (int k = 0; k < 6; k++)
		{
			const uint8		at = (uint8) ((int16) rom[0x38a - 2 * k] >> 1);
			const uint16	c = (uint16) (0x0700 & ram[at]);
			cost += 13;
			if (c)
			{
				ram[at] = (uint16) ((ram[at] & 0x07ff) | MulLo(4, c));
				cost += 4;
			}
		}
		const uint16	n = (uint16) (ram[0xf5] - 1);
		ram[0xf5] = 1;
		ram[0xf6] = n;
	}
	return (cost + 2);
}

uint32 SettleRing (bool up)		// one ring, its six sextants
{
	uint32	cost = 4;
	for (int k = 0; k < 6; k++)
		cost += 2 + Relax((uint16) (up ? 0x38b - 2 * k : 0x381 + 2 * k), up);
	ram[0xf5]++;
	return (cost);
}

bool Range (void)
{
	uint16	&d = s.reg[0];
	NT_BEGIN
	NT_SR(1, 0x0000);
	NT_BURN(s.cmd == 0x1e ? 16 : 14); NT_WAIT(2); NT_READNF(3, s.in[0]);		// the radii
	Radii((uint16) s.in[0]);
	NT_BURN(4);
	for (d = 0; d < 6; d++)
	{
		Sextant(d);
		NT_BURN(26);
		if (ram[0xf4])
		{
			ram[0xff] = (uint16) (ram[0xf4] + 2);
			NT_BURN(5 + StepOut());
			NT_BURN(StepOut());
		}
		do
		{
			ram[0xfe] = 0;
			NT_BURN(2);
			do
			{
				ram[0xf7] = SlotOf();
				NT_BURN(16 + (ram[0xfe] ? 1 + StepAlong() : 2 + StepOut()));
				NT_SR(10, 0x0000); NT_WRITE(11, *tr);								// the cell
				NT_WAIT(12); NT_WRITE(13, 0x0000); NT_SR(14, 0x0400);
				ram[0xf8] = *tr;
				*tr = ram[0xf7];
				NT_BURN(4); NT_WAIT(15); NT_READ(16, s.in[1]);						// its terrain
				ram[0xf7] = (uint16) ((int16) *tr >> 1);
				{
					const uint8		at = MapWord(*tr);
					const uint16	bit = rom[*tr & 0x0f];
					if (s.in[1] & 0x80)
						ram[at] |= bit;
					else
						ram[at] &= (uint16) ~bit;
					s.out[0] = (int16) (uint8) ram[0xf7];
					NT_BURN(3 + ((s.in[1] & 0x80) ? 11 : 12) + ((*tr & 1) ? 3 : 4));
				}
				NT_WAIT(17); NT_READNF(18, s.in[2]);								// its step cost
				{
					const uint16	c = (uint16) s.in[2] & 0xff;
					const uint8		at = (uint8) s.out[0];
					if (!(*tr & 1))
						ram[at] = (c & 0x80) ? 0xf8f8 : (uint16) (c << 8 | 0xf8f8);
					else if (!(c & 0x80))
						ram[at] |= c;
					NT_BURN(2 + ((c & 0x80) ? 1 : 2) + 6);
				}
				ram[0xfe]++;
			} while (ram[0xfe] != (uint16) (ram[0xff] + 1));
			ram[0xff]++;
			NT_BURN(4);
		} while (ram[0xff] != ram[0xf5]);
		NT_BURN(1);
	}
	NT_SR(20, 0x0000); NT_WRITE(21, 0xffff);
	NT_WAIT(22);
	NT_BURN(Settle());
	do
	{
		NT_BURN(SettleRing(s.cmd != 0x1e));
		NT_PAY(23);
	} while (ram[0xf5] != ram[0xf6]);
	NT_BURN(1 + (s.cmd == 0x1e || s.reg[1]));
	NT_SR(24, 0x4000); NT_WRITE(25, 0x0000);
	NT_BURN(3); NT_WAIT(26); NT_READNF(27, s.in[0]);							// the radii again
	Radii((uint16) s.in[0]);
	NT_BURN(4);
	for (d = 0; d < 6; d++)
	{
		Sextant(d);
		ram[0xf9] = (uint16) (0xe8 + d);
		NT_BURN(27);
		do
		{
			ram[0xfe] = 0;
			NT_BURN(2);
			do
			{
				NT_BURN(5 + (ram[0xfe] ? 1 + StepAlong() : 2 + StepOut()));
				NT_SR(30, 0x0000);
				NT_BURN(3); NT_WRITE(31, *tr);										// the cell
				{
					const uint16	slot = SlotOf();
					uint16	w = ram[(uint8) ((int16) slot >> 1)];
					if (!(slot & 1))
						w = Swap(w);
					const uint16	v = (uint16) ((int16) w >> 3) & 0x1f;
					s.out[0] = (int16) (v == 0x1f ? 0xffff : v);
					s.i = 7 + !(slot & 1) + (v == 0x1f);
				}
				NT_BURN(10); NT_WAIT(32); NT_BURN(1); NT_SR(33, 0x0400);
				NT_BURN(s.i); NT_WRITE(34, s.out[0]);								// its distance
				NT_WAIT(35);
				NT_BURN(7);
				ram[0xfe]++;
			} while (ram[0xfe] != (uint16) (ram[0xff] + 1));
			ram[0xff]++;
			NT_BURN(4);
		} while (ram[0xff] != ram[0xf5]);
		ram[(uint8) ram[0xf9]] = *trb;
		NT_BURN(3);
	}
	NT_SR(40, 0x0000); NT_WRITE(41, 0xffff);
	NT_EPILOGUE(42);
	NT_END
}

// ---- power-on and the command wait

bool Boot (void)			// $000-$003: past the wait for the last read, 8-bit mode, DR = $80
{
	NT_BEGIN
	NT_WAIT(1); NT_BURN(1); NT_SR(2, 0x0400); NT_WRITE(3, 0x0080);
	NT_END
}

bool Idle (void)			// $004-$007: wait for a command; bits 6-7 send it back to the wait
{
	NT_BEGIN
	for (;;)
	{
		NT_BURN(1); NT_WAIT(1); NT_READ(2, s.in[0]);
		s.cmd = (uint8) s.in[0];
		NT_BURN(1);
		if (!(s.cmd & 0xc0))
			break;
	}
	NT_END
}

bool (*Program (uint8 cmd)) (void)
{
	if ((cmd & 0x07) == 0x03)
		return (Cell);
	if ((cmd & 0x0f) == 0x07)
		return (Step);
	if ((cmd & 0x0f) == 0x06)
		return (Map);
	if (!cmd)
		return (Multiply);
	if ((cmd & 0x03) == 0x01)
		return (Turn);
	if ((cmd & 0x07) == 0x02)
		return (Objects);
	if (cmd == 0x3e)
		return (Start);
	if ((cmd & 0x1f) == 0x10)
		return (Heads);
	if (cmd == 0x18)
		return (Planar);
	if ((cmd & 0x1f) == 0x1f)
		return (RomDump);
	if (cmd == 0x0f)
		return (MemTest);
	if (cmd == 0x2f)
		return (Version);
	if (cmd == 0x20)
		return (Hang);
	if ((cmd & 0x0f) == 0x04)
		return (Proximity);
	if ((cmd & 0x1f) == 0x0c)
		return (Members);
	if ((cmd & 0x1f) == 0x1c)
		return (Nearest);
	if ((cmd & 0x1f) == 0x08 || cmd == 0x38)
		return (Decode);
	return (Range);				// $0E, $1E, $2E
}

void ChipReset (bool variant)
{
	(void) variant;
}

void ChipLoaded (void)
{
}

}	// namespace

const S9xUPD7725Native	S9xDSP3Native = NT_CHIP;
