/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native DSP-4 (Top Gear 3000), on the kit in upd7725np.h.

#include <string.h>
#include "snes9x.h"
#include "upd7725n.h"

namespace
{

#define NT_IDLE_WAIT	1
#include "upd7725np.h"

// the data ROM: $4000 / n from 1 to $5A, then the RGB555 masks
uint16	rom[1024];

void BuildRom (void)
{
	memset(rom, 0, sizeof(rom));
	for (int n = 1; n <= 0x5a; n++)
		rom[n] = (uint16) (0x4000 / n);
	rom[0x5b] = 0x7c00;
	rom[0x5c] = 0x03e0;
	rom[0x5d] = 0x001f;
}

// back through $000: DR = $FFFF, then the command wait
#define NT_EPILOGUE(id)		do { NT_BURN(1); NT_WRITE(id, 0xffff); } while (0)

// ---- the commands; each starts after its dispatch from $003 (10 instructions, 9 for $10-$14)

bool Multiply (void)		// $00: a * b, the low word then the high
{
	NT_BEGIN
	NT_BURN(10); NT_WRITE(1, 0x0000);
	NT_BURN(1); NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(2); NT_WAIT(4); NT_READ(5, s.in[1]);
	{
		const int32	p = (int32) s.in[0] * s.in[1];
		s.out[0] = (int16) p;
		s.out[1] = (int16) (p >> 16);
		s.i = (int32) (p >> 15) & 1;
		*tr = 0x8000;
		*trb = 0x7fff;
	}
	NT_BURN(6 + s.i); NT_WRITE(6, s.out[0]);
	NT_WAIT(7); NT_WRITE(8, s.out[1]);
	NT_WAIT(9); NT_EPILOGUE(10);
	NT_END
}

bool Version (void)			// $02: 1
{
	NT_BEGIN
	NT_BURN(10); NT_WRITE(1, 0x0001);
	NT_WAIT(2); NT_EPILOGUE(3);
	NT_END
}

inline uint16 Swap (uint16 v)
{
	return ((uint16) (v << 8 | v >> 8));
}

// the multiplier's high word
inline uint16 MulHi (uint16 k, uint16 l)
{
	return ((uint16) ((int32) (int16) k * (int16) l >> 15));
}

// $0A: t, then two words of four signed nibbles each (the top first, each * 48); the four pairs' values at t,
// a + (b - a) * t. RAM $02 keeps t, $00/$01 the nibble mask, $10-$40 and $11-$41 the values.
bool Blend (void)
{
	NT_BEGIN
	NT_BURN(10); NT_WRITE(1, 0x0000);
	NT_BURN(2); NT_WAIT(2); NT_READ(3, s.in[0]);			// t
	ram[0x02] = (uint16) s.in[0];
	for (s.i = 0; s.i < 2; s.i++)
	{
		NT_BURN(s.i ? 19 : 4); NT_WAIT(4); NT_READ(5, s.in[1]);
		ram[s.i] = 0xf000;
		for (int k = 0; k < 4; k++)
			ram[0x10 * (k + 1) + s.i] = MulHi(0x0180, (uint16) ((uint16) s.in[1] << (4 * k) & 0xf000));
	}
	for (s.i = 0; s.i < 4; s.i++)
	{
		{
			const uint8	at = (uint8) (0x10 * (s.i + 1));
			s.out[0] = (int16) (uint16) (ram[at] + MulHi(ram[0x02], (uint16) (ram[at + 1] - ram[at])));
		}
		if (s.i)
		{
			NT_BURN(1); NT_WAIT(6); NT_BURN(5);
		}
		else
			NT_BURN(22);
		NT_WRITE(7, s.out[0]);
	}
	NT_WAIT(8); NT_EPILOGUE(9);
	NT_END
}

bool Pack (void)			// $11: four words x to a word of nibbles, (x * $2AA) >> 15 each, the first on top
{
	NT_BEGIN
	NT_BURN(9); NT_WRITE(1, 0x0000);
	s.out[0] = 0;
	*tr = 0xfff0;
	*trb = 0x000f;
	for (s.i = 0; s.i < 4; s.i++)
	{
		NT_BURN(s.i ? 6 : 7); NT_WAIT(2); NT_READ(3, s.in[0]);
		s.out[0] = (int16) (uint16) (((uint16) s.out[0] << 4 & 0xfff0) | (MulHi(0x02aa, (uint16) s.in[0]) & 0x000f));
	}
	NT_BURN(5); NT_WRITE(4, s.out[0]);
	NT_WAIT(5); NT_EPILOGUE(6);
	NT_END
}

bool MemTest (void)			// $12: fills RAM, waits on RQM, checks; twice, the pattern inverted; working RAM reports 0
{
	NT_BEGIN
	NT_BURN(780); NT_WAIT(1);
	NT_BURN(2052); NT_WAIT(2);
	for (int i = 0; i < 256; i++)
		ram[i] = (i & 1) ? 0x5555 : 0xaaaa;		// what the second pass leaves
	*trb = 0xaaaa;
	NT_BURN(1282); NT_WRITE(3, 0x0000);
	NT_WAIT(4); NT_EPILOGUE(5);
	NT_END
}

bool RomDump (void)			// $13: the data ROM's 1024 words
{
	NT_BEGIN
	NT_BURN(12);
	for (s.i = 0; ; )
	{
		NT_WRITE(1, rom[s.i]);
		NT_WAIT(2);
		if (++s.i == 1024)
			break;
		NT_BURN(2);
	}
	NT_BURN(1); NT_EPILOGUE(3);
	NT_END
}

bool Revision (void)		// $14: $0400
{
	NT_BEGIN
	NT_BURN(9); NT_WRITE(1, 0x0400);
	NT_WAIT(2); NT_EPILOGUE(3);
	NT_END
}

bool Fill (void)			// $03/$0E: RAM $00-$77 to $2121 / $1010
{
	NT_BEGIN
	*tr = s.cmd == 0x03 ? 0x2121 : 0x1010;
	for (int i = 0; i < 0x78; i++)
		ram[i] = *tr;
	NT_BURN(10 + 3 + 3 * 0x78); NT_EPILOGUE(1);
	NT_END
}

bool Clear (void)			// $05: RAM $F0-$FF to 0, $EE/$EF to $F0 / 3
{
	NT_BEGIN
	for (int i = 0xf0; i <= 0xff; i++)
		ram[i] = 0;
	ram[0xee] = 0x00f0;
	ram[0xef] = 0x0003;
	NT_BURN(10 + 2 + 2 * 16 + 4); NT_EPILOGUE(1);
	NT_END
}

bool Dump (void)			// $06: RAM $F0-$FF
{
	NT_BEGIN
	NT_BURN(11);
	for (s.i = 0xf0; ; )
	{
		NT_WRITE(1, ram[s.i]);
		NT_WAIT(2);
		NT_BURN(1);
		if (++s.i > 0xff)
			break;
	}
	NT_EPILOGUE(3);
	NT_END
}

// ---- $01, $0D, $0F: the road, projected segment by segment. A 32-bit value keeps its high word $70 above its low.
enum
{
	WY = 0x00, WX = 0x01, DWY = 0x02, DWX = 0x03,	// world y, x and their steps, each (lo, lo + $70)
	VY1 = 0x10, VX1 = 0x11, COLOR = 0x12,		// the last projected y and x; $8000, or $0F's colour
	RASTER = 0x20, YOFS2 = 0x21,				// the raster line reached; the y offset term (doubled)
	YACC = 0x30, XACC = 0x40,					// the scroll accumulators (lo, lo + 1)
	VBOT = 0x50, LINES = 0x51,					// the viewport's bottom line (counting down), lines left
	DYS = 0x60, DXS = 0x61,						// the segment's y and x scroll travel, doubled
	SKIP = 0x80, PTR = 0x81,					// lines skipped above the top; the HDMA table pointer
	Y0 = 0x90, X0 = 0x91,						// the segment's starting scroll
	YOFS = 0xa0, TOP = 0xa1, YENV = 0xb0
};

inline void Add32 (uint8 at, uint16 lo, uint16 hi)
{
	const uint32	v = ((uint32) ram[at + 0x70] << 16 | ram[at]) + ((uint32) hi << 16 | lo);
	ram[at] = (uint16) v;
	ram[at + 0x70] = (uint16) (v >> 16);
}

// $149: a 1.7.8 word added to a 32-bit value; the instructions it takes
inline uint32 Add78 (uint8 at, uint16 v)
{
	Add32(at, (uint16) (v << 8), (uint16) ((int16) v >> 8));
	return ((v & 0x8000) ? 10 : 9);
}

// a step of a scroll accumulator: k times the segment's 1/lines (L), as the multiplier's double word
inline void Step32 (uint8 at, uint16 k, uint16 l)
{
	const int32	p = (int32) (int16) k * (int16) l;
	const uint16	n = (uint16) (p << 1), m = (uint16) (p >> 15);
	const uint32	lo = (uint32) ram[at] + n;
	ram[at] = (uint16) lo;
	ram[at + 1] = (uint16) (ram[at + 1] + m + (lo >> 16));
}

// $07/$10 ("flat") take each segment's x and y from the CPU (a + d * b >> 15 each) instead of projecting them,
// without the world's steps or turn-offs; $0F/$10 first take a colour for the lines.
bool Road (void)
{
	uint16	&l = s.reg[0];		// the multiplier's L through a segment's lines
	const bool	flat = s.cmd == 0x07 || s.cmd == 0x10;
	NT_BEGIN
	if (s.cmd == 0x0f || s.cmd == 0x10)
	{
		NT_BURN(s.cmd == 0x10 ? 10 : 11); NT_WRITE(1, 0x0000);
		NT_BURN(1); NT_WAIT(2); NT_BURN(1); NT_READNF(3, s.in[0]);			// the colour
		ram[COLOR] = (uint16) s.in[0] & 0x801f;
		NT_BURN(2); NT_READNF(4, s.in[0]);
		ram[COLOR + 1] = (uint16) s.in[0] & 0x03e0;
		NT_BURN(2); NT_READ(5, s.in[0]);
		ram[COLOR + 2] = (uint16) s.in[0] & 0x7c00;
		NT_BURN(flat ? 2 : 3);
	}
	else
	{
		ram[COLOR] = 0x8000;
		NT_BURN(flat ? 13 : 14);
	}
	// $15D: the world
	NT_WRITE(10, 0x0000);
	NT_BURN(1); NT_WAIT(11); NT_READ(12, s.in[0]); ram[WY] = (uint16) s.in[0];
	NT_WAIT(13); NT_READ(14, s.in[0]); ram[WY + 0x70] = (uint16) s.in[0];
	NT_READNF(15, s.in[0]); ram[VY1] = (uint16) s.in[0];
	NT_WAIT(16); NT_READ(17, s.in[0]); ram[RASTER] = (uint16) s.in[0];
	NT_WAIT(18); NT_READ(19, s.in[0]); ram[TOP] = (uint16) s.in[0];
	NT_WAIT(20); NT_READ(21, s.in[0]); ram[YACC + 1] = (uint16) s.in[0];
	NT_WAIT(22); NT_READ(23, s.in[0]); ram[VBOT] = (uint16) s.in[0];
	NT_WAIT(24); NT_READ(25, s.in[0]); ram[WX] = (uint16) s.in[0];
	NT_WAIT(26); NT_READ(27, s.in[0]); ram[WX + 0x70] = (uint16) s.in[0];
	NT_READNF(28, s.in[0]); ram[VX1] = (uint16) s.in[0];
	NT_READNF(29, s.in[1]);
	NT_WAIT(30); NT_READ(31, s.in[0]); ram[XACC + 1] = (uint16) (s.in[1] + s.in[0]);
	NT_BURN(1); NT_WAIT(32); NT_READ(33, s.in[0]); ram[PTR] = (uint16) s.in[0];
	NT_WAIT(34); NT_READ(35, s.in[0]); ram[YOFS] = (uint16) s.in[0]; ram[YOFS2] = (uint16) (s.in[0] << 1);
	if (flat)
	{
		NT_BURN(3); NT_WAIT(98); NT_READ(99, s.in[0]);						// the distance
		s.in[15] = s.in[0];
		if (s.in[15] < 0)
			goto flat_end;
		goto segment;
	}
	NT_BURN(2); NT_WAIT(36); NT_READ(37, s.in[0]); ram[DWY] = (uint16) s.in[0];
	NT_WAIT(38); NT_READ(39, s.in[0]); ram[DWY + 0x70] = (uint16) s.in[0];
	NT_WAIT(40); NT_READ(41, s.in[0]); ram[DWX] = (uint16) s.in[0];
	NT_WAIT(42); NT_READ(43, s.in[0]); ram[DWX + 0x70] = (uint16) s.in[0];
	NT_BURN(1); NT_WAIT(44); NT_READ(45, s.in[0]);							// the distance
	s.in[15] = s.in[0];
	if (s.in[15] < 0)
	{
		NT_BURN(1); NT_EPILOGUE(46);
		s.line = 0;
		return (true);
	}
	*tr = 0xff00;
	*trb = 0x00ff;
	NT_BURN(5); NT_WAIT(47); NT_READ(48, s.in[0]);						// y's envelope, 1.7.8
	NT_BURN(Add78(WY, (uint16) s.in[0]));
	if (s.cmd == 0x0d)
	{
		NT_BURN(2); NT_WAIT(49); NT_READ(50, s.in[0]);					// x's, 1.7.8
		NT_BURN(Add78(WX, (uint16) s.in[0]) + 5);
	}
	else
	{
		NT_BURN(2); NT_WAIT(51); NT_READ(52, s.in[0]);					// x's, 32-bit
		ram[WX] = (uint16) (ram[WX] + s.in[0]);
		s.i = (uint16) s.in[0] > ram[WX];		// the carry
		NT_BURN(2); NT_WAIT(53); NT_READ(54, s.in[0]);
		ram[WX + 0x70] = (uint16) (ram[WX + 0x70] + s.in[0] + s.i);
		NT_BURN(7);
	}
	for (;;)
	{
		if (flat)
		{
		segment:
			*tr = 0xff00;
			*trb = 0x00ff;
			NT_BURN(4); NT_WAIT(100); NT_READ(101, s.in[0]);				// y, a + d * b >> 15
			NT_WAIT(102); NT_READ(103, s.in[1]);
			ram[WY + 0x70] = (uint16) (s.in[0] + MulHi((uint16) s.in[15], (uint16) s.in[1]));
			NT_BURN(3); NT_WAIT(104); NT_READ(105, s.in[0]);				// x
			NT_WAIT(106); NT_READ(107, s.in[1]);
			ram[WX + 0x70] = (uint16) (s.in[0] + MulHi((uint16) s.in[15], (uint16) s.in[1]));
			NT_BURN(3); NT_WAIT(108); NT_READNF(109, s.in[0]);				// the y offset's envelope
			ram[YENV] = (uint16) s.in[0];
			NT_BURN(1); NT_WRITE(110, ram[WX + 0x70]);
			{
				const uint16	x = ram[WX + 0x70];
				ram[X0] = ram[XACC + 1];
				ram[DXS] = (uint16) ((uint16) (x - ram[VX1]) << 1);
				ram[VX1] = x;
			}
			NT_BURN(5); NT_WAIT(111);
			{
				const uint16	y2o = (uint16) (MulHi((uint16) s.in[15], ram[YOFS]) << 1);
				ram[DYS] = (uint16) (y2o - ram[YOFS2]);
				ram[YOFS2] = y2o;
				ram[Y0] = ram[YACC + 1];
				s.out[0] = (int16) ram[WY + 0x70];
				s.out[1] = (int16) (uint16) (ram[VY1] - ram[WY + 0x70]);
				ram[VY1] = ram[WY + 0x70];
			}
			NT_BURN(8); NT_WRITE(112, s.out[0]);
			goto lines;
		}
		NT_WAIT(60); NT_READ(61, s.in[0]);									// the y step's change
		NT_BURN(Add78(DWY, (uint16) s.in[0]) + 2);
		NT_WAIT(62); NT_READ(63, s.in[0]);									// the x step's
		NT_BURN(Add78(DWX, (uint16) s.in[0]) + 1);
		NT_WAIT(64); NT_READNF(65, s.in[0]);								// the y offset's envelope
		ram[YENV] = (uint16) s.in[0];
		// the segment's projection: x and x', y and y', at distance K
		NT_BURN(1); NT_WRITE(66, ram[WX + 0x70]);
		{
			const uint16	k = (uint16) s.in[15];
			const uint16	x2 = MulHi(k, ram[WX + 0x70]);
			ram[X0] = ram[XACC + 1];
			ram[DXS] = (uint16) ((uint16) (x2 - ram[VX1]) << 1);
			ram[VX1] = x2;
			s.out[0] = (int16) x2;
		}
		NT_BURN(2); NT_WAIT(67); NT_WRITE(68, s.out[0]);
		{
			const uint16	k = (uint16) s.in[15];
			const uint16	y2o = (uint16) (MulHi(k, ram[YOFS]) << 1);
			ram[DYS] = (uint16) (y2o - ram[YOFS2]);
			ram[YOFS2] = y2o;
			ram[Y0] = ram[YACC + 1];
		}
		NT_BURN(5); NT_WAIT(69); NT_BURN(7); NT_WRITE(70, ram[WY + 0x70]);
		{
			const uint16	y2 = MulHi((uint16) s.in[15], ram[WY + 0x70]);
			s.out[0] = (int16) y2;
			s.out[1] = (int16) (uint16) (ram[VY1] - y2);			// lines from the last y
			ram[VY1] = y2;
		}
		NT_BURN(2); NT_WAIT(71); NT_WRITE(72, s.out[0]);
	lines:
		// the lines it fills, between the raster line reached and the window's top
		{
			const uint16	y2 = (uint16) s.out[0], d = (uint16) s.out[1];
			uint32	cost;
			int16	lines = 0;
			bool	draw = false;
			if (d && !(d & 0x8000))
				*tr = d;
			if (d & 0x8000)
				cost = 1;
			else if (!d)
				cost = 2;
			else
			{
				const uint16	r = (uint16) (ram[RASTER] - y2);
				if (r & 0x8000)
					cost = 6;
				else if (!r)
					cost = 7;
				else
				{
					ram[RASTER] = y2;
					const uint16	t = (uint16) (y2 - ram[TOP]);
					if (!(t & 0x8000))
					{
						lines = (int16) r;
						draw = true;
						cost = 10;
					}
					else
					{
						const uint16	u = (uint16) (r + t);
						if (u & 0x8000)
							cost = 12;
						else if (!u)
							cost = 13;
						else
						{
							lines = (int16) u;
							draw = true;
							cost = 13;
						}
					}
				}
			}
			s.out[2] = lines;
			s.i = draw;
			s.out[3] = (int16) cost;
		}
		NT_BURN(2); NT_WAIT(73); NT_BURN(s.out[3]);
		if (!s.i)
		{
			// nothing to draw: the accumulators jump to the segment's end
			ram[XACC + 1] = (uint16) (((int16) ram[DXS] >> 1) + ram[XACC + 1]);
			ram[YACC + 1] = (uint16) (((int16) ram[DYS] >> 1) + ram[YACC + 1]);
			NT_BURN(9); NT_WRITE(74, 0x0000);
			NT_WAIT(75); NT_WRITE(76, 0x8000);
		}
		else
		{
			NT_WRITE(77, s.out[2]);										// the lines
			NT_WAIT(78);
			NT_BURN(3);
			if (!(ram[COLOR] & 0x8000))
			{
				// $18F: four colours, each the base ($12-$14) moved t of the way to a target, channel by channel
				ram[0x22] = (uint16) s.out[2];
				ram[0x23] = (uint16) s.out[1];
				*tr = 4;
				for (s.out[4] = 0; s.out[4] < 4; s.out[4]++)
				{
					NT_BURN(s.out[4] ? 4 : 5); NT_WRITE(120, 0x0000);
					NT_WAIT(121); NT_READ(122, s.in[3]);								// t
					NT_WAIT(123); NT_BURN(2); NT_READNF(124, s.in[4]);					// the target
					NT_BURN(3); NT_READNF(125, s.in[4]);
					NT_BURN(6); NT_READ(126, s.in[4]);
					{
						const uint16	t = (uint16) s.in[3], c = (uint16) s.in[4];
						const uint16	r = (uint16) (ram[COLOR] + MulHi(t, (uint16) ((c & 0x001f) - ram[COLOR]))) & 0x001f;
						const uint16	g = (uint16) (((uint16) (ram[COLOR + 1] + MulHi(t, (uint16) ((c & 0x03e0) - ram[COLOR + 1]))) & 0x03e0) | r);
						const uint16	b = (uint16) (((uint16) (ram[COLOR + 2] + MulHi(t, (uint16) ((c & 0x7c00) - ram[COLOR + 2]))) & 0x7c00) | g);
						*trb = g;
						s.out[5] = (int16) b;
					}
					NT_BURN(9); NT_WRITE(127, s.out[5]);
					NT_WAIT(128);
					(*tr)--;
				}
				*tr = (uint16) s.out[1];
				NT_BURN(8);
			}
			ram[LINES] = (uint16) s.out[2];
			{
				const uint16	d = (uint16) s.out[1], k = (uint16) (s.out[2] - d);
				l = rom[d & 0x3ff];
				NT_BURN(4);
				if (k)
				{
					ram[SKIP] = k;
					s.i = 0;
					do
					{
						Step32(YACC, ram[DYS], l);
						Step32(XACC, ram[DXS], l);
						s.i += 14;
					} while (++ram[SKIP]);
					NT_BURN(1 + s.i);
				}
			}
			*tr = 0xff80;
			*trb = 0x0400;
			NT_BURN(4);
			for (;;)
			{
				NT_WRITE(80, ram[PTR]);											// the HDMA pointer
				ram[PTR] = (uint16) (ram[PTR] - 4);
				NT_BURN(1); NT_WAIT(81);
				s.out[0] = (int16) (uint16) (ram[YACC + 1] + ram[YENV] - ram[VBOT]);
				ram[VBOT]--;
				NT_BURN(3); NT_WRITE(82, s.out[0]);								// y scroll
				NT_BURN(1); NT_WAIT(83);
				Step32(YACC, ram[DYS], l);
				{
					const uint16	x = ram[XACC + 1];
					if ((uint16) (x + 0x80) & 0x8000)
						{ s.out[0] = (int16) 0xff80; s.i = 2; s.out[1] = 0; }
					else if ((uint16) (x - 0x380) & 0x8000)
						{ s.out[0] = (int16) x; s.i = 0; s.out[1] = 2; }
					else
						{ s.out[0] = 0x0380; s.i = 2; s.out[1] = 2; }
				}
				NT_BURN(8 + s.out[1]); NT_WRITE(84, s.out[0]);					// x scroll, -$80 to $37F
				NT_BURN(s.i); NT_WAIT(85);
				Step32(XACC, ram[DXS], l);
				NT_BURN(9);
				if (!--ram[LINES])
					break;
				NT_BURN(2);
			}
			ram[YACC + 1] = (uint16) (((int16) ram[DYS] >> 1) + ram[Y0]);
			ram[XACC + 1] = (uint16) (((int16) ram[DXS] >> 1) + ram[X0]);
			ram[YACC] = 0x8000;
			ram[XACC] = 0x8000;
			NT_BURN(11); NT_WRITE(86, 0x8000);
		}
		if (flat)
		{
			NT_BURN(2); NT_WAIT(113); NT_READ(114, s.in[0]);
			s.in[15] = s.in[0];
			if (s.in[15] >= 0)
				continue;
		flat_end:
			NT_BURN(3);
			if ((uint16) s.in[15] != 0x8000)
			{
				NT_WAIT(115); NT_BURN(1);
			}
			NT_WRITE(116, 0xffff);
			s.line = 0;
			return (true);
		}
		// $079: the next segment's distance; $8000 ends, $8001 / $8002+ a turn-off in x / y first
		for (;;)
		{
			NT_BURN(2); NT_WAIT(87); NT_READ(88, s.in[0]);
			s.in[15] = s.in[0];
			if (s.in[15] >= 0)
				break;
			if ((uint16) s.in[15] == 0x8000)
			{
				NT_BURN(3); NT_EPILOGUE(89);
				s.line = 0;
				return (true);
			}
			s.i = (uint16) s.in[15] == 0x8001;
			NT_BURN(s.i ? 6 : 6 + 2 * (uint32) ((uint16) s.in[15] - 0x8001));
			NT_WAIT(90); NT_READ(91, s.in[1]);									// its distance
			NT_WAIT(92); NT_READ(93, s.in[2]);									// the offset
			{
				const uint8		w = s.i ? WX : WY, v1 = s.i ? VX1 : VY1, acc = s.i ? XACC + 1 : XACC;
				const uint16	m = MulHi((uint16) s.in[1], (uint16) s.in[2]);
				ram[w + 0x70] = (uint16) (ram[w + 0x70] + s.in[2]);
				ram[v1] = (uint16) (ram[v1] + m);
				ram[acc] = (uint16) (ram[acc] + m);
			}
			NT_BURN(10); NT_WAIT(94); NT_READ(95, s.in[2]);						// and its step's
			{
				const uint8	d = s.i ? DWX : DWY;
				ram[d + 0x70] = (uint16) (ram[d + 0x70] + s.in[2]);
				ram[d] = 0;
			}
			NT_BURN(2);
		}
		Add32(WY, ram[DWY], ram[DWY + 0x70]);
		Add32(WX, ram[DWX], ram[DWX + 0x70]);
		*tr = 0xff00;
		*trb = 0x00ff;
		NT_BURN(1 + 16 + 4);
	}
	NT_END
}

// ---- the sprites' OAM rows: RAM $00-$77 count what each row has room for, a byte each (row 2n in word n's high
// byte), from $03/$0E; $EE/$EF point into the OAM high table at $F0-$FF and pick its 2-bit field

// $1C5 (tall, 16 rows) / $26B (8): takes a sprite's rows from the counters, false if one is out of room (C set).
// Its undo only reaches words below 0, as the firmware's does. The instructions it took into cost.
bool Reserve (uint16 row, bool tall, uint32 &cost)
{
	const int		steps = tall ? 8 : 4;
	const uint16	half = tall ? 0x0200 : 0x0100;
	uint16	b = (uint16) ((int16) row >> 1);
	int		k = 1;
	*trb = row;
	cost = 2;
	if (row & 1)
	{
		cost++;
		if (!(b & 0x8000))
		{
			// the first row alone, in its word's low byte
			*tr = half;
			const uint16	w = (uint16) (Swap(ram[(uint8) b]) - half);
			cost += 6;
			if (w & 0x8000)
			{
				cost += 2;
				return (false);
			}
			ram[(uint8) b] = Swap(w);
			b++;
			cost += 2;
		}
		else
		{
			for (k = 1; ; k++)
			{
				cost += 2;
				if (!(++b & 0x8000))
					goto rows;
				if (k == steps)
				{
					cost += 2;
					return (false);
				}
			}
		}
	}
	*tr = tall ? 0x0202 : 0x0101;
	cost += 2;
	if (b & 0x8000)
	{
		for (k = 2; ; k++)
		{
			cost += 2;
			if (!(++b & 0x8000))
				break;
			if (k == steps)
			{
				cost += tall ? 2 : 3;
				return (false);
			}
		}
	}
rows:
	for (; k < steps; k++)
	{
		const uint16	w = (uint16) (ram[(uint8) b] - *tr);
		if (w & 0x8000)
		{
			cost += 4;
			goto undo;
		}
		if (w & 0x0080)
		{
			cost += 6;
			goto undo;
		}
		ram[(uint8) b] = w;
		b++;
		cost += 7;
	}
	// the last row(s): an odd first row leaves the high byte alone, an even one the whole word, unchecked
	cost += 4;
	if (row & 1)
	{
		*tr = half;
		const uint16	w = (uint16) (ram[(uint8) b] - half);
		*tr = tall ? 0x0202 : 0x0101;
		cost += 5;
		if (w & 0x8000)
			goto undo;
		ram[(uint8) b] = w;
		cost++;
		return (true);
	}
	ram[(uint8) b] = (uint16) (ram[(uint8) b] - *tr);
	cost += 5;
	return (true);
undo:
	for (; k > 1; k--)
	{
		cost += 2;
		if (!(--b & 0x8000))
		{
			cost += 2;
			return (false);
		}
		ram[(uint8) b] = (uint16) (ram[(uint8) b] + *tr);
		cost += 4;
	}
	cost += 2;
	if (!(--b & 0x8000) || !(row & 1))
	{
		cost += (b & 0x8000) ? 4 : 2;
		return (false);
	}
	*tr = 2;
	ram[(uint8) b] = (uint16) (ram[(uint8) b] + 2);
	cost += 9;
	return (false);
}

// $403 (short) / $409 (tall): a sprite's 2 bits in the OAM high table (x bit 8, and its size), the field at
// $EF in the word $EE points at, then the next field; the instructions it takes
uint32 HighBits (uint16 x, bool tall)
{
	uint32	cost;
	*tr = 0x0100;
	if (tall)
	{
		*trb = 0xaaaa;
		cost = 4;
		if (x & 0x0100)
		{
			*trb = 0xffff;
			cost++;
		}
	}
	else
	{
		*trb = 0;
		cost = 4;
		if (x & 0x0100)
		{
			*trb = 0x5555;
			cost += 2;
		}
	}
	const uint16	mask = ram[0xef];
	const uint8		at = (uint8) ram[0xee];
	ram[at] |= (uint16) (*trb & mask);
	cost += 7;
	if (!(mask & 0x4000))
	{
		ram[0xef] = (uint16) (mask << 2);
		return (cost + 2);
	}
	ram[0xef] = (uint16) ((uint16) (mask << 4) | 3);
	ram[0xee] = (uint16) (ram[0xee] + 1);
	return (cost + 4);
}

// $3F6: an OAM entry from y (at), x (at - 1) and the attributes at $B9/$BB: 1, then y << 8 | x, then them
#define NT_ENTRY(id, at)	do {												\
	*tr = 0x00ff;																\
	*trb = ram[at] & 0xff;														\
	s.out[6] = (int16) (uint16) (*trb << 8 | (ram[(at) - 1] & 0xff));			\
	NT_BURN(6); NT_WRITE(id, 0x0001);											\
	NT_WAIT((id) + 1); NT_WRITE((id) + 2, s.out[6]);							\
	NT_WAIT((id) + 3); NT_WRITE((id) + 4, ram[(at) - 0x10]);					\
	NT_WAIT((id) + 5); NT_BURN(1); } while (0)

// $3EE: the sprite's screen position, the group's ($A8/$A9) plus its offset (y, then x)
#define NT_PLACE(id)	do {													\
	NT_BURN(1); NT_WAIT(id); NT_READ((id) + 1, s.in[6]);						\
	ram[0xc9] = (uint16) (ram[0xa9] + s.in[6]);									\
	NT_BURN(2); NT_WAIT((id) + 2); NT_READNF((id) + 3, s.in[6]);				\
	ram[0xc8] = (uint16) (ram[0xa8] + s.in[6]);									\
	NT_BURN(1); } while (0)

// $36A: a group's tall sprites to a 0, then its short ones: each placed if it's in the window ($88/$98, $89/$99)
// and its lines have room, one crossing the bottom first as a piece there ($EE); $0000, $0000 after each
#define NT_SPRITES(id)	do {													\
	for (s.reg[1] = 1; ; )														\
	{																			\
		NT_BURN(1); NT_WAIT(id); NT_READ((id) + 1, s.in[5]);					\
		ram[0xb9] = (uint16) s.in[5];											\
		if (!s.in[5])															\
		{																		\
			if (!s.reg[1])														\
			{																	\
				NT_BURN(2);														\
				break;															\
			}																	\
			s.reg[1] = 0;														\
			NT_BURN(2);															\
			continue;															\
		}																		\
		ram[0xb9] = (uint16) (s.in[5] + ram[0xba]);								\
		NT_BURN(4); NT_PLACE((id) + 2);											\
		s.i = Clip();															\
		if (s.i == CLIP_CROSS)													\
		{																		\
			ram[0xca] = ram[0xc8];												\
			ram[0xcb] = ram[0x99];												\
			ram[0xbb] = 0x00ee;													\
			if (Place(ram[0xcb], ram[0xca], 10, 3))								\
			{																	\
				NT_ENTRY((id) + 6, 0xcb);										\
				NT_BURN(3);														\
				s.i = Clip2();													\
			}																	\
			else																\
				s.i = CLIP_SKIP;												\
		}																		\
		if (s.i == CLIP_DRAW && Place(ram[0xc9], ram[0xc8], 0, 1))			\
			NT_ENTRY((id) + 12, 0xc9);											\
		NT_WRITE((id) + 18, 0x0000);											\
		NT_WAIT((id) + 19); NT_WRITE((id) + 20, 0x0000);						\
		NT_BURN(2);																\
	} } while (0)

enum { CLIP_SKIP, CLIP_DRAW, CLIP_CROSS };

// a sprite's lines reserved and its high-table bits set, up to its OAM entry ($396 / $3DC); before is owed
// ahead of the reservation, failed what a line out of room costs after it
bool Place (uint16 y, uint16 x, uint32 before, uint32 failed)
{
	uint32	c;
	if (!Reserve(y, s.reg[1] != 0, c))
	{
		NT_BURN(before + c + failed);
		return (false);
	}
	NT_BURN(before + c + 4);
	NT_BURN(HighBits(x, s.reg[1] != 0) + 2);
	return (true);
}

int	Clip2 (void);

// $36A's window test after the sprite's placed, its instructions owed; tall or short from s.reg[1]
int Clip (void)
{
	const uint16	x = ram[0xc8], y = ram[0xc9], size = s.reg[1] ? 0x0f : 0x07;
	uint32	cost = 5;
	*trb = size;
	*tr = x;
	uint16	a = (uint16) (x - ram[0x98]);
	if (a)
	{
		cost++;
		if (!(a & 0x8000))
			{ NT_BURN(cost); return (CLIP_SKIP); }
	}
	cost += 5;
	a = (uint16) (x - ram[0x88]);
	if (a & 0x8000)
	{
		cost += 2;
		if ((uint16) (a + size) & 0x8000)
			{ NT_BURN(cost); return (CLIP_SKIP); }
	}
	cost += 4;
	*tr = y;
	a = (uint16) (y - ram[0x99]);
	if (a)
	{
		cost++;
		if (!(a & 0x8000))
			{ NT_BURN(cost); return (CLIP_SKIP); }
	}
	cost += 2;
	NT_BURN(cost);
	if (!((uint16) (a + size) & 0x8000))
		return (CLIP_CROSS);
	return (Clip2());
}

// from $38F: the top, against TR and TRB as they're left (a piece at the bottom changes them)
int Clip2 (void)
{
	const uint16	a = (uint16) (*tr - ram[0x89]);
	uint32	cost = 3;
	int		r = CLIP_DRAW;
	if (a & 0x8000)
	{
		cost += 2;
		if ((uint16) (a + *trb) & 0x8000)
			r = CLIP_SKIP;
	}
	NT_BURN(cost + (r == CLIP_DRAW ? 3 : 0));
	return (r);
}

// $09/$04: the origin and window, then groups: a bottom, a distance K (0 none, negative the end, $09's $8xxx a
// nested group), the group's position at K and attributes, its sprites. $04's bottom doubles as its y.
bool Objects (void)
{
	const bool	nine = s.cmd == 0x09;
	NT_BEGIN
	NT_BURN(10); NT_WRITE(1, 0x0000);
	NT_BURN(1); NT_WAIT(2); NT_READ(3, s.in[0]); ram[0x78] = (uint16) s.in[0];
	NT_WAIT(4); NT_READ(5, s.in[0]); ram[0x79] = (uint16) s.in[0];
	NT_WAIT(6); NT_READ(7, s.in[0]); ram[0xb8] = (uint16) s.in[0];
	NT_BURN(1); NT_WAIT(8); NT_READ(9, s.in[0]); ram[0x88] = (uint16) s.in[0];
	NT_WAIT(10); NT_READ(11, s.in[0]); ram[0x98] = (uint16) s.in[0];
	NT_WAIT(12); NT_READ(13, s.in[0]); ram[0x89] = (uint16) s.in[0];
	NT_WAIT(14); NT_READ(15, s.in[0]); ram[0x99] = (uint16) s.in[0];
	for (;;)
	{
		NT_BURN(1); NT_WAIT(16);
		if (nine)
		{
			NT_READ(17, s.in[0]);
		}
		else
		{
			NT_READNF(18, s.in[0]);
		}
		{
			const uint16	b = (uint16) (s.in[0] + ram[0x79]);
			*tr = b;
			s.i = ((uint16) (b - ram[0x99]) & 0x8000) != 0;
			if (s.i)
				ram[0x99] = b;
		}
		if (nine)
		{
			NT_BURN(4 + s.i); NT_WAIT(19); NT_READ(20, s.in[1]);		// K
		}
		else
		{
			NT_BURN(4 + s.i); NT_READ(21, s.in[0]);						// the same word: the group's y
			ram[0xa9] = (uint16) s.in[0];
			NT_WAIT(22); NT_READ(23, s.in[1]);
		}
		if (!s.in[1])
		{
			NT_BURN(nine ? 2 : 3);
			continue;
		}
		if (s.in[1] < 0)
		{
			if (!nine)
			{
				NT_BURN(2); NT_EPILOGUE(24);
				s.line = 0;
				return (true);
			}
			{
				const uint16	k = (uint16) s.in[1], a = (uint16) (k << 3);
				if (k == 0x8000)
					s.i = 4;
				else if ((k & 0x6000) == 0)
					s.i = 0;
				else
					s.i = a ? 10 : 11;
			}
			if (s.i)
			{
				NT_BURN(s.i); NT_WRITE(25, 0xffff);
				s.line = 0;
				return (true);
			}
			// a nested group: its scale, two pairs projected through it, a distance for those
			*tr = 0x7f80;
			NT_BURN(10); NT_WAIT(26); NT_READ(27, s.in[0]);
			s.in[7] = (int16) (uint16) ((uint16) ((int16) s.in[0] >> 1) & 0x7f80);
			NT_BURN(3); NT_WAIT(28); NT_READ(29, s.in[0]);
			NT_WAIT(30); NT_READ(31, s.in[1]);
			ram[0xa9] = (uint16) (s.in[1] + MulHi((uint16) s.in[7], (uint16) (s.in[0] - s.in[1])));
			NT_BURN(3); NT_WAIT(32); NT_READ(33, s.in[2]);
			NT_WAIT(34); NT_READ(35, s.in[3]);
			ram[0xa8] = (uint16) s.in[3];
			s.out[0] = (int16) (uint16) (s.in[2] - s.in[3]);
			s.out[1] = (int16) (uint16) (-s.in[3] - MulHi((uint16) s.in[7], (uint16) s.out[0]));
			NT_BURN(2); NT_WAIT(36); NT_READ(37, s.in[7]);					// K
			NT_WAIT(38); NT_READNF(39, s.in[4]);
			s.out[1] = (int16) (uint16) (s.out[1] + s.in[4]);
			NT_WRITE(40, s.out[1]);
			NT_WAIT(41); NT_BURN(1); NT_WRITE(42, s.out[0]);
			ram[0xa8] = (uint16) (MulHi((uint16) s.in[7], (uint16) s.out[1]) + ram[0x78]);
			s.out[2] = (int16) ram[0xa9];
			NT_BURN(3); NT_WAIT(43); NT_READ(44, s.in[0]);
			ram[0xa9] = (uint16) (MulHi((uint16) s.in[7], (uint16) (s.out[2] + s.in[0])) + ram[0x79]);
			NT_BURN(4); NT_WAIT(45); NT_READ(46, s.in[0]);
			ram[0xba] = (uint16) s.in[0];
		}
		else
		{
			// the group at distance K: x = $78 - a + K * b, y = c + K * d + $79
			NT_BURN(nine ? 3 : 4); NT_WAIT(47); NT_READ(48, s.in[0]);
			if (nine)
			{
				NT_WAIT(49); NT_READ(50, s.in[2]);
				ram[0xa9] = (uint16) s.in[2];
			}
			NT_WAIT(51); NT_READ(52, s.in[3]);
			ram[0xa8] = (uint16) (-s.in[0] + MulHi((uint16) s.in[1], (uint16) s.in[3]) + ram[0x78]);
			NT_BURN(3); NT_WAIT(53); NT_READ(54, s.in[3]);
			ram[0xa9] = (uint16) (ram[0xa9] + MulHi((uint16) s.in[1], (uint16) s.in[3]) + ram[0x79]);
			NT_BURN(3); NT_WAIT(55); NT_READ(56, s.in[0]);
			ram[0xba] = (uint16) s.in[0];
		}
		NT_BURN(1);
		NT_SPRITES(60);
		NT_BURN(nine ? 2 : 1);
	}
	NT_END
}

bool Sprite (void)			// $0B / $0C: one sprite, short / tall: x, y and its attributes
{
	NT_BEGIN
	s.reg[1] = s.cmd == 0x0c;
	NT_BURN(11); NT_WRITE(1, 0x0000);
	NT_BURN(1); NT_WAIT(2); NT_READ(3, s.in[0]); ram[0xc8] = (uint16) s.in[0];
	NT_BURN(1); NT_WAIT(4); NT_READ(5, s.in[0]); ram[0xc9] = (uint16) s.in[0];
	NT_WAIT(6); NT_READNF(7, s.in[0]); ram[0xb9] = (uint16) s.in[0];
	{
		uint32	c;
		s.i = Reserve(ram[0xc9], s.reg[1] != 0, c);
		NT_BURN(2 + c);
	}
	if (!s.i)
	{
		NT_WRITE(8, 0x0000);
		NT_WAIT(9); NT_EPILOGUE(10);
		s.line = 0;
		return (true);
	}
	NT_BURN(3 + HighBits(ram[0xc8], s.reg[1] != 0) + 2);
	NT_ENTRY(20, 0xc9);
	NT_EPILOGUE(30);
	NT_END
}

// ---- $08: two window polygons of two edges each (entries 0-1, 2-3), stepped down the lines between distances.
// Per entry, $10 + i on: step, x, lines, line, acc lo/hi, clip right/left, last x, top, HDMA pointer, -, centre, bottom.

// $579/$573: entry i's new x (its source, plus v at distance $04) and line, and their steps from the last
void Env (int i, uint16 v)
{
	const uint16	a = (uint16) (ram[(uint8) ram[0xc0 + i]] + MulHi(ram[0x04], v));
	const uint16	d = (uint16) (a - ram[0x20 + i]);
	ram[0x20 + i] = a;
	ram[0x10 + i] = (uint16) (d << 1);
	const uint16	b = ram[(uint8) ram[0xd0 + i]];
	ram[0x30 + i] = (uint16) (b - ram[0x40 + i]);
	ram[0x40 + i] = b;
	*tr = (uint16) (0x40 + i);
}

// $631: entry i one line on, its x clipped; the instructions it took into cost
uint16 Edge (int i, uint32 &cost)
{
	const int32		p = (int32) (int16) ram[0x10 + i] * (int16) rom[ram[0x30 + i] & 0x3ff];
	const uint32	lo = (uint32) ram[0x50 + i] + (uint16) (p << 1);
	ram[0x50 + i] = (uint16) lo;
	ram[0x60 + i] = (uint16) (ram[0x60 + i] + (uint16) (p >> 15) + (lo >> 16));
	const uint16	x = (uint16) (ram[0x60 + i] + ram[0xe0 + i]);
	uint16	r;
	if (!((uint16) (x - ram[0x70 + i]) & 0x8000))
		{ r = ram[0x70 + i]; cost += 12; }
	else if ((uint16) (x - ram[0x80 + i]) & 0x8000)
		{ r = ram[0x80 + i]; cost += 13; }
	else
		{ r = x; cost += 13; }
	ram[0x90 + i] = r;
	return (r);
}

void Rewind (void)			// $587: the accumulators back to the entries' x
{
	for (int i = 0; i < 4; i++)
	{
		ram[0x60 + i] = ram[0x20 + i];
		ram[0x50 + i] = 0;
	}
}

// $5B9 / $5EE: a polygon's lines past those below its bottom, each its HDMA pointer and right << 8 | left;
// split: $5EE, whose polygon can run up instead ($30 > 0)
#define NT_POLY(id, e, split)	do {											\
	s.reg[3] = (e);																\
	s.i = (int32) (uint16) (0 - ram[0x30 + (e)]);								\
	NT_BURN(3);																	\
	if (split && (s.i & 0x8000))												\
	{																			\
		NT_BURN(1);																\
		if (!PolyUp())													\
			goto poly##id##_none;														\
		NT_WRITE((id) + 10, *tr);												\
		for (;;)																\
		{																		\
			{																	\
				uint32	c = 0;													\
				*trb = Edge(0, c);												\
				NT_BURN(1 + c + 1);												\
			}																	\
			NT_WAIT((id) + 11);													\
			s.out[9] = (int16) (uint16) (ram[0xb0] + 6);						\
			NT_BURN(2); NT_WRITE((id) + 12, s.out[9]);							\
			ram[0xb0] = ram[0xb1] = (uint16) (s.out[9] - 2);					\
			{																	\
				uint32	c = 0;													\
				s.out[9] = (int16) (uint16) (Swap(Edge(1, c)) | *trb);			\
				NT_BURN(4 + 1 + c + 2);											\
			}																	\
			NT_WAIT((id) + 13); NT_WRITE((id) + 14, s.out[9]);					\
			NT_BURN(4);															\
			if (!--*tr)															\
				break;															\
		}																		\
		NT_WAIT((id) + 15); NT_BURN(1);											\
		break;																	\
	}																			\
	if (!split && (s.i & 0x8000))												\
		goto poly##id##_none;															\
	NT_BURN(1);																	\
	if (!s.i || !PolyDown(e))										\
		goto poly##id##_none;															\
	NT_WRITE((id) + 1, *tr);													\
	for (;;)																	\
	{																			\
		{																		\
			uint32	c = 0;														\
			const uint8	h = (uint8) (0xb0 + (e)), z = (uint8) (e);				\
			c += (ram[h] < ram[z]) ? 6 : 5;										\
			if (ram[h] < ram[z])												\
				ram[0x80 + (e)] = 0;											\
			*trb = Edge((e), c);												\
			NT_BURN(1 + c + 1 + 1);												\
		}																		\
		NT_WAIT((id) + 2); NT_WRITE((id) + 3, ram[0xb0 + (e)]);					\
		ram[0xb0 + (e)] = ram[0xb1 + (e)] = (uint16) (ram[0xb0 + (e)] - 4);		\
		{																		\
			uint32	c = 0;														\
			const uint8	h = (uint8) (0xb1 + (e)), z = (uint8) ((e) + 1);		\
			c += (ram[h] < ram[z]) ? 6 : 5;										\
			if (ram[h] < ram[z])												\
				ram[0x71 + (e)] = 0x00ff;										\
			s.out[9] = (int16) (uint16) (Swap(Edge((e) + 1, c)) | *trb);		\
			NT_BURN(4 + 1 + c + 1 + 2);											\
		}																		\
		NT_WAIT((id) + 4); NT_WRITE((id) + 5, s.out[9]);						\
		NT_BURN(4);																\
		if (!--*tr)																\
			break;																\
	}																			\
	NT_WAIT((id) + 6); NT_BURN(1);												\
	break;																		\
poly##id##_none:																		\
	NT_WRITE((id) + 7, 0x0000);													\
	NT_BURN(1); NT_WAIT((id) + 8); NT_BURN(1);									\
	} while (0)

// $5BC on: lines counted down from the bottom, the ones below it skipped, clipped at the top; false: none
bool PolyDown (int e)
{
	const uint16	n = (uint16) s.i;
	uint32	cost = 0;
	ram[0x30 + e] = ram[0x31 + e] = n;
	*tr = n;
	const uint16	a = (uint16) (n + ram[0x40 + e] - ram[0xf0 + e]);
	cost += 5;
	if (a)
	{
		*trb = a;
		const uint16	b = (uint16) (n - a);
		cost += 3;
		if ((b & 0x8000) || (cost++, !b))
		{
			NT_BURN(cost);
			return (false);
		}
		*tr = b;
		cost++;
		do
		{
			cost += 2;
			Edge(e, cost);
			Edge(e + 1, cost);
			cost += 4;
		} while (--*trb);
		cost++;
	}
	ram[0xf0 + e] = ram[0x40 + e];
	const uint16	t = (uint16) (ram[0x40 + e] - ram[0xa0 + e]);
	cost += 4;
	if (t & 0x8000)
	{
		const uint16	u = (uint16) (t + *tr);
		cost += 2;
		if ((u & 0x8000) || (cost++, !u))
		{
			NT_BURN(cost);
			return (false);
		}
		*tr = u;
		cost++;
	}
	NT_BURN(cost);
	return (true);
}

// $5F2 on: polygon 0's lines counted up from its line, those above the bottom skipped first; false: none
bool PolyUp (void)
{
	uint32	cost = 4;
	*tr = ram[0x30];
	const uint16	a = (uint16) (ram[0x40] - ram[0xf0]);
	if (!(a & 0x8000))
	{
		const uint16	b = (uint16) (*tr - a);
		cost += 3;
		if ((b & 0x8000) || (cost++, !b))
		{
			NT_BURN(cost);
			return (false);
		}
		*tr = b;
		cost++;
	}
	uint16	v = (uint16) (ram[0x40] - *tr - ram[0xa0] - 1);
	cost += 5;
	if (v & 0x8000)
	{
		*trb = v;
		const uint16	u = (uint16) (v + *tr);
		cost += 3;
		if ((u & 0x8000) || (cost++, !u))
		{
			NT_BURN(cost);
			return (false);
		}
		*tr = u;
		cost++;
		do
		{
			cost += 2;
			Edge(0, cost);
			Edge(1, cost);
			cost += 4;
		} while (++*trb);
	}
	NT_BURN(cost);
	return (true);
}

uint32	ClipLeft (int i);
uint32	ClipRight (int i);

// $510-$53D, the polygons apart: the first's HDMA pointers start, its clips follow the second's; the instructions
uint32 Apart (void)
{
	uint32	cost = 4;
	if (ram[0x10] & 0x8000)
	{
		cost += 3;
		if (!ram[0x02])
		{
			ram[0x02] = ram[0xb0] & 0xfffc;
			cost += 5;
		}
	}
	cost += 4;
	if (!(ram[0x11] & 0x8000))
	{
		cost++;
		if (ram[0x11])
		{
			cost += 3;
			if (!ram[0x03])
			{
				ram[0x03] = ram[0xb1] & 0xfffc;
				cost += 5;
			}
		}
	}
	cost += 2 + ClipLeft(2) + 2 + ClipRight(3) + 3;
	bool	both = true;
	if (!ram[0x09])
	{
		if (ram[0x30] & 0x8000)
			{ both = false; cost += 3; }
		else if (!ram[0x30])
			{ both = false; cost += 4; }
		else
			{ ram[0x09] = ram[0x30]; cost += 6; }
	}
	if (both)
	{
		cost += 2 + ClipLeft(0) + 2 + ClipRight(1) + 4;
		ram[0xf0] = ram[0xf1] = ram[0xf2];
	}
	return (cost);
}

bool Edges (void)
{
	NT_BEGIN
	NT_BURN(10); NT_WRITE(1, 0x0000);
	// $553 x 8: clip right and left, the sources' pointers, centres, HDMA pointers, bottoms, tops
	for (s.i = 0; s.i < 32; s.i++)
	{
		NT_BURN(!s.i ? 3 : (s.i & 3) ? 1 : 5); NT_WAIT(2); NT_READ(3, s.in[0]);
		{
			static const uint8	at[8] = { 0x70, 0x80, 0xc0, 0xd0, 0xe0, 0xb0, 0xf0, 0xa0 };
			ram[at[s.i >> 2] + (s.i & 3)] = (uint16) s.in[0];
		}
	}
	ram[0x09] = 0;
	NT_BURN(7);
	for (s.i = 0; s.i < 4; s.i++)
	{
		NT_WAIT(4); NT_READ(5, s.in[0]);
		ram[0x05 + s.i] = (uint16) s.in[0];
	}
	// $561: the distance (the first one's sign doesn't matter), then the views' x and y
	for (s.reg[2] = 1; ; s.reg[2] = 0)
	{
		NT_BURN(3); NT_WAIT(6); NT_READ(7, s.in[0]);
		ram[0x04] = (uint16) s.in[0];
		if (s.in[0] >= 0)
		{
			NT_BURN(3); NT_WAIT(8); NT_READ(9, s.in[0]);
			ram[0xa4] = (uint16) -s.in[0];
			NT_BURN(1); NT_WAIT(10); NT_READ(11, s.in[0]);
			ram[0xa5] = (uint16) s.in[0];
			NT_WAIT(12); NT_READ(13, s.in[0]);
			ram[0xa6] = (uint16) -s.in[0];
			NT_BURN(1); NT_WAIT(14); NT_READ(15, s.in[0]);
			ram[0xa7] = (uint16) s.in[0];
		}
		else if (!s.reg[2])
		{
			// the end: the first polygon's line count ($09) back
			NT_BURN(1 + 1 + 1 + 1); NT_WRITE(16, ram[0x09]);
			NT_WAIT(17); NT_EPILOGUE(18);
			s.line = 0;
			return (true);
		}
		else
			NT_BURN(2);
		// the four sources' changes: entries 0-3 (the last one read without asking for more)
		s.out[10] = (int16) (s.reg[2] ? 0 : (ram[0x41] == ram[0x42]) ? 1 : 2);
		NT_BURN(s.reg[2] ? 3 : 1 + 4 + 1);
		for (s.i = 0; s.i < 4; s.i++)
		{
			NT_BURN(4); NT_WAIT(20);
			if (s.i < 3)
			{
				NT_READ(21, s.in[0]);
			}
			else
			{
				NT_READNF(22, s.in[0]);
				NT_BURN(1);
			}
			Env(s.i, (uint16) s.in[0]);
			NT_BURN(9);
		}
		if (s.reg[2])
		{
			// $501: the window's start, entries 0 and 1 as they stand
			Rewind();
			ram[0x10] = ram[0x11] = 0;
			{
				uint32	c = 0;
				const uint16	l = Edge(0, c);
				*tr = l;
				s.out[9] = (int16) (uint16) (Swap(Edge(1, c)) | l);
				NT_BURN(1 + 13 + 1 + 1 + 3 + 1 + 1 + 1 + 2 + c);
			}
			NT_WRITE(30, s.out[9]);
			NT_WAIT(31); NT_WRITE(32, 0x8000);
			Rewind();
			for (int i = 0; i < 4; i++)
				ram[i] = 0;
			NT_BURN(1 + 13 + 1 + 1 + 6);
			continue;
		}
		if (s.out[10] == 2)
		{
			NT_BURN(Apart() + 2);
			NT_POLY(40, 0, true);
		}
		else
		{
			NT_BURN(2);
			NT_POLY(60, 0, false);
		}
		NT_BURN(2);
		NT_POLY(80, 2, false);
		Rewind();
		NT_BURN(s.out[10] == 2 ? 1 + 1 + 13 : 1 + 13);
		NT_WRITE(100, 0x8000);
		NT_BURN(1);
	}
	NT_END
}

// $5A7: a falling edge's x (accumulator + centre) pulls its clip left in; the instructions
uint32 ClipLeft (int i)
{
	if (!(ram[0x10 + i] & 0x8000))
		return (4);
	const uint16	x = (uint16) (ram[0x60 + i] + ram[0xe0 + i]);
	if (!((uint16) (x - ram[0x80 + i]) & 0x8000))
		ram[0x80 + i] = x;
	return (8);
}

// $5B0: a rising edge's pulls its clip right in
uint32 ClipRight (int i)
{
	if (ram[0x10 + i] & 0x8000)
		return (4);
	const uint16	x = (uint16) (ram[0x60 + i] + ram[0xe0 + i]);
	if ((uint16) (x - ram[0x70 + i]) & 0x8000)
		ram[0x70 + i] = x;
	return (8);
}

bool NoOp (void)			// bits above the low four with a low four of 5-15: straight back
{
	NT_BEGIN
	NT_BURN(s.cmd == 0x1f ? 9 : 10); NT_WRITE(1, 0xffff);
	NT_END
}

bool Unknown (void)			// not yet native
{
	NT_BEGIN
	NT_BURN(0);
	NT_END
}

// ---- power-on and the command wait

bool Boot (void)			// $000: DR = $FFFF
{
	NT_BEGIN
	NT_WRITE(1, 0xffff);
	NT_END
}

bool Idle (void)			// $001-$002: the command, a word; above the low four bits only $10-$14 mean anything
{
	NT_BEGIN
	NT_WAIT(1); NT_READNF(2, s.in[0]);
	s.cmd = (uint8) ((s.in[0] & 0x0f) | (((uint16) s.in[0] >> 4) ? 0x10 : 0));
	NT_END
}

bool (*Program (uint8 cmd)) (void)
{
	switch (cmd)
	{
		case 0x00:	return (Multiply);
		case 0x01:	return (Road);
		case 0x07:	return (Road);
		case 0x10:	return (Road);
		case 0x0d:	return (Road);
		case 0x0f:	return (Road);
		case 0x02:	return (Version);
		case 0x03:	return (Fill);
		case 0x04:	return (Objects);
		case 0x08:	return (Edges);
		case 0x09:	return (Objects);
		case 0x0b:	return (Sprite);
		case 0x0c:	return (Sprite);
		case 0x05:	return (Clear);
		case 0x06:	return (Dump);
		case 0x0e:	return (Fill);
		case 0x0a:	return (Blend);
		case 0x11:	return (Pack);
		case 0x12:	return (MemTest);
		case 0x13:	return (RomDump);
		case 0x14:	return (Revision);
		default:	return (cmd > 0x14 ? NoOp : Unknown);
	}
}

void ChipReset (bool variant)
{
	(void) variant;
	BuildRom();
}

void ChipLoaded (void)
{
	BuildRom();
}

}	// namespace

const S9xUPD7725Native	S9xDSP4Native = NT_CHIP;

#ifdef UPD7725N_LAB
bool S9xDSP4NLabKnows (int cmd)
{
	const uint8	c = (uint8) ((cmd & 0x0f) | (((uint16) cmd >> 4) ? 0x10 : 0));
	return (Program(c) != Unknown);
}

bool S9xDSP4NLabReserve (uint16 row, bool tall, uint32 *cost)
{
	return (Reserve(row, tall, *cost));
}

const uint16 *S9xDSP4NLabROM (void)
{
	BuildRom();
	return (rom);
}
#endif
