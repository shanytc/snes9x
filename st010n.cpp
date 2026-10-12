/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native ST010 (F1 ROC II, Exhaust Heat II): the chip's eight commands on its own
// clock, with every access to the RAM it shares with the CPU at the instruction the
// firmware makes it, and no dump. The CPU puts a command's inputs in that RAM, the
// command in byte $20 and $80 in byte $21; the chip clears both when it is done.

#include <string.h>
#include "snes9x.h"
#include "upd96050n.h"

namespace {

#include "st010n_rom.h"

// Saved as-is, so the layout is fixed.
struct State
{
	int32	line;		// where the program resumes (0: power-on)
	int32	sub;		// where the heading routine resumes, inside a command
	uint32	due;		// instructions owed before the next access
	int32	i, j;		// loop counters
	uint16	a, b, tr, trb, k, l;
	uint16	dp, rp;
	uint16	v[8];		// a command's values across its accesses
	uint16	h[4];		// the heading routine's
	uint8	carry, pad[3];
};
static_assert(sizeof(State) == 64, "savestate layout");

State	s;

#include "upd96050np.h"

const uint16	SR_RQM = 0x8000;

// DPINC and DPDEC step DP's low nibble and keep only its low byte; M1 flips bit 4.
inline uint16 DpInc (uint16 dp) { return ((dp & 0xf0) | ((dp + 1) & 0x0f)); }
inline uint16 DpDec (uint16 dp) { return ((dp & 0xf0) | ((dp - 1) & 0x0f)); }

// SHR1 keeps the sign
inline uint16 Asr (uint16 v, int n) { return ((uint16) ((int16) v >> n)); }

// sine and cosine of an angle's high byte, from the data ROM
inline uint16 Sin (uint16 angle) { return (kDataRom[0xb0 + (angle >> 8)]); }
inline uint16 Cos (uint16 angle) { return (kDataRom[0xf0 + (angle >> 8)]); }

// The heading routine (cmd 1, and cmd 5's first step): the angle of (x, y) in words 0 and 1 to word 8,
// and in s.h[0]; it leaves the quadrant in word 2, y in word 3 and the reduced |x|, |y| in words 0 and 1.
bool Heading (void)
{
	NT_SUB_BEGIN
	NT_BURN(1);
	NT_SUB_AT(1);  s.h[0] = Rd(1);										// y
	NT_BURN(1);
	NT_SUB_AT(2);  Wr(3, s.h[0]);
	NT_BURN(2);
	NT_SUB_AT(3);  s.h[1] = Rd(0) & 0x8000;								// x's sign
	NT_SUB_AT(4);  s.h[2] = (uint16) ((s.h[1] ^ Rd(1)) & 0x8000);		// signs differ
	NT_BURN(6);
	s.h[1] |= Asr(s.h[2], 1) & 0x4000;									// the quadrant
	NT_SUB_AT(5);  Wr(2, s.h[1]);

	// |x|, then |y|
	NT_SUB_AT(6);  s.a = Rd(0);
	NT_SUB_AT(7);  s.a &= Rd(0);
	NT_BURN(1);
	if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
	NT_SUB_AT(8);  Wr(0, s.a);
	NT_SUB_AT(9);  s.a = Rd(1);
	NT_SUB_AT(10); s.a &= Rd(1);
	NT_BURN(1);
	if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
	NT_SUB_AT(11); Wr(1, s.a);

	// halve both until the larger is under 32, keeping a nonzero smaller one at least 1
	NT_SUB_AT(12); s.a = Rd(0);
	NT_SUB_AT(13); s.carry = s.a < Rd(1);
	NT_BURN(1);
	if (!s.carry)
	{
		NT_SUB_AT(14); s.a = Rd(0);
		NT_SUB_AT(15); s.b = Rd(1);
		NT_BURN(1);
		for (;;)
		{
			NT_BURN(4);
			if (s.a < 0x20)
				break;
			s.carry = s.b & 1;
			s.a = Asr(s.a, 1);
			s.b = Asr(s.b, 1);
			NT_BURN(3);
			if (s.b)
				continue;
			NT_BURN(1);
			if (!s.carry)
				continue;
			NT_BURN(2);
			s.b = 1;
		}
	}
	else
	{
		NT_SUB_AT(16); s.a = Rd(0);
		NT_SUB_AT(17); s.b = Rd(1);
		NT_BURN(1);
		for (;;)
		{
			NT_BURN(4);
			if (s.b < 0x20)
				break;
			s.carry = s.a & 1;
			s.b = Asr(s.b, 1);
			s.a = Asr(s.a, 1);
			NT_BURN(3);
			if (s.a)
				continue;
			NT_BURN(1);
			if (!s.carry)
				continue;
			NT_BURN(2);
			s.a = 1;
		}
	}
	NT_SUB_AT(18); Wr(0, s.a);
	NT_SUB_AT(19); Wr(1, s.b);
	NT_SUB_AT(20); s.a &= Rd(0);
	NT_BURN(1);

	if (!s.a)
	{
		// straight up or down
		NT_BURN(2);
		NT_SUB_AT(21); s.a = Rd(2) & 0x4000;
		NT_BURN(1);
		if (s.a)
		{
			NT_BURN(2);
			NT_SUB_AT(22); s.a = (uint16) ((Rd(3) ^ 0x8000) & 0x8000);
			NT_BURN(3);
			NT_SUB_AT(23); Wr(8, s.a);
			s.h[0] = s.a;
			s.sub = 0;
			return (true);
		}
	}
	else
	{
		NT_BURN(2);
		NT_SUB_AT(24); s.a = Rd(2) & 0x4000;
		NT_BURN(1);
		if (s.a)
		{
			// the quadrants where x and y differ in sign read the table the other way
			NT_BURN(1);
			NT_SUB_AT(25); s.tr = Rd(0);
			NT_SUB_AT(26); s.trb = Rd(1);
			NT_SUB_AT(27); Wr(0, s.trb);
			NT_SUB_AT(28); Wr(1, s.tr);
		}
	}

	// the arctangent table by (x, y), with the quadrant
	NT_BURN(1);
	NT_SUB_AT(29); s.a = (uint16) (((Rd(0) << 5) & 0xffe0) + 0x1f0);
	NT_BURN(6);
	NT_SUB_AT(30); s.a = (uint16) (s.a + Rd(1));
	NT_BURN(2);
	s.a = (s.a & 0x7ff) < 0x5f0 ? kDataRom[s.a & 0x7ff] : 0;
	NT_SUB_AT(31); s.a |= Rd(2);
	NT_BURN(3);
	s.a ^= 0x8000;
	NT_SUB_AT(32); Wr(8, s.a);
	s.h[0] = s.a;
	NT_SUB_END
}

// $0F00 x the sine or cosine of each raster line's angle, one table of 176 lines to the RAM from `top` down
#define RASTER(id, top, factor)																		\
	s.k = (factor);																					\
	for (s.i = 0; s.i < 176; s.i++)																	\
	{																								\
		NT_BURN(2);																					\
		NT_AT(id); Wr((top) - s.i, Mul(s.k, kDataRom[0xaf - s.i]).m);								\
		NT_BURN(4);																					\
	}

// inc and dec of the sort's DP, with the nibble carry the firmware patches in by hand
#define SORT_INC	s.dp = DpInc(s.dp); NT_BURN(1); if (!(s.dp & 0x0f)) { NT_BURN(1); s.dp ^= 0x10; }
#define SORT_DEC	s.dp = DpDec(s.dp); NT_BURN(1); if ((s.dp & 0x0f) == 0x0f) { NT_BURN(1); s.dp ^= 0x10; }

bool Main (void)
{
	NT_BEGIN

	// power-on: DR = 0 for the CPU, which reads it before the chip goes on (a JRQM on itself)
	NT_AT(1); *dr = 0; *sr |= SR_RQM;
	s.due = 1;
	s.line = 2; case 2:
	if (!Pay(s.due))
		return (false);
	if (*sr & SR_RQM)
	{
		s.due = 1;
		executed += budget;
		budget = 0;
		return (false);
	}
	s.line = 4; case 4:
	NT_BURN(1);

	// the command wait: word $10 = command | $80 << 8
	for (;;)
	{
		NT_AT(10); s.a = Rd(0x10);
		NT_AT(11); s.a &= Rd(0x10);
		NT_BURN(1);
		if (s.a & 0x8000)
			break;
		// the CPU syncs the chip before it writes, so every whole turn left in this run reads the same
		s.line = 12; case 12:
		if (!Pay(s.due))
			return (false);
		if (!(ram[0x21] & 0x80))
		{
			const uint64	turns = budget / 3;
			executed += turns * 3;
			budget -= turns * 3;
		}
	}

	if ((s.a & 15) == 0)
		NT_BURN(9);			// 0 (and $10...): nothing
	else if ((s.a & 7) == 0)
	{
		// 8: rotate (x, y) by an angle
		NT_BURN(8);
		NT_BURN(1);
		NT_AT(100); s.v[0] = Rd(2);
		NT_BURN(4 + 9);
		s.v[1] = Sin(s.v[0]); s.v[2] = Cos(s.v[0]);
		NT_BURN(2);
		NT_AT(101); s.v[3] = Rd(1);														// y
		NT_BURN(5);
		NT_AT(102); s.v[4] = Rd(0);														// x
		NT_BURN(3);
		s.a = (uint16) (Mul(s.v[1], s.v[3]).m + Mul(s.v[2], s.v[4]).m);
		s.b = (uint16) (Mul(s.v[2], s.v[3]).m - Mul(s.v[1], s.v[4]).m);
		NT_AT(103); Wr(8, s.a);
		NT_AT(104); Wr(9, s.b);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 4)
	{
		// the length of (x, y): $3D78/32768 the longer side plus $1976/32768 the shorter
		NT_BURN(6);
		NT_BURN(1);
		NT_AT(200); s.a = Rd(0);
		NT_AT(201); s.a &= Rd(0);
		NT_BURN(1);
		if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
		NT_AT(202); Wr(0, s.a);
		NT_AT(203); s.a = Rd(1);
		NT_AT(204); s.a &= Rd(1);
		NT_BURN(1);
		if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
		NT_AT(205); Wr(1, s.a);
		NT_AT(206); s.carry = s.a < Rd(0);
		NT_BURN(1);
		if (!s.carry)
		{
			NT_AT(207); s.tr = Rd(0);
			NT_AT(208); s.trb = Rd(1);
			NT_AT(209); Wr(0, s.trb);
			NT_AT(210); Wr(1, s.tr);
		}
		NT_BURN(4);
		NT_AT(211); s.v[0] = Rd(0);
		NT_BURN(3);
		NT_AT(212); s.v[1] = Rd(1);
		NT_BURN(9);
		{
			const Product	p = Mul(0x3d78, s.v[0]), q = Mul(0x1976, s.v[1]);
			const uint32	lo = (uint32) p.n + q.n;
			const uint32	hi = (uint32) p.m + q.m + (lo >> 16);
			// doubled, the carry out of the sum shifted in, then rounded
			const uint32	lo2 = ((lo << 1) & 0xffff) | ((hi >> 16) & 1);
			const uint32	hi2 = ((hi << 1) & 0xffff) | ((lo >> 15) & 1);
			s.a = (uint16) (hi2 + ((lo2 + 0x8000) >> 16));
		}
		NT_AT(213); Wr(8, s.a);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 2)
	{
		// sort: driver places in words $20+ (with words $40+ along) into descending order
		NT_BURN(6);
		NT_BURN(1);
		NT_AT(300); s.b = Rd(0x12);
		NT_AT(301); s.a = Rd(0x12);
		NT_BURN(5);
		s.rp = (uint16) ((s.b - 1 + 0x20) & 0x7ff);
		NT_AT(302); Wr(0, s.a);
		NT_BURN(3);
		if (s.a < 2)
			goto done;
		NT_AT(303); s.a = Rd(0);
		NT_BURN(3);
		if (s.a >= 0x10)
			goto done;
		do
		{
			NT_BURN(2);
			s.tr = 0x20;
			s.b = 0x40;
			do
			{
				NT_BURN(1);
				s.dp = s.tr & 0x7ff;
				NT_AT(304); s.a = Rd(s.dp);
				SORT_INC
				NT_AT(305); s.trb = Rd(s.dp);
				SORT_DEC
				NT_BURN(2);
				if (s.a < s.trb)
				{
					NT_AT(306); s.trb = Rd(s.dp);
					SORT_INC
					NT_AT(307); s.a = Rd(s.dp);
					SORT_DEC
					NT_AT(308); Wr(s.dp, s.a);
					SORT_INC
					NT_AT(309); Wr(s.dp, s.trb);
					NT_BURN(1);
					s.dp = s.b & 0x7ff;
					NT_AT(310); s.trb = Rd(s.dp);
					SORT_INC
					NT_AT(311); s.a = Rd(s.dp);
					SORT_DEC
					NT_AT(312); Wr(s.dp, s.a);
					SORT_INC
					NT_AT(313); Wr(s.dp, s.trb);
				}
				NT_BURN(5);
				s.b++;
				s.tr++;
			} while (s.tr < s.rp);
			NT_BURN(4);
			s.rp = (s.rp - 1) & 0x7ff;
			NT_AT(314); s.a = Rd(0);
			NT_BURN(1);
			s.a--;
			NT_AT(315); Wr(0, s.a);
			NT_BURN(1);
		} while (s.a);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 6)
	{
		// multiply: words 8-9 = word 0 x word 1, doubled
		NT_BURN(7);
		NT_BURN(1);
		NT_AT(400); s.l = Rd(0);
		NT_AT(401); s.k = Rd(1);
		NT_BURN(3);
		NT_AT(402); Wr(8, Mul(s.k, s.l).n);
		NT_BURN(2);
		NT_AT(403); Wr(9, Mul(s.k, s.l).m);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 1)
	{
		// heading
		NT_BURN(6);
		NT_BURN(1);
		s.sub = 0;
		NT_CALL(500, Heading);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 5)
	{
		// navigation: steer a car at (x0, y0) toward (MaxX, MaxY)
		NT_BURN(7);
		NT_BURN(1);
		NT_AT(600); s.a = Rd(0x60);
		NT_BURN(1);
		NT_AT(601); s.a = (uint16) (s.a - Rd(0x63));
		NT_BURN(1);
		NT_AT(602); Wr(0x68, s.a);
		NT_BURN(1);
		NT_AT(603); s.b = Rd(0x61);
		NT_BURN(1);
		NT_AT(604); s.b = (uint16) (s.b - Rd(0x65));
		NT_BURN(1);
		NT_AT(605); Wr(0x69, s.b);
		NT_BURN(1);
		NT_AT(606); Wr(0, s.a);
		NT_AT(607); Wr(1, s.b);
		NT_BURN(1);
		s.sub = 0;
		NT_CALL(608, Heading);

		// turn Theta0 toward the heading, at most $280 a step
		NT_BURN(1);
		NT_AT(609); s.a = (uint16) (s.h[0] - Rd(0x66));
		NT_BURN(1);
		NT_AT(610); Wr(0x67, s.a);
		NT_BURN(3);
		if (s.a & 0xff00)
		{
			NT_BURN(1);
			if (s.a & 0x8000) { NT_BURN(2); s.tr = 0xfd80; }
			else { NT_BURN(1); s.tr = 0x0280; }
			NT_BURN(1);
			NT_AT(611); s.a = (uint16) (Rd(0x66) + s.tr);
			NT_BURN(1);
			NT_AT(612); Wr(0x66, s.a);
		}
		NT_BURN(1);

		// a sharp turn slows the car; a gentle one speeds it up to its top speed
		NT_AT(613); s.a = Rd(0x67);
		NT_BURN(2);
		if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
		NT_BURN(5);
		s.trb = Asr(s.a, 4);
		NT_BURN(3);
		if (s.trb >= 0x100)
		{
			NT_BURN(1);
			NT_AT(614); s.a = Rd(0x6a);
			NT_BURN(2);
			if (s.a < s.trb) { NT_BURN(1); s.a = 0; }
			else s.a = (uint16) (s.a - s.trb);
			NT_AT(615); Wr(0x6a, s.a);
			NT_BURN(1);
		}
		else
		{
			NT_BURN(1);
			NT_AT(616); s.a = Rd(0x6a);
			NT_BURN(1);
			NT_AT(617); s.v[0] = Rd(0x6b); s.carry = (uint32) s.a + s.v[0] > 0xffff; s.a = (uint16) (s.a + s.v[0]);
			NT_BURN(1);
			if (s.carry) { NT_BURN(1); s.a = 0xffff; }
			NT_BURN(2);
			s.tr = s.a;
			NT_AT(618); s.carry = s.a < Rd(0x6c);
			NT_BURN(1);
			if (!s.carry)
			{
				NT_AT(619); s.tr = Rd(0x6c);
			}
			NT_BURN(1);
			NT_AT(620); Wr(0x6a, s.tr);
		}

		// move along Theta0 by the speed's high byte
		NT_BURN(1);
		NT_AT(621); s.v[0] = Rd(0x66);
		NT_BURN(3 + 10 + 10 + 2 + 1);
		s.k = Asr(Sin(s.v[0]), 5);
		s.trb = Asr(Cos(s.v[0]), 5);
		NT_AT(622); s.l = Rd(0x6a) >> 8;
		NT_BURN(9);
		NT_AT(623); Wr(0, Mul(s.k, s.l).n);
		NT_AT(624); Wr(1, Mul(s.k, s.l).m);
		NT_BURN(5 + 1);
		NT_AT(625); Wr(2, Mul(s.trb, s.l).n);
		NT_AT(626); Wr(3, Mul(s.trb, s.l).m);

		// x0 -= the sine step, y0 -= the cosine step, each 29 bits
		NT_BURN(1);
		NT_AT(627); s.a = Rd(0x62);
		NT_AT(628); s.b = Rd(0x63);
		NT_BURN(1);
		NT_AT(629); s.v[1] = Rd(0); s.carry = s.a < s.v[1]; s.a = (uint16) (s.a - s.v[1]);
		NT_AT(630); s.b = (uint16) ((s.b - Rd(1) - s.carry) & 0x1fff);
		NT_BURN(3);
		NT_AT(631); Wr(0x62, s.a);
		NT_AT(632); Wr(0x63, s.b);
		NT_BURN(1);
		NT_AT(633); s.a = Rd(0x64);
		NT_AT(634); s.b = Rd(0x65);
		NT_BURN(1);
		NT_AT(635); s.v[1] = Rd(2); s.carry = s.a < s.v[1]; s.a = (uint16) (s.a - s.v[1]);
		NT_AT(636); s.b = (uint16) ((s.b - Rd(3) - s.carry) & 0x1fff);
		NT_BURN(3);
		NT_AT(637); Wr(0x64, s.a);
		NT_AT(638); Wr(0x65, s.b);

		// close enough to the target (along the compass's axis): take the next one
		NT_BURN(1);
		NT_AT(639); s.a = Rd(0x6d);
		NT_BURN(2);
		if (s.a & 0x8000) { NT_BURN(3); s.tr = 0x08; s.trb = 0x80; }
		else { NT_BURN(2); s.tr = 0x80; s.trb = 0x08; }
		NT_BURN(1);
		NT_AT(640); s.a = Rd(0x68);
		NT_BURN(2);
		if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
		NT_BURN(2);
		if (s.a >= s.tr)
		{
			NT_BURN(1);
			goto done;
		}
		NT_AT(641); s.a = Rd(0x69);
		NT_BURN(2);
		if (s.a & 0x8000) { NT_BURN(2); s.a = Abs(s.a); }
		NT_BURN(2);
		if (s.a >= s.trb)
		{
			NT_BURN(1);
			goto done;
		}
		NT_BURN(1);
		NT_AT(642); s.a = Rd(0x6f);
		NT_BURN(1);
		NT_AT(643); Wr(0x60, s.a);
		NT_BURN(1);
		NT_AT(644); s.a = Rd(0x70);
		NT_BURN(2);
		s.v[0] = (s.a & 0x8000) ? 0xffff : 0x0000;
		NT_BURN(3);
		NT_AT(645); Wr(0x61, s.a & 0x0fff);
		NT_BURN(2);
		NT_AT(646); Wr(0x6d, s.v[0]);
		if (s.v[0]) NT_BURN(1);
		NT_BURN(1);
		NT_AT(647); s.a = Rd(0x6e);
		NT_BURN(2);
		NT_AT(648); Wr(0x6e, s.a | 8);
		NT_BURN(1);
	}
	else if ((s.a & 7) == 3)
	{
		// scale: words 8-9 = x x scale, 10-11 = y x scale, each doubled
		NT_BURN(6);
		NT_BURN(1);
		NT_AT(700); s.k = Rd(2);
		NT_AT(701); s.l = Rd(1);
		NT_BURN(3);
		s.v[0] = Mul(s.k, s.l).n; s.v[1] = Mul(s.k, s.l).m;
		NT_AT(702); s.l = Rd(0);
		NT_BURN(7);
		NT_AT(703); Wr(8, Mul(s.k, s.l).n);
		NT_AT(704); Wr(9, Mul(s.k, s.l).m);
		NT_AT(705); Wr(10, s.v[0]);
		NT_AT(706); Wr(11, s.v[1]);
		NT_BURN(1);
	}
	else
	{
		// 7: Mode 7 raster scales for an angle: four tables of 176 lines
		NT_BURN(7);
		NT_BURN(1);
		NT_AT(800); s.v[0] = (Rd(0) >> 8) & 0xff;
		NT_BURN(3);
		NT_AT(801); Wr(0, s.v[0]);
		NT_BURN(10);
		s.v[1] = kDataRom[0xb0 + s.v[0]]; s.v[2] = kDataRom[0xf0 + s.v[0]];
		NT_AT(802); Wr(1, s.v[1]);
		NT_AT(803); Wr(2, s.v[2]);
		NT_BURN(1);
		NT_AT(804); s.v[3] = Rd(2);
		NT_BURN(4);
		RASTER(805, 0x127, s.v[3])
		NT_BURN(1);
		NT_AT(806); s.v[3] = Rd(1);
		NT_BURN(4);
		RASTER(807, 0x1d7, s.v[3])
		NT_BURN(1);
		NT_AT(808); s.v[3] = (uint16) (~Rd(1) + 1);
		NT_BURN(7);
		RASTER(809, 0x287, s.v[3])
		NT_BURN(1);
		NT_AT(810); s.v[3] = Rd(2);
		NT_BURN(4);
		RASTER(811, 0x337, s.v[3])
		NT_BURN(1);
	}
done:

	// done: the command word cleared, back to the wait
	NT_BURN(2);
	NT_AT(20); Wr(0x10, 0);
	NT_BURN(1);
	s.line = 4;
	return (true);
	}
	return (true);
}

void Attach (uint16 *d, uint16 *st, uint8 *r)
{
	dr = d;
	sr = st;
	ram = r;
}

void Reset (void)
{
	memset(&s, 0, sizeof(s));
	executed = 0;
}

void Idle (void)
{
	memset(&s, 0, sizeof(s));
	s.line = 4;
}

// its command wait polls the RAM, so every sync runs it
uint64 Run (uint64 n)
{
	budget = n;
	while (budget)
		Main();
	return (1);
}

uint32 StateSize (void)
{
	return (sizeof(s) + sizeof(executed));
}

void StateSave (uint8 *buf)
{
	memcpy(buf, &s, sizeof(s));
	memcpy(buf + sizeof(s), &executed, sizeof(executed));
}

bool8 StateLoad (const uint8 *buf, uint32 size)
{
	if (size != sizeof(s) + sizeof(executed))
		return (FALSE);
	memcpy(&s, buf, sizeof(s));
	memcpy(&executed, buf + sizeof(s), sizeof(executed));
	return (TRUE);
}

#ifdef UPD96050_LAB
void LabEvents (void (*fn) (char kind, uint64 at, uint32 word, uint16 v))
{
	lab_event = fn;
}
#endif

} // namespace

const S9xUPD96050Native	S9xST010Native =
{
	Attach, Reset, Idle, Run, StateSize, StateSave, StateLoad,
#ifdef UPD96050_LAB
	LabEvents,
#endif
};
