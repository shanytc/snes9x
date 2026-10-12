/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The native ST011's evaluation, commands $03 and $0F (the CPU's search calls them at its
// leaves): a chain of passes over the position, each leaving maps and scores in the RAM.
// Included once, inside st011n.cpp's unnamed namespace.

// LD A; LD B; LD TR, then INC A|A->DP; DEC B|TR->MEM; JNZB for each word: n words from `base` get `v`
inline void Fill (uint32 base, uint32 n, uint16 v)
{
	c += 3;
	for (uint32 w = base; w < base + n; w++)
	{
		c += 2; WrW(w, v);
		c += 1;
	}
	s.tr = v;
}

// 00F8 ($0F only): the position turned round: the board mirrored through its centre with the colours swapped ($60),
// the hands swapped, the kings' squares mirrored
void Flip (void)
{
	c += 1 + 2;
	s.rp = 0x6d;
	for (s.a = 0x15; s.a != 0x41; )
	{
		c += 4;
		uint16	x = RdW(s.a);
		if (!(x & 0x8000))
		{
			c += 1;
			if (x) { c += 2; x ^= 0x60; }
			s.tr = x;
			c += 5;
			uint16	y = RdW(s.rp);
			if (y) { c += 2; y ^= 0x60; }
			c += 1; WrW(s.rp, s.tr);
			c += 2; WrW(s.a, y);
			s.b = y;
		}
		c += 4;
		s.a++;
		s.rp--;
	}
	c += 4;
	s.b = RdW(0x41);
	if (s.b)
	{
		c += 2;
		s.b ^= 0x60;
		c += 1; WrW(0x41, s.b);
	}
	c += 3;
	s.a = 0x89; s.rp = 0x91;
	for (s.b = 7; s.b; s.b--)
	{
		s.k = RdW(s.a);
		s.l = RdW(s.rp);
		c += 5; WrW(s.rp, s.k);
		c += 2; WrW(s.a, s.l);
		c += 1;
		s.a--; s.rp--;
	}
	s.a = (uint16) (0x82 - RdW(0x93));
	s.b = (uint16) (0x82 - RdW(0x94));
	c += 7; WrW(0x94, s.a);
	c += 2; WrW(0x93, s.b);
}

// The king-safety terms of 017E: each square round a king has a value (its distance from the board's edge, $6C2 +
// square); the squares a king covers take the data ROM's value for it (tables $184, $16A, $177 by ring), and the
// sum of the changes, times 25 for the outer ring and 40 for the inner ones, goes into word $98 (black's negated).
inline void RingSquare (int off, uint16 tbl)
{
	const uint32	w = (uint32) (0x6c2 + s.trb + off);
	c += 7;
	const uint16	old = RdW(w);
	s.tr = old;
	s.a = Rom((uint32) (tbl + old));
	c += 1; WrW(w, s.a);
	c += 3;
	s.k = (uint16) (s.a - old + s.k);
}

// K times L, as the multiplier's low word N, then back down a bit (ADD N; SHR1)
inline uint16 Scaled (void)
{
	const uint16	n = (uint16) ((int32) (int16) s.k * (int16) s.l << 1);
	return ((uint16) ((n >> 1) | (n & 0x8000)));
}

void KingRings (void)
{
	static const uint8	kEdge[10] = { 0, 2, 4, 5, 5, 5, 5, 5, 4, 2 }, kInner[10] = { 0, 3, 5, 6, 6, 6, 6, 6, 5, 3 };
	c += 1 + 1;
	s.a = 0x6d6;
	for (int row = 0; row < 9; row++)
	{
		const uint8	*v = (row == 0 || row == 8) ? kEdge : kInner;
		c += 1;
		for (int i = 0; i < 10; i++)
		{
			c += 2;
			WrW(s.a, v[i]);
			s.a++;
		}
		c += 1;
	}
	Fill(0x6c2, 21, 0);
	Fill(0x730, 21, 0);

	// black's king: the ring two ranks out, then the near ones
	c += 3;
	s.trb = RdW(0x93);
	s.k = 0;
	RingSquare(-21, 0x184); RingSquare(-20, 0x184); RingSquare(-19, 0x184);
	s.l = 0x19;
	c += 7;
	s.a = (uint16) -Scaled();
	c += 1; WrW(0x98, s.a);
	c += 1;
	s.k = 0;
	RingSquare(-11, 0x16a); RingSquare(-10, 0x16a); RingSquare(-9, 0x16a);
	RingSquare(-1, 0x177); RingSquare(1, 0x177); RingSquare(9, 0x177); RingSquare(10, 0x177); RingSquare(11, 0x177);
	s.l = 0x28;
	c += 8;
	s.a = (uint16) (-Scaled() + RdW(0x98));
	c += 1; WrW(0x98, s.a);

	// white's
	c += 3;
	s.trb = RdW(0x94);
	s.k = 0;
	RingSquare(21, 0x184); RingSquare(20, 0x184); RingSquare(19, 0x184);
	s.l = 0x19;
	c += 6;
	s.a = (uint16) (Scaled() + RdW(0x98));
	c += 1; WrW(0x98, s.a);
	c += 1;
	s.k = 0;
	RingSquare(11, 0x16a); RingSquare(10, 0x16a); RingSquare(9, 0x16a);
	RingSquare(1, 0x177); RingSquare(-1, 0x177); RingSquare(-9, 0x177); RingSquare(-10, 0x177); RingSquare(-11, 0x177);
	s.l = 0x28;
	c += 6;
	s.a = (uint16) (Scaled() + RdW(0x98));
	c += 1; WrW(0x98, s.a);
	c += 1;
}

// 038E / 03EF: from a king along `dir`: if the first piece is the king's own (`own`) and the next one is an enemy
// ranging piece `slider` (low two bits aside), the pinner's word in $1A4 and the pinned piece's in $9E get `l`
void PinLine (uint16 kingw, uint16 dir, uint16 own, uint16 slider, uint16 l)
{
	s.tr = dir; s.trb = slider; s.l = l;
	c += 3 + 1 + 3;
	s.a = RdW(kingw);
	do { c += 4; s.a = (uint16) (s.a + dir); } while (!RdW(s.a));
	c += 3;
	if (!(RdW(s.a) & own)) { c += 1; return; }
	c += 2;
	s.k = s.a;
	do { c += 4; s.a = (uint16) (s.a + dir); } while (!RdW(s.a));
	c += 4;
	if ((RdW(s.a) & 0xfc) != slider) { c += 1; return; }
	c += 4; WrW(0x1a4 + s.a, l);
	c += 4; WrW(0x9e + s.k, l);
	c += 1;
}

// 03AB / 040C: the same along the king's file towards the enemy's lance (`lance`), mark $40
void PinFile (uint16 kingw, uint16 dir, uint16 own, uint16 lance)
{
	s.tr = dir; s.trb = 0x38;
	c += 1 + 5;
	s.a = RdW(kingw);
	do { c += 4; s.a = (uint16) (s.a + dir); } while (!RdW(s.a));
	c += 3;
	if (!(RdW(s.a) & own)) { c += 1; return; }
	c += 2;
	s.k = s.a;
	do { c += 4; s.a = (uint16) (s.a + dir); } while (!RdW(s.a));
	c += 3;
	const uint16	v = RdW(s.a);
	if (v != lance)
	{
		c += 4;
		if ((v & 0xfc) != 0x38) { c += 1; return; }
	}
	c += 4; WrW(0x1a4 + s.a, 0x40);
	c += 4; WrW(0x9e + s.k, 0x40);
	c += 1;
}

// 0360 (black's king, white's pinners) and 03CD (white's king, black's): the pin marks, $20 for the 11 diagonal,
// $60 the 9 one, $80 the rank, $40 the file
void Pinners (bool white)
{
	const uint16	kingw = white ? 0x94 : 0x93, own = white ? 0x20 : 0x40;
	const uint16	bishop = white ? 0x54 : 0x34, rook = white ? 0x58 : 0x38;
	const int		sgn = white ? 1 : -1;
	c += 1;
	if (!white)
	{
		Fill(0x1a4, 0x83, 0);
		Fill(0x9e, 0x83, 0);
	}
	c += 4;
	if (!RdW(kingw)) { c += 1; return; }
	PinLine(kingw, (uint16) (11 * sgn), own, bishop, 0x20);
	PinFile(kingw, (uint16) (10 * sgn), own, white ? 0x44 : 0x24);
	PinLine(kingw, (uint16) (9 * sgn), own, bishop, 0x60);
	PinLine(kingw, (uint16) (1 * sgn), own, rook, 0x80);
	PinLine(kingw, (uint16) (-1 * sgn), own, rook, 0x80);
	PinLine(kingw, (uint16) (-9 * sgn), own, bishop, 0x60);
	PinLine(kingw, (uint16) (-10 * sgn), own, rook, 0x40);
	PinLine(kingw, (uint16) (-11 * sgn), own, bishop, 0x20);
	c += 1;
}

// ---- 042E: both sides' attacks: word $121 + square gets $80 for black, $8000 for white; $4B6 (black) or $433
// (white) + square ORs in the attacker's pinner bit (data ROM $1B1 by its mark in $1A4: 0, 1, 2, 4, 8)

enum { A_MARK = 1, A_RAY };
struct AOp { uint8 op; int8 arg; };

// one attacked square, A + off
inline void AttackMark (int off, bool white)
{
	const uint32	sq = (uint32) (s.a + off);
	c += 6; WrW(0x121 + sq, RdW(0x121 + sq) | (white ? 0x8000 : 0x0080));
	c += 6; WrW((white ? 0x433 : 0x4b6) + sq, RdW((white ? 0x433 : 0x4b6) + sq) | s.k);
}

// 0462 / 0484: a ray up to and including the first piece, one square past the enemy king
void AttackRay (uint16 dir, bool white)
{
	s.tr = dir;
	c += 2;
	for (;;)
	{
		c += 1;
		s.a = (uint16) (s.a + dir);
		AttackMark(0, white);
		c += 4;
		if (RdW(s.a))
			break;
	}
	c += 3;
	if (RdW(s.a) == (white ? 0x5c : 0x3c))
	{
		c += 1;
		s.a = (uint16) (s.a + dir);
		AttackMark(0, white);
	}
	c += 1;
	s.a = s.trb;
}

const AOp	abPawn[] = { { A_MARK, -10 }, { 0, 0 } };
const AOp	abGold[] = { { A_MARK, -11 }, { A_MARK, -10 }, { A_MARK, -9 }, { A_MARK, -1 }, { A_MARK, 1 }, { A_MARK, 10 }, { 0, 0 } };
const AOp	abLance[] = { { A_RAY, -10 }, { 0, 0 } };
const AOp	abKnight[] = { { A_MARK, -21 }, { A_MARK, -19 }, { 0, 0 } };
const AOp	abSilver[] = { { A_MARK, -11 }, { A_MARK, -10 }, { A_MARK, -9 }, { A_MARK, 9 }, { A_MARK, 11 }, { 0, 0 } };
const AOp	abBishop[] = { { A_RAY, -11 }, { A_RAY, -9 }, { A_RAY, 9 }, { A_RAY, 11 }, { 0, 0 } };
const AOp	abHorse[] = { { A_MARK, -10 }, { A_MARK, -1 }, { A_MARK, 1 }, { A_MARK, 10 },
						  { A_RAY, -11 }, { A_RAY, -9 }, { A_RAY, 9 }, { A_RAY, 11 }, { 0, 0 } };
const AOp	abRook[] = { { A_RAY, -10 }, { A_RAY, -1 }, { A_RAY, 1 }, { A_RAY, 10 }, { 0, 0 } };
const AOp	abDragon[] = { { A_MARK, -11 }, { A_MARK, -9 }, { A_MARK, 9 }, { A_MARK, 11 },
						   { A_RAY, -10 }, { A_RAY, -1 }, { A_RAY, 1 }, { A_RAY, 10 }, { 0, 0 } };
const AOp	abKing[] = { { A_MARK, -11 }, { A_MARK, -10 }, { A_MARK, -9 }, { A_MARK, -1 }, { A_MARK, 1 }, { A_MARK, 9 },
						 { A_MARK, 10 }, { A_MARK, 11 }, { 0, 0 } };
const AOp	awPawn[] = { { A_MARK, 10 }, { 0, 0 } };
const AOp	awGold[] = { { A_MARK, 11 }, { A_MARK, 10 }, { A_MARK, 9 }, { A_MARK, 1 }, { A_MARK, -1 }, { A_MARK, -10 }, { 0, 0 } };
const AOp	awLance[] = { { A_RAY, 10 }, { 0, 0 } };
const AOp	awKnight[] = { { A_MARK, 21 }, { A_MARK, 19 }, { 0, 0 } };
const AOp	awSilver[] = { { A_MARK, 11 }, { A_MARK, 10 }, { A_MARK, 9 }, { A_MARK, -9 }, { A_MARK, -11 }, { 0, 0 } };
const AOp	awBishop[] = { { A_RAY, 11 }, { A_RAY, 9 }, { A_RAY, -9 }, { A_RAY, -11 }, { 0, 0 } };
const AOp	awHorse[] = { { A_MARK, 10 }, { A_MARK, 1 }, { A_MARK, -1 }, { A_MARK, -10 },
						  { A_RAY, 11 }, { A_RAY, 9 }, { A_RAY, -9 }, { A_RAY, -11 }, { 0, 0 } };
const AOp	awRook[] = { { A_RAY, 10 }, { A_RAY, 1 }, { A_RAY, -1 }, { A_RAY, -10 }, { 0, 0 } };
const AOp	awDragon[] = { { A_MARK, 11 }, { A_MARK, 9 }, { A_MARK, -9 }, { A_MARK, -11 },
						   { A_RAY, 10 }, { A_RAY, 1 }, { A_RAY, -1 }, { A_RAY, -10 }, { 0, 0 } };
const AOp	awKing[] = { { A_MARK, 11 }, { A_MARK, 10 }, { A_MARK, 9 }, { A_MARK, 1 }, { A_MARK, -1 }, { A_MARK, -9 },
						 { A_MARK, -10 }, { A_MARK, -11 }, { 0, 0 } };
const AOp	aToWait[] = { { 0, 0 } };

const AOp	*const kAttack[32] =	// data ROM $191: black's 16 types, then white's
{
	abPawn, abGold, abLance, abGold, abKnight, abGold, abSilver, abGold, abGold, aToWait, abBishop, abHorse, abRook, abDragon, abKing, aToWait,
	awPawn, awGold, awLance, awGold, awKnight, awGold, awSilver, awGold, awGold, aToWait, awBishop, awHorse, awRook, awDragon, awKing, aToWait
};

// FALSE: the chip went back to its command wait
bool AttackMaps (void)
{
	c += 1;
	Fill(0x121, 0x83, 0);
	Fill(0x433, 0x83, 0);
	Fill(0x4b6, 0x83, 0);
	c += 1;
	for (s.a = 0x15; s.a != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.a);
		if (piece && !(c += 1, piece & 0x8000))
		{
			c += 14 + 9;
			s.trb = s.a;
			const uint16	pin = RdW(0x1a4 + s.a);
			s.k = Rom(0x1b1 + (uint16) (((int16) pin) >> 5));
			const int		idx = (piece & 0x3e) >> 1;
			s.tr = 0x191;
			const AOp		*p = kAttack[idx];
			if (p == aToWait)
				return (false);
			const bool		white = idx >= 16;
			for (; p->op; p++)
				if (p->op == A_MARK)
					AttackMark(p->arg, white);
				else
					AttackRay((uint16) (int16) p->arg, white);
			c += 1;
		}
		c += 4;
		s.a++;
	}
	c += 1;
	return (true);
}

// 07AA: each piece's value (data ROM $1B6 by type) in word $63F + square
void PieceValues (void)
{
	c += 1;
	Fill(0x63f, 0x83, 0);
	c += 1;
	for (s.a = 0x15; s.a != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.a);
		if (piece && !(c += 1, piece & 0x8000))
		{
			s.tr = 0x1b6;
			c += 10; WrW(0x63f + s.a, Rom(0x1b6 + ((piece & 0x1e) >> 1)));
		}
		c += 4;
		s.a++;
	}
	c += 1;
}

// 07C5: each side's pieces as a list, most valuable first: heads in words $745 (black) and $746 (white), each
// square's next in $745 + square
void ValueLists (void)
{
	c += 1;
	c += 2; WrW(0x745, 0);
	c += 2; WrW(0x746, 0);
	c += 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.trb);
		if (piece && !(c += 1, piece & 0x8000))
		{
			c += 4;
			s.k = (piece & 0x40) ? 0 : 1;
			if (s.k)
				c += 1;
			const uint16	v = RdW(0x63f + s.trb);
			for (;;)
			{
				c += 14;
				s.l = s.k;
				s.k = RdW(0x745 + s.l);
				if (v >= RdW(0x63f + s.k))
					break;
			}
			c += 4; WrW(0x745 + s.trb, s.k);
			c += 4; WrW(0x745 + s.l, s.trb);
		}
		c += 6;
		s.trb++;
	}
	c += 1;
}

// ---- 07F1 (black) and 0A6B (white): each side's pieces, most valuable first, mark the squares they cover: the
// side's map ($1A4 black, $227 white, both from $7878) shifts its low byte up and takes the piece's value (so it ends
// with the two cheapest attackers), and the count map $539 adds 1 (black) or $100 (white). A pinned piece counts
// the squares off its line only where the pinner's own attacks reach (the other side's map $433 / $4B6 ANDs L).
// Ranging pieces x-ray on through pieces of theirs that move the same way (data ROM $216-$276), those squares
// counting only.

enum { S_CALLMARK = 1, S_COND, S_MARK, S_COUNT, S_XRAY };
struct SOp { uint8 op; int8 off; uint16 tab; };
const SOp	kSupToWait[] = { { 0, 0, 0 } };

const SOp	kSupB092B[] = { { S_CALLMARK, -10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0954[] = { { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_CALLMARK, -1, 0x000 }, { S_CALLMARK, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB092D[] = { { S_XRAY, -10, 0x216 }, { 0, 0, 0 } };
const SOp	kSupB0931[] = { { S_MARK, -21, 0x000 }, { S_MARK, -19, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB094E[] = { { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_CALLMARK, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB095F[] = { { S_XRAY, -11, 0x246 }, { S_XRAY, 11, 0x256 }, { S_XRAY, -9, 0x246 }, { S_XRAY, 9, 0x256 }, { 0, 0, 0 } };
const SOp	kSupB095B[] = { { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -1, 0x000 }, { S_CALLMARK, 1, 0x000 }, { S_XRAY, -11, 0x246 }, { S_XRAY, 11, 0x256 }, { S_XRAY, -9, 0x246 }, { S_XRAY, 9, 0x256 }, { 0, 0, 0 } };
const SOp	kSupB0970[] = { { S_XRAY, -10, 0x216 }, { S_XRAY, 10, 0x226 }, { S_XRAY, -1, 0x276 }, { S_XRAY, 1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupB096C[] = { { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_XRAY, -10, 0x216 }, { S_XRAY, 10, 0x226 }, { S_XRAY, -1, 0x276 }, { S_XRAY, 1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupB097D[] = { { S_COUNT, -11, 0x000 }, { S_COUNT, -10, 0x000 }, { S_COUNT, -9, 0x000 }, { S_COUNT, -1, 0x000 }, { S_COUNT, 1, 0x000 }, { S_COUNT, 9, 0x000 }, { S_COUNT, 10, 0x000 }, { S_COUNT, 11, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09AE[] = { { S_COND, -10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09C9[] = { { S_CALLMARK, -11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09B0[] = { { S_COND, -10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09B2[] = { { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09E5[] = { { S_XRAY, -11, 0x246 }, { S_XRAY, 11, 0x256 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09FC[] = { { S_XRAY, -11, 0x246 }, { S_XRAY, 11, 0x256 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A28[] = { { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A3F[] = { { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09D0[] = { { S_COND, -11, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09B8[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09EE[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A09[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A2D[] = { { S_XRAY, -10, 0x216 }, { S_XRAY, 10, 0x226 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A48[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_XRAY, -10, 0x216 }, { S_XRAY, 10, 0x226 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09D7[] = { { S_COND, -11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09BE[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_CALLMARK, 9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09F3[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_XRAY, -9, 0x246 }, { S_XRAY, 9, 0x256 }, { 0, 0, 0 } };
const SOp	kSupB0A12[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_XRAY, -9, 0x246 }, { S_XRAY, 9, 0x256 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A55[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09DE[] = { { S_COND, -11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_CALLMARK, -1, 0x000 }, { S_CALLMARK, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB09C4[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_CALLMARK, -11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, -1, 0x000 }, { S_COND, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A1F[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_CALLMARK, -1, 0x000 }, { S_CALLMARK, 1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupB0A36[] = { { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_XRAY, -1, 0x276 }, { S_XRAY, 1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupB0A5E[] = { { S_COND, -11, 0x000 }, { S_COND, 11, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 9, 0x000 }, { S_XRAY, -1, 0x276 }, { S_XRAY, 1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupW0B9F[] = { { S_CALLMARK, 10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0BC8[] = { { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_CALLMARK, 1, 0x000 }, { S_CALLMARK, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0BA1[] = { { S_XRAY, 10, 0x226 }, { 0, 0, 0 } };
const SOp	kSupW0BA5[] = { { S_MARK, 21, 0x000 }, { S_MARK, 19, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0BC2[] = { { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_CALLMARK, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0BD3[] = { { S_XRAY, 11, 0x256 }, { S_XRAY, -11, 0x246 }, { S_XRAY, 9, 0x256 }, { S_XRAY, -9, 0x246 }, { 0, 0, 0 } };
const SOp	kSupW0BCF[] = { { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_CALLMARK, 1, 0x000 }, { S_CALLMARK, -1, 0x000 }, { S_XRAY, 11, 0x256 }, { S_XRAY, -11, 0x246 }, { S_XRAY, 9, 0x256 }, { S_XRAY, -9, 0x246 }, { 0, 0, 0 } };
const SOp	kSupW0BE4[] = { { S_XRAY, 10, 0x226 }, { S_XRAY, -10, 0x216 }, { S_XRAY, 1, 0x276 }, { S_XRAY, -1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupW0BE0[] = { { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -11, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_XRAY, 10, 0x226 }, { S_XRAY, -10, 0x216 }, { S_XRAY, 1, 0x276 }, { S_XRAY, -1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupW0BF1[] = { { S_COUNT, 11, 0x000 }, { S_COUNT, 10, 0x000 }, { S_COUNT, 9, 0x000 }, { S_COUNT, 1, 0x000 }, { S_COUNT, -1, 0x000 }, { S_COUNT, -9, 0x000 }, { S_COUNT, -10, 0x000 }, { S_COUNT, -11, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C22[] = { { S_COND, 10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C3D[] = { { S_CALLMARK, 11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C24[] = { { S_COND, 10, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C26[] = { { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C59[] = { { S_XRAY, 11, 0x256 }, { S_XRAY, -11, 0x246 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C70[] = { { S_XRAY, 11, 0x256 }, { S_XRAY, -11, 0x246 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C9C[] = { { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0CB3[] = { { S_CALLMARK, 11, 0x000 }, { S_CALLMARK, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C44[] = { { S_COND, 11, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C2C[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C62[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C7D[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_CALLMARK, 10, 0x000 }, { S_CALLMARK, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0CA1[] = { { S_XRAY, 10, 0x226 }, { S_XRAY, -10, 0x216 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0CBC[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_XRAY, 10, 0x226 }, { S_XRAY, -10, 0x216 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C4B[] = { { S_COND, 11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C32[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_CALLMARK, -9, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C67[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_XRAY, 9, 0x256 }, { S_XRAY, -9, 0x246 }, { 0, 0, 0 } };
const SOp	kSupW0C86[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_XRAY, 9, 0x256 }, { S_XRAY, -9, 0x246 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0CC9[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_CALLMARK, 9, 0x000 }, { S_CALLMARK, -9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C52[] = { { S_COND, 11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_CALLMARK, 1, 0x000 }, { S_CALLMARK, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C38[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_CALLMARK, 11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, 1, 0x000 }, { S_COND, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0C93[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_CALLMARK, 1, 0x000 }, { S_CALLMARK, -1, 0x000 }, { 0, 0, 0 } };
const SOp	kSupW0CAA[] = { { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_XRAY, 1, 0x276 }, { S_XRAY, -1, 0x276 }, { 0, 0, 0 } };
const SOp	kSupW0CD2[] = { { S_COND, 11, 0x000 }, { S_COND, -11, 0x000 }, { S_COND, 10, 0x000 }, { S_COND, -10, 0x000 }, { S_COND, 9, 0x000 }, { S_COND, -9, 0x000 }, { S_XRAY, 1, 0x276 }, { S_XRAY, -1, 0x276 }, { 0, 0, 0 } };

const SOp	*const kSupport[2][80] =	// data ROM $1C6 (black) and $29B (white): pin mark / 2 + type
{
	{
		kSupB092B, kSupB0954, kSupB092D, kSupB0954, kSupB0931, kSupB0954, kSupB094E, kSupB0954, kSupB0954, kSupToWait, kSupB095F, kSupB095B, kSupB0970, kSupB096C, kSupB097D, kSupToWait,
		kSupB09AE, kSupB09C9, kSupB09B0, kSupB09C9, NULL, kSupB09C9, kSupB09B2, kSupB09C9, kSupB09C9, kSupToWait, kSupB09E5, kSupB09FC, kSupB0A28, kSupB0A3F, NULL, kSupToWait,
		kSupB092B, kSupB09D0, kSupB092D, kSupB09D0, NULL, kSupB09D0, kSupB09B8, kSupB09D0, kSupB09D0, kSupToWait, kSupB09EE, kSupB0A09, kSupB0A2D, kSupB0A48, NULL, kSupToWait,
		kSupB09AE, kSupB09D7, kSupB09B0, kSupB09D7, NULL, kSupB09D7, kSupB09BE, kSupB09D7, kSupB09D7, kSupToWait, kSupB09F3, kSupB0A12, kSupB0A28, kSupB0A55, NULL, kSupToWait,
		kSupB09AE, kSupB09DE, kSupB09B0, kSupB09DE, NULL, kSupB09DE, kSupB09C4, kSupB09DE, kSupB09DE, kSupToWait, kSupB09EE, kSupB0A1F, kSupB0A36, kSupB0A5E, NULL, kSupToWait,
	},
	{
		kSupW0B9F, kSupW0BC8, kSupW0BA1, kSupW0BC8, kSupW0BA5, kSupW0BC8, kSupW0BC2, kSupW0BC8, kSupW0BC8, kSupToWait, kSupW0BD3, kSupW0BCF, kSupW0BE4, kSupW0BE0, kSupW0BF1, kSupToWait,
		kSupW0C22, kSupW0C3D, kSupW0C24, kSupW0C3D, NULL, kSupW0C3D, kSupW0C26, kSupW0C3D, kSupW0C3D, kSupToWait, kSupW0C59, kSupW0C70, kSupW0C9C, kSupW0CB3, NULL, kSupToWait,
		kSupW0B9F, kSupW0C44, kSupW0BA1, kSupW0C44, NULL, kSupW0C44, kSupW0C2C, kSupW0C44, kSupW0C44, kSupToWait, kSupW0C62, kSupW0C7D, kSupW0CA1, kSupW0CBC, NULL, kSupToWait,
		kSupW0C22, kSupW0C4B, kSupW0C24, kSupW0C4B, NULL, kSupW0C4B, kSupW0C32, kSupW0C4B, kSupW0C4B, kSupToWait, kSupW0C67, kSupW0C86, kSupW0C9C, kSupW0CC9, NULL, kSupToWait,
		kSupW0C22, kSupW0C52, kSupW0C24, kSupW0C52, NULL, kSupW0C52, kSupW0C38, kSupW0C52, kSupW0C52, kSupToWait, kSupW0C62, kSupW0C93, kSupW0CAA, kSupW0CD2, NULL, kSupToWait,
	},
};

// the square A + off: its two cheapest attackers and its count (14 words; the count's write closes them)
inline void SupMark (int off, bool white)
{
	const uint32	m = (uint32) ((white ? 0x227 : 0x1a4) + s.a + off), n = (uint32) (0x539 + s.a + off);
	c += 8; WrW(m, (uint16) (((RdW(m) & 0xff) << 8) | s.k));
	c += 6; WrW(n, (uint16) (RdW(n) + (white ? 0x100 : 1)));
}

// the count alone (6 words)
inline void SupCount (int off, bool white)
{
	const uint32	n = (uint32) (0x539 + s.a + off);
	c += 6; WrW(n, (uint16) (RdW(n) + (white ? 0x100 : 1)));
}

// 0821 / 0A95: an x-ray along DR (the table that lets it through pieces in TR); A back from TRB at the end
void SupRay (bool white)
{
	do
	{
		c += 1;
		s.a = (uint16) (s.a + FromDR());
		SupMark(0, white);
		c += 4;
	} while (!RdW(s.a));
	c += 1;
	if (!(RdW(s.a) & 0x8000))
	{
		c += 1;
		for (;;)
		{
			// the piece reached: the table says on through it (2), one square past it (1), or stop (0)
			c += 8;
			const uint16	v = (uint16) (Rom((uint32) (s.tr + ((RdW(s.a) & 0x3e) >> 1))) - 1);
			s.b = v;
			if (v & 0x8000)
				break;
			c += 1;
			if (!v)
			{
				c += 1;
				s.a = (uint16) (s.a + FromDR());
				SupCount(0, white);
				break;
			}
			do
			{
				c += 1;
				s.a = (uint16) (s.a + FromDR());
				SupCount(0, white);
				c += 4;
			} while (!RdW(s.a));
			c += 1;
			if (RdW(s.a) & 0x8000)
				break;
		}
	}
	c += 1;
	s.a = s.trb;
}

// FALSE: the chip went back to its command wait
bool Support (bool white)
{
	c += 1;
	Fill(white ? 0x227 : 0x1a4, 0x83, 0x7878);
	if (!white)
		Fill(0x539, 0x83, 0);
	c += 2;
	s.a = RdW(white ? 0x746 : 0x745);
	do
	{
		c += 28;
		s.k = RdW(0x63f + s.a);
		const uint16	pin = RdW(0x9e + s.a);
		s.l = Rom((uint32) ((white ? 0x2eb : 0x296) + (uint16) ((int16) pin >> 5)));
		s.trb = s.a;
		const uint32	idx = (uint32) (((RdW(s.a) & 0x1e) | pin) >> 1);
		s.tr = white ? 0x29b : 0x1c6;
		const SOp		*p = idx < 80 ? kSupport[white][idx] : kSupToWait;
		if (idx >= 80)
			s.unknown = 1;
		if (p == kSupToWait)
			return (false);
		if (p)
		{
			for (; p->op; p++)
				switch (p->op)
				{
					case S_CALLMARK:
						c += 1;
						SupMark(p->off, white);
						break;
					case S_COND:
					{
						const uint32	w = (uint32) ((white ? 0x4b6 : 0x433) + s.a + p->off);
						c += 1 + 6;
						if (RdW(w) & s.l)
							SupCount(p->off, white);
						else
							c += 1;
						break;
					}
					case S_MARK:
						SupMark(p->off, white);
						break;
					case S_COUNT:
						SupCount(p->off, white);
						break;
					case S_XRAY:
						c += 1; ToDR((uint16) (int16) p->off);
						s.tr = p->tab;
						c += 2;
						SupRay(white);
						break;
				}
			c += 1;
		}
		c += 5;
		s.a = RdW(0x745 + s.a);
	} while (s.a);
	c += 1;
	return (true);
}

// ---- 0CDF: each square's exchange: black's and white's cheapest attackers (K, L) and their second ones ($7D2,
// $7D3), against the attack counts (doubled, plus one for the side whose piece stands there). The side with more
// keeps its cheapest less the other's second (when the other has 2 or more); level, both, kept only from 3 up.
// Word $3B0 + square = L << 8 | K. DR carries the counts on the way.

// a count doubled, plus one when the piece on the square has `bit` (the colour bits come out of SHL1's carry)
inline uint16 Weighted (uint16 count, uint16 piece, int bit) { return ((uint16) ((count << 1) | ((piece >> bit) & 1))); }

inline uint16 Swap (uint16 v) { return ((uint16) ((v << 8) | (v >> 8))); }

void Exchange (void)
{
	c += 1 + 1;
	for (s.a = 0x15; s.a != 0x6e; )
	{
		c += 4;
		if (!(RdW(s.a) & 0x8000))
		{
			s.trb = s.a;
			const uint16	bm = RdW(0x1a4 + s.a);
			s.k = bm & 0xff;
			c += 10; WrW(0x7d2, (uint16) (bm >> 8));
			const uint16	wm = RdW(0x227 + s.a);
			s.l = wm & 0xff;
			c += 9; WrW(0x7d3, (uint16) (wm >> 8));
			const uint16	cnt = RdW(0x539 + s.a);
			c += 6; ToDR(cnt & 0xff);
			s.tr = (uint16) (cnt >> 8);
			const uint16	piece = RdW(s.a);
			c += 7; FromDR();
			const uint16	bw = Weighted(cnt & 0xff, Swap(piece), 14), ww = Weighted(s.tr, Swap(piece), 13);
			c += 2; ToDR(bw);
			s.tr = ww;
			c += 4; FromDR();
			c += 2;
			if (bw < ww)
			{
				c += 2; FromDR();
				c += 1;
				if (bw < 3)
				{
					c += 2;
					s.l = 0;
				}
				else
				{
					c += 5;
					const uint16	two = RdW(0x7d2);
					if (s.l < two) { c += 2; s.l = 0; }
					else s.l = (uint16) (s.l - two);
				}
			}
			else if (bw == ww)
			{
				c += 1;
				c += 5;
				const uint16	w2 = RdW(0x7d3), b2 = RdW(0x7d2);
				if (s.k < w2) { c += 1; s.k = 0; } else s.k = (uint16) (s.k - w2);
				c += 5;
				if (s.l < b2) { c += 1; s.l = 0; } else s.l = (uint16) (s.l - b2);
				c += 3;
				if (s.k < 3) { c += 2; s.k = 0; s.l = 0; }
			}
			else
			{
				c += 1 + 3;
				if (ww < 3)
				{
					c += 2;
					s.k = 0;
				}
				else
				{
					c += 5;
					const uint16	two = RdW(0x7d3);
					if (s.k < two) { c += 2; s.k = 0; }
					else s.k = (uint16) (s.k - two);
				}
			}
			c += 8; WrW(0x3b0 + s.a, (uint16) (Swap(s.l) | s.k));
		}
		c += 4;
		s.a++;
	}
	c += 1;
}

// ---- 0D3F (black) and 0F19 (white): each piece, from the most valuable, sums the king-ring values ($6C2) of the
// squares it reaches, plus 2 for each target whose exchange there doesn't lose it the piece (the other side's part
// of $3B0, plus the target's value, not under its own); a pinned piece only counts the ring values off its line.
// Word $2AA + square gets the sum.

enum { M_STEP = 1, M_STEPR, M_RAY, M_RAYR };
struct MOp { int8 op; int8 off; };
const MOp	kMobToWait[] = { { 0, 0 } };

const MOp	kMobB0DE0[] = { { M_STEP, -10 }, { 0, 0 } };
const MOp	kMobB0DF6[] = { { M_STEP, -11 }, { M_STEP, -10 }, { M_STEP, -9 }, { M_STEP, -1 }, { M_STEP, 1 }, { M_STEP, 10 }, { 0, 0 } };
const MOp	kMobB0DE3[] = { { M_RAY, -10 }, { 0, 0 } };
const MOp	kMobB0DE6[] = { { M_STEP, -21 }, { M_STEP, -19 }, { 0, 0 } };
const MOp	kMobB0DEB[] = { { M_STEP, -11 }, { M_STEP, -10 }, { M_STEP, -9 }, { M_STEP, 9 }, { M_STEP, 11 }, { 0, 0 } };
const MOp	kMobB0E0B[] = { { M_RAY, -11 }, { M_RAY, -9 }, { M_RAY, 9 }, { M_RAY, 11 }, { 0, 0 } };
const MOp	kMobB0E03[] = { { M_STEP, -10 }, { M_STEP, -1 }, { M_STEP, 1 }, { M_STEP, 10 }, { M_RAY, -11 }, { M_RAY, -9 }, { M_RAY, 9 }, { M_RAY, 11 }, { 0, 0 } };
const MOp	kMobB0E1C[] = { { M_RAY, -10 }, { M_RAY, -1 }, { M_RAY, 1 }, { M_RAY, 10 }, { 0, 0 } };
const MOp	kMobB0E14[] = { { M_STEP, -11 }, { M_STEP, -9 }, { M_STEP, 9 }, { M_STEP, 11 }, { M_RAY, -10 }, { M_RAY, -1 }, { M_RAY, 1 }, { M_RAY, 10 }, { 0, 0 } };
const MOp	kMobB0E25[] = { { 0, 0 } };
const MOp	kMobB0E26[] = { { M_STEPR, -10 }, { 0, 0 } };
const MOp	kMobB0E5D[] = { { M_STEP, -11 }, { M_STEPR, -10 }, { M_STEPR, -9 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEPR, 10 }, { 0, 0 } };
const MOp	kMobB0E29[] = { { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobB0E2C[] = { { M_STEPR, -21 }, { M_STEPR, -19 }, { 0, 0 } };
const MOp	kMobB0E31[] = { { M_STEP, -11 }, { M_STEPR, -10 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEP, 11 }, { 0, 0 } };
const MOp	kMobB0E99[] = { { M_RAY, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAY, 11 }, { 0, 0 } };
const MOp	kMobB0E91[] = { { M_STEPR, -10 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEPR, 10 }, { M_RAY, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAY, 11 }, { 0, 0 } };
const MOp	kMobB0EDD[] = { { M_RAYR, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobB0ED5[] = { { M_STEP, -11 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEP, 11 }, { M_RAYR, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobB0E6A[] = { { M_STEPR, -11 }, { M_STEP, -10 }, { M_STEPR, -9 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEP, 10 }, { 0, 0 } };
const MOp	kMobB0E3C[] = { { M_STEPR, -11 }, { M_STEP, -10 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEPR, 11 }, { 0, 0 } };
const MOp	kMobB0EAA[] = { { M_RAYR, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0EA2[] = { { M_STEP, -10 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEP, 10 }, { M_RAYR, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0EEE[] = { { M_RAY, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAY, 10 }, { 0, 0 } };
const MOp	kMobB0EE6[] = { { M_STEPR, -11 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEPR, 11 }, { M_RAY, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAY, 10 }, { 0, 0 } };
const MOp	kMobB0E77[] = { { M_STEPR, -11 }, { M_STEPR, -10 }, { M_STEP, -9 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEPR, 10 }, { 0, 0 } };
const MOp	kMobB0E47[] = { { M_STEPR, -11 }, { M_STEPR, -10 }, { M_STEP, -9 }, { M_STEP, 9 }, { M_STEPR, 11 }, { 0, 0 } };
const MOp	kMobB0EBB[] = { { M_RAYR, -11 }, { M_RAY, -9 }, { M_RAY, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0EB3[] = { { M_STEPR, -10 }, { M_STEPR, -1 }, { M_STEPR, 1 }, { M_STEPR, 10 }, { M_RAYR, -11 }, { M_RAY, -9 }, { M_RAY, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0EFF[] = { { M_RAYR, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobB0EF7[] = { { M_STEPR, -11 }, { M_STEP, -9 }, { M_STEP, 9 }, { M_STEPR, 11 }, { M_RAYR, -10 }, { M_RAYR, -1 }, { M_RAYR, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobB0E84[] = { { M_STEPR, -11 }, { M_STEPR, -10 }, { M_STEPR, -9 }, { M_STEP, -1 }, { M_STEP, 1 }, { M_STEPR, 10 }, { 0, 0 } };
const MOp	kMobB0E52[] = { { M_STEPR, -11 }, { M_STEPR, -10 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEPR, 11 }, { 0, 0 } };
const MOp	kMobB0ECC[] = { { M_RAYR, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0EC4[] = { { M_STEPR, -10 }, { M_STEP, -1 }, { M_STEP, 1 }, { M_STEPR, 10 }, { M_RAYR, -11 }, { M_RAYR, -9 }, { M_RAYR, 9 }, { M_RAYR, 11 }, { 0, 0 } };
const MOp	kMobB0F10[] = { { M_RAYR, -10 }, { M_RAY, -1 }, { M_RAY, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobB0F08[] = { { M_STEPR, -11 }, { M_STEPR, -9 }, { M_STEPR, 9 }, { M_STEPR, 11 }, { M_RAYR, -10 }, { M_RAY, -1 }, { M_RAY, 1 }, { M_RAYR, 10 }, { 0, 0 } };
const MOp	*const kMobBTable[80] =
{
	kMobB0DE0, kMobB0DF6, kMobB0DE3, kMobB0DF6, kMobB0DE6, kMobB0DF6, kMobB0DEB, kMobB0DF6, kMobB0DF6, kMobToWait, kMobB0E0B, kMobB0E03, kMobB0E1C, kMobB0E14, kMobB0E25, kMobToWait,
	kMobB0E26, kMobB0E5D, kMobB0E29, kMobB0E5D, kMobB0E2C, kMobB0E5D, kMobB0E31, kMobB0E5D, kMobB0E5D, kMobToWait, kMobB0E99, kMobB0E91, kMobB0EDD, kMobB0ED5, kMobB0E25, kMobToWait,
	kMobB0DE0, kMobB0E6A, kMobB0DE3, kMobB0E6A, kMobB0E2C, kMobB0E6A, kMobB0E3C, kMobB0E6A, kMobB0E6A, kMobToWait, kMobB0EAA, kMobB0EA2, kMobB0EEE, kMobB0EE6, kMobB0E25, kMobToWait,
	kMobB0E26, kMobB0E77, kMobB0E29, kMobB0E77, kMobB0E2C, kMobB0E77, kMobB0E47, kMobB0E77, kMobB0E77, kMobToWait, kMobB0EBB, kMobB0EB3, kMobB0EFF, kMobB0EF7, kMobB0E25, kMobToWait,
	kMobB0E26, kMobB0E84, kMobB0E29, kMobB0E84, kMobB0E2C, kMobB0E84, kMobB0E52, kMobB0E84, kMobB0E84, kMobToWait, kMobB0ECC, kMobB0EC4, kMobB0F10, kMobB0F08, kMobB0E25, kMobToWait,
};
const MOp	kMobW0FB1[] = { { M_STEP, 10 }, { 0, 0 } };
const MOp	kMobW0FC7[] = { { M_STEP, 11 }, { M_STEP, 10 }, { M_STEP, 9 }, { M_STEP, 1 }, { M_STEP, -1 }, { M_STEP, -10 }, { 0, 0 } };
const MOp	kMobW0FB4[] = { { M_RAY, 10 }, { 0, 0 } };
const MOp	kMobW0FB7[] = { { M_STEP, 21 }, { M_STEP, 19 }, { 0, 0 } };
const MOp	kMobW0FBC[] = { { M_STEP, 11 }, { M_STEP, 10 }, { M_STEP, 9 }, { M_STEP, -9 }, { M_STEP, -11 }, { 0, 0 } };
const MOp	kMobW0FDC[] = { { M_RAY, 11 }, { M_RAY, 9 }, { M_RAY, -9 }, { M_RAY, -11 }, { 0, 0 } };
const MOp	kMobW0FD4[] = { { M_STEP, 10 }, { M_STEP, 1 }, { M_STEP, -1 }, { M_STEP, -10 }, { M_RAY, 11 }, { M_RAY, 9 }, { M_RAY, -9 }, { M_RAY, -11 }, { 0, 0 } };
const MOp	kMobW0FED[] = { { M_RAY, 10 }, { M_RAY, 1 }, { M_RAY, -1 }, { M_RAY, -10 }, { 0, 0 } };
const MOp	kMobW0FE5[] = { { M_STEP, 11 }, { M_STEP, 9 }, { M_STEP, -9 }, { M_STEP, -11 }, { M_RAY, 10 }, { M_RAY, 1 }, { M_RAY, -1 }, { M_RAY, -10 }, { 0, 0 } };
const MOp	kMobW0FF6[] = { { 0, 0 } };
const MOp	kMobW0FF7[] = { { M_STEPR, 10 }, { 0, 0 } };
const MOp	kMobW102E[] = { { M_STEP, 11 }, { M_STEPR, 10 }, { M_STEPR, 9 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEPR, -10 }, { 0, 0 } };
const MOp	kMobW0FFA[] = { { M_RAYR, 10 }, { 0, 0 } };
const MOp	kMobW0FFD[] = { { M_STEPR, 21 }, { M_STEPR, 19 }, { 0, 0 } };
const MOp	kMobW1002[] = { { M_STEP, 11 }, { M_STEPR, 10 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEP, -11 }, { 0, 0 } };
const MOp	kMobW106A[] = { { M_RAY, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAY, -11 }, { 0, 0 } };
const MOp	kMobW1062[] = { { M_STEPR, 10 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEPR, -10 }, { M_RAY, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAY, -11 }, { 0, 0 } };
const MOp	kMobW10AE[] = { { M_RAYR, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobW10A6[] = { { M_STEP, 11 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEP, -11 }, { M_RAYR, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobW103B[] = { { M_STEPR, 11 }, { M_STEP, 10 }, { M_STEPR, 9 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEP, -10 }, { 0, 0 } };
const MOp	kMobW100D[] = { { M_STEPR, 11 }, { M_STEP, 10 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEPR, -11 }, { 0, 0 } };
const MOp	kMobW107B[] = { { M_RAYR, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW1073[] = { { M_STEP, 10 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEP, -10 }, { M_RAYR, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW10BF[] = { { M_RAY, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAY, -10 }, { 0, 0 } };
const MOp	kMobW10B7[] = { { M_STEPR, 11 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEPR, -11 }, { M_RAY, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAY, -10 }, { 0, 0 } };
const MOp	kMobW1048[] = { { M_STEPR, 11 }, { M_STEPR, 10 }, { M_STEP, 9 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEPR, -10 }, { 0, 0 } };
const MOp	kMobW1018[] = { { M_STEPR, 11 }, { M_STEPR, 10 }, { M_STEP, 9 }, { M_STEP, -9 }, { M_STEPR, -11 }, { 0, 0 } };
const MOp	kMobW108C[] = { { M_RAYR, 11 }, { M_RAY, 9 }, { M_RAY, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW1084[] = { { M_STEPR, 10 }, { M_STEPR, 1 }, { M_STEPR, -1 }, { M_STEPR, -10 }, { M_RAYR, 11 }, { M_RAY, 9 }, { M_RAY, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW10D0[] = { { M_RAYR, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobW10C8[] = { { M_STEPR, 11 }, { M_STEP, 9 }, { M_STEP, -9 }, { M_STEPR, -11 }, { M_RAYR, 10 }, { M_RAYR, 1 }, { M_RAYR, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobW1055[] = { { M_STEPR, 11 }, { M_STEPR, 10 }, { M_STEPR, 9 }, { M_STEP, 1 }, { M_STEP, -1 }, { M_STEPR, -10 }, { 0, 0 } };
const MOp	kMobW1023[] = { { M_STEPR, 11 }, { M_STEPR, 10 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEPR, -11 }, { 0, 0 } };
const MOp	kMobW109D[] = { { M_RAYR, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW1095[] = { { M_STEPR, 10 }, { M_STEP, 1 }, { M_STEP, -1 }, { M_STEPR, -10 }, { M_RAYR, 11 }, { M_RAYR, 9 }, { M_RAYR, -9 }, { M_RAYR, -11 }, { 0, 0 } };
const MOp	kMobW10E1[] = { { M_RAYR, 10 }, { M_RAY, 1 }, { M_RAY, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	kMobW10D9[] = { { M_STEPR, 11 }, { M_STEPR, 9 }, { M_STEPR, -9 }, { M_STEPR, -11 }, { M_RAYR, 10 }, { M_RAY, 1 }, { M_RAY, -1 }, { M_RAYR, -10 }, { 0, 0 } };
const MOp	*const kMobWTable[80] =
{
	kMobW0FB1, kMobW0FC7, kMobW0FB4, kMobW0FC7, kMobW0FB7, kMobW0FC7, kMobW0FBC, kMobW0FC7, kMobW0FC7, kMobToWait, kMobW0FDC, kMobW0FD4, kMobW0FED, kMobW0FE5, kMobW0FF6, kMobToWait,
	kMobW0FF7, kMobW102E, kMobW0FFA, kMobW102E, kMobW0FFD, kMobW102E, kMobW1002, kMobW102E, kMobW102E, kMobToWait, kMobW106A, kMobW1062, kMobW10AE, kMobW10A6, kMobW0FF6, kMobToWait,
	kMobW0FB1, kMobW103B, kMobW0FB4, kMobW103B, kMobW0FFD, kMobW103B, kMobW100D, kMobW103B, kMobW103B, kMobToWait, kMobW107B, kMobW1073, kMobW10BF, kMobW10B7, kMobW0FF6, kMobToWait,
	kMobW0FF7, kMobW1048, kMobW0FFA, kMobW1048, kMobW0FFD, kMobW1048, kMobW1018, kMobW1048, kMobW1048, kMobToWait, kMobW108C, kMobW1084, kMobW10D0, kMobW10C8, kMobW0FF6, kMobToWait,
	kMobW0FF7, kMobW1055, kMobW0FFA, kMobW1055, kMobW0FFD, kMobW1055, kMobW1023, kMobW1055, kMobW1055, kMobToWait, kMobW109D, kMobW1095, kMobW10E1, kMobW10D9, kMobW0FF6, kMobToWait,
};

// the target A: worth taking (2) when the exchange there doesn't lose the piece (value L)
inline void MobGain (bool white)
{
	c += 4;
	s.tr = RdW(0x63f + s.a);
	const uint16	x = RdW(0x3b0 + s.a);
	c += white ? 8 : 9;
	const uint16	sum = (uint16) ((white ? (x & 0xff) : (x >> 8)) + s.tr);
	if (sum < s.l)
		return;
	c += 3;
	s.k = (uint16) (s.k + 2);
}

void MobStep (int off, bool full, bool white)
{
	c += 2 + 5;
	s.a = (uint16) (s.a + off);
	const uint16	v = RdW(s.a);
	if (!(v & 0x8000))
	{
		c += 6;
		s.k = (uint16) (s.k + RdW(0x6c2 + s.a));
		if (full)
		{
			c += 4;
			if (!(v & (white ? 0x20 : 0x40)))
				MobGain(white);
		}
	}
	c += 1;
	s.a = s.trb;
}

void MobRay (int dir, bool full, bool white)
{
	c += 1; ToDR((uint16) dir);
	c += 1 + 1;
	for (;;)
	{
		c += 1;
		s.a = (uint16) (s.a + FromDR());
		c += 4;
		if (RdW(s.a))
			break;
		c += 6;
		s.k = (uint16) (s.k + RdW(0x6c2 + s.a));
		if (full)
		{
			c += 4;
			s.tr = RdW(0x63f + s.a);
			const uint16	x = RdW(0x3b0 + s.a);
			c += white ? 8 : 9;
			if ((uint16) ((white ? (x & 0xff) : (x >> 8)) + s.tr) >= s.l)
			{
				c += 3;
				s.k = (uint16) (s.k + 2);
			}
		}
	}
	c += 1;
	const uint16	v = RdW(s.a);
	if (!(v & 0x8000))
	{
		c += 6;
		s.k = (uint16) (s.k + RdW(0x6c2 + s.a));
		if (full)
		{
			c += 4;
			if (!(v & (white ? 0x20 : 0x40)))
				MobGain(white);
		}
	}
	c += 1;
	s.a = s.trb;
}

// FALSE: the chip went back to its command wait
bool Mobility (bool white)
{
	c += 1;
	if (!white)
		Fill(0x2aa, 0x83, 0);
	c += 2;
	s.a = RdW(white ? 0x746 : 0x745);
	do
	{
		c += 23;
		s.trb = s.a;
		s.k = RdW(0x2aa + s.a);
		s.l = RdW(0x63f + s.a);
		const uint32	idx = (uint32) (((RdW(s.a) & 0x1e) | RdW(0x9e + s.a)) >> 1);
		s.tr = white ? 0x340 : 0x2f0;
		const MOp		*p = idx < 80 ? (white ? kMobWTable : kMobBTable)[idx] : kMobToWait;
		if (idx >= 80)
			s.unknown = 1;
		if (p == kMobToWait)
			return (false);
		if (p)
		{
			for (; p->op; p++)
				switch (p->op)
				{
					case M_STEP:	MobStep(p->off, true, white); break;
					case M_STEPR:	MobStep(p->off, false, white); break;
					case M_RAY:		MobRay(p->off, true, white); break;
					case M_RAYR:	MobRay(p->off, false, white); break;
				}
			c += 1;
		}
		c += 4; WrW(0x2aa + s.a, s.k);
		c += 5;
		s.a = RdW(0x745 + s.a);
	} while (s.a);
	c += 1;
	return (true);
}

// ---- 10EA: material. The hands by the data ROM's tables (black's added, white's taken off, each side's worth of
// one more such piece in $7D7 / $7DF), times 8; then each piece on the board is priced by its mobility (data ROM $390
// by $2AA, times 5 / 2 for the types $4B0 marks) and type ($490), counted into the total, and its capture value
// (an eighth of that plus $4A0, plus what the taker's hand gains) goes into $63F + square. Word $97 = the total.
void Material (void)
{
	static const uint16	kHand[7] = { 0x4c0, 0x4d4, 0x4da, 0x4e0, 0x4e6, 0x4ec, 0x4f0 };
	c += 1 + 1;
	s.k = 0;
	for (int i = 0; i < 7; i++)
		for (int white = 0; white < 2; white++)
		{
			const uint32	at = (uint32) (kHand[i] + RdW((white ? 0x8b : 0x83) + i));
			s.k = (uint16) (white ? s.k - Rom(at) : s.k + Rom(at));
			s.b = (uint16) (Rom(at + 1) - Rom(at));
			s.a = (uint16) at;
			c += 14; WrW((white ? 0x7df : 0x7d7) + i, s.b);
		}
	c += 5;
	s.k = (uint16) (s.k << 3);
	c += 2; WrW(0x7de, 0);
	c += 2; WrW(0x7e6, 0);
	c += 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.trb);
		if (piece && !(c += 1, piece & 0x8000))
		{
			c += 9;
			const uint16	type = (piece & 0x1e) >> 1;
			s.tr = type;
			const uint16	x = Rom((uint32) (0x390 + RdW(0x2aa + s.trb)));
			if (!Rom(0x4b0 + type))
			{
				c += 8;
				s.a = x;
			}
			else
			{
				c += 11;
				const uint16	five = (uint16) (x * 5);
				s.a = (uint16) ((five >> 1) | (five & 0x8000));
			}
			c += 12;
			s.a = (uint16) (s.a + Rom(0x490 + type));
			s.l = s.a;
			s.a = (uint16) ((int16) s.a >> 3);
			s.a = (uint16) (s.a + Rom(0x4a0 + type));
			c += 8;
			s.tr = (uint16) ((type & 0xe) >> 1);
			const bool	white = (piece & 0x20) != 0;
			c += 4;
			s.a = (uint16) (s.a + RdW((white ? 0x7d7 : 0x7df) + s.tr));
			c += 4; WrW(0x63f + s.trb, s.a);
			if (!white)
			{
				c += 4;
				s.k = (uint16) (s.k + s.l);
			}
			else
			{
				c += 3;
				s.k = (uint16) (s.k - s.l);
			}
		}
		c += 6;
		s.trb++;
	}
	s.tr = 0x6e;
	c += 2; WrW(0x97, s.k);
	c += 1;
}

// ---- 120E: the exchange again, after the second support pass: as 0CDF for $3B0, and $433 + square gets what each
// side nets (its cheapest less the other's, never below 0; white's << 8), DR carrying black's
void Exchange2 (void)
{
	LAB_ENTER(0x120e);
	c += 1 + 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		if (!(RdW(s.trb) & 0x8000))
		{
			const uint16	bm = RdW(0x1a4 + s.trb);
			s.k = bm & 0xff;
			c += 9; ToDR(s.k);
			c += 2; WrW(0x7d2, (uint16) (bm >> 8));
			const uint16	wm = RdW(0x227 + s.trb);
			s.l = wm & 0xff;
			s.tr = s.l;
			c += 11; WrW(0x7d3, (uint16) (wm >> 8));
			const uint16	cnt = RdW(0x539 + s.trb);
			c += 9; WrW(0x7d4, cnt & 0xff);
			c += 2; WrW(0x7d5, (uint16) (cnt >> 8));
			const uint16	piece = Swap(RdW(s.trb));
			const uint16	bw = Weighted(cnt & 0xff, piece, 14), ww = Weighted(cnt >> 8, piece, 13);
			c += 8; WrW(0x7d4, bw);
			c += 4; WrW(0x7d5, ww);
			c += 4;
			if (bw < ww)
			{
				c += 4;
				if (bw < 3)
				{
					c += 1;
					s.l = 0;
				}
				else
				{
					c += 5;
					const uint16	two = RdW(0x7d2);
					if (s.l < two) { c += 1; s.l = 0; }
					else s.l = (uint16) (s.l - two);
				}
				c += 2; FromDR();
				const uint16	d = cdr;
				c += 2;
				if (s.tr < d) { c += 2; s.tr = 0; }
				else s.tr = (uint16) (s.tr - d);
			}
			else if (bw == ww)
			{
				c += 1 + 5;
				const uint16	w2 = RdW(0x7d3), b2 = RdW(0x7d2);
				if (s.k < w2) { c += 1; s.k = 0; } else s.k = (uint16) (s.k - w2);
				c += 5;
				if (s.l < b2) { c += 1; s.l = 0; } else s.l = (uint16) (s.l - b2);
				c += 3;
				if (s.k < 3) { c += 2; s.k = 0; s.l = 0; }
				c += 1; FromDR();
				const uint16	d = cdr;
				c += 2;
				if (d < s.l) { ToDR((uint16) (d - s.l)); c += 1; c += 1; ToDR(0); }
				else { ToDR((uint16) (d - s.l)); c += 1; }
				c += 4;
				if (s.tr < s.k) { c += 1; s.tr = 0; }
				else s.tr = (uint16) (s.tr - s.k);
			}
			else
			{
				c += 1 + 4;
				if (ww < 3)
				{
					c += 1;
					s.k = 0;
				}
				else
				{
					c += 5;
					const uint16	two = RdW(0x7d3);
					if (s.k < two) { c += 1; s.k = 0; }
					else s.k = (uint16) (s.k - two);
				}
				c += 1; FromDR();
				const uint16	d = cdr;
				c += 2;
				if (d < s.tr) { ToDR((uint16) (d - s.tr)); c += 2; ToDR(0); c += 1; }
				else { ToDR((uint16) (d - s.tr)); c += 1; }
			}
			c += 7; WrW(0x3b0 + s.trb, (uint16) (s.k | Swap(s.l)));
			c += 3; FromDR();
			c += 4; WrW(0x433 + s.trb, (uint16) (Swap(s.tr) | cdr));
		}
		c += 6;
		s.trb++;
	}
	c += 1;
}

// ---- 2000 (black) and 2692 (white): threats. Each piece of the side (not pinned), from the most valuable, tries its
// moves to squares where the exchange doesn't lose it (its value K under the other side's part of $3B0), and marks
// what it would newly attack from there: $5BC + square gets a flag (black 1, white | $100), the board map $2AA the
// mover's value (black's low byte, white's high). Rays mark on while the target's net ($433, the other side's part)
// stays above K. Then the drops from the hand do the same into the drop map $32D. The horse and dragon keep the 8
// flags round themselves and put them back after.

enum { T_MK = 1, T_SF, T_RY, T_TF, T_IO };
struct TOp { int8 op; int8 off; int8 arg; };
enum { H_STEP = 1, H_KSTEP, H_BRAY, H_LANCE, H_SAVE, H_RESTORE };
struct HOp { int8 op; int8 off; const TOp *empty, *capture; };
const HOp	kHndToWait[] = { { 0, 0, NULL, NULL } };

const TOp	kThrB2406[] = { { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2408[] = { { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrB240B[] = { { T_MK, -21, 0 }, { T_MK, -19, 0 }, { 0, 0, 0 } };
const TOp	kThrB240E[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2414[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, -9, 0 }, { T_MK, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrB2419[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB241D[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, -9, 0 }, { T_MK, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB2422[] = { { T_MK, -11, 0 }, { T_MK, -1, 0 }, { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2426[] = { { T_MK, -9, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB242A[] = { { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB242E[] = { { T_RY, -11, 0 }, { T_SF, 0, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB2430[] = { { T_SF, 0, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB2436[] = { { T_RY, -9, 0 }, { T_SF, 0, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2438[] = { { T_SF, 0, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB243E[] = { { T_RY, 9, 0 }, { T_SF, 0, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2440[] = { { T_SF, 0, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2446[] = { { T_RY, 11, 0 }, { T_SF, 0, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB2448[] = { { T_SF, 0, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB244E[] = { { T_RY, -11, 0 }, { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB2450[] = { { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB245A[] = { { T_RY, -9, 0 }, { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB245C[] = { { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2466[] = { { T_RY, 9, 0 }, { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2468[] = { { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -11, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2472[] = { { T_RY, 11, 0 }, { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB2474[] = { { T_SF, 0, 0 }, { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB247E[] = { { T_MK, -10, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { T_RY, -11, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB248B[] = { { T_RY, -10, 0 }, { T_SF, 0, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB248D[] = { { T_SF, 0, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB2493[] = { { T_RY, -1, 0 }, { T_SF, 0, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2495[] = { { T_SF, 0, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB249B[] = { { T_RY, 1, 0 }, { T_SF, 0, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB249D[] = { { T_SF, 0, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24A3[] = { { T_RY, 10, 0 }, { T_SF, 0, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24A5[] = { { T_SF, 0, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24AB[] = { { T_RY, -10, 0 }, { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24AD[] = { { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24B7[] = { { T_RY, -1, 0 }, { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24B9[] = { { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24C3[] = { { T_RY, 1, 0 }, { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24C5[] = { { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -10, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24CF[] = { { T_RY, 10, 0 }, { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24D1[] = { { T_SF, 0, 0 }, { T_MK, -11, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrB24DB[] = { { T_MK, -11, 0 }, { T_MK, 11, 0 }, { T_IO, -10, -9 }, { T_IO, -1, 9 }, { T_RY, -10, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB24F2[] = { { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_IO, -10, -11 }, { T_IO, 1, 11 }, { T_RY, -10, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2509[] = { { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_IO, 10, 11 }, { T_IO, -1, -11 }, { T_RY, -10, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2520[] = { { T_MK, -11, 0 }, { T_MK, 11, 0 }, { T_IO, 10, 9 }, { T_IO, 1, -9 }, { T_RY, -10, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2537[] = { { T_TF, -11, 0 }, { T_TF, -10, 0 }, { T_TF, -9, 0 }, { T_TF, -1, 0 }, { T_TF, 9, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB2550[] = { { T_TF, -11, 0 }, { T_TF, -10, 0 }, { T_TF, -9, 0 }, { T_TF, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrB2561[] = { { T_TF, -11, 0 }, { T_TF, -10, 0 }, { T_TF, -9, 0 }, { T_TF, -1, 0 }, { T_TF, 9, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB257A[] = { { T_TF, -11, 0 }, { T_TF, -1, 0 }, { T_TF, 1, 0 }, { T_TF, 9, 0 }, { 0, 0, 0 } };
const TOp	kThrB258B[] = { { T_TF, -9, 0 }, { T_TF, -1, 0 }, { T_TF, 1, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB259C[] = { { T_TF, -11, 0 }, { T_TF, -9, 0 }, { T_TF, -1, 0 }, { T_TF, 9, 0 }, { T_TF, 10, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB25B5[] = { { T_TF, -10, 0 }, { T_TF, 9, 0 }, { T_TF, 10, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const TOp	kThrB25C6[] = { { T_TF, -11, 0 }, { T_TF, -9, 0 }, { T_TF, 1, 0 }, { T_TF, 9, 0 }, { T_TF, 10, 0 }, { T_TF, 11, 0 }, { 0, 0, 0 } };
const HOp	kHndB21B6[] = { { H_STEP, -10, kThrB2406, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB21DF[] = { { H_STEP, -11, kThrB2414, NULL }, { H_STEP, -10, kThrB2419, NULL }, { H_STEP, -9, kThrB241D, NULL }, { H_STEP, -1, kThrB2422, NULL }, { H_STEP, 1, kThrB2426, NULL }, { H_STEP, 10, kThrB242A, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB21BA[] = { { H_LANCE, -10, NULL, kThrB2408 }, { 0, 0, NULL, NULL } };
const HOp	kHndB21C8[] = { { H_STEP, -21, kThrB240B, NULL }, { H_STEP, -19, kThrB240B, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB21CF[] = { { H_STEP, -11, kThrB240E, NULL }, { H_STEP, -10, kThrB240E, NULL }, { H_STEP, -9, kThrB240E, NULL }, { H_STEP, 9, kThrB240E, NULL }, { H_STEP, 11, kThrB240E, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB21F2[] = { { H_BRAY, -11, kThrB2430, kThrB242E }, { H_BRAY, -9, kThrB2438, kThrB2436 }, { H_BRAY, 9, kThrB2440, kThrB243E }, { H_BRAY, 11, kThrB2448, kThrB2446 }, { 0, 0, NULL, NULL } };
const HOp	kHndB2237[] = { { H_SAVE, 0, NULL, NULL }, { H_BRAY, -11, kThrB2450, kThrB244E }, { H_BRAY, -9, kThrB245C, kThrB245A }, { H_BRAY, 9, kThrB2468, kThrB2466 }, { H_BRAY, 11, kThrB2474, kThrB2472 }, { H_STEP, -10, kThrB247E, NULL }, { H_STEP, -1, kThrB247E, NULL }, { H_STEP, 1, kThrB247E, NULL }, { H_STEP, 10, kThrB247E, NULL }, { H_RESTORE, 0, NULL, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB22EA[] = { { H_BRAY, -10, kThrB248D, kThrB248B }, { H_BRAY, -1, kThrB2495, kThrB2493 }, { H_BRAY, 1, kThrB249D, kThrB249B }, { H_BRAY, 10, kThrB24A5, kThrB24A3 }, { 0, 0, NULL, NULL } };
const HOp	kHndB232F[] = { { H_SAVE, 0, NULL, NULL }, { H_BRAY, -10, kThrB24AD, kThrB24AB }, { H_BRAY, -1, kThrB24B9, kThrB24B7 }, { H_BRAY, 1, kThrB24C5, kThrB24C3 }, { H_BRAY, 10, kThrB24D1, kThrB24CF }, { H_STEP, -11, kThrB24DB, NULL }, { H_STEP, -9, kThrB24F2, NULL }, { H_STEP, 9, kThrB2509, NULL }, { H_STEP, 11, kThrB2520, NULL }, { H_RESTORE, 0, NULL, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndB23E2[] = { { H_KSTEP, -11, kThrB2537, NULL }, { H_KSTEP, -10, kThrB2550, NULL }, { H_KSTEP, -9, kThrB2561, NULL }, { H_KSTEP, -1, kThrB257A, NULL }, { H_KSTEP, 1, kThrB258B, NULL }, { H_KSTEP, 9, kThrB259C, NULL }, { H_KSTEP, 10, kThrB25B5, NULL }, { H_KSTEP, 11, kThrB25C6, NULL }, { 0, 0, NULL, NULL } };
const HOp	*const kHndB[16] = { kHndB21B6, kHndB21DF, kHndB21BA, kHndB21DF, kHndB21C8, kHndB21DF, kHndB21CF, kHndB21DF, kHndB21DF, kHndToWait, kHndB21F2, kHndB2237, kHndB22EA, kHndB232F, kHndB23E2, kHndB23E2 };
const TOp	kThrW2AB0[] = { { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AB2[] = { { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AB5[] = { { T_MK, 21, 0 }, { T_MK, 19, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AB8[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2ABE[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, 9, 0 }, { T_MK, 1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AC3[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AC7[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, 9, 0 }, { T_MK, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2ACC[] = { { T_MK, 11, 0 }, { T_MK, 1, 0 }, { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AD0[] = { { T_MK, 9, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AD4[] = { { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AD8[] = { { T_RY, 11, 0 }, { T_SF, 0, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2ADA[] = { { T_SF, 0, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AE0[] = { { T_RY, 9, 0 }, { T_SF, 0, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AE2[] = { { T_SF, 0, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AE8[] = { { T_RY, -9, 0 }, { T_SF, 0, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AEA[] = { { T_SF, 0, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AF0[] = { { T_RY, -11, 0 }, { T_SF, 0, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AF2[] = { { T_SF, 0, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AF8[] = { { T_RY, 11, 0 }, { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2AFA[] = { { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B04[] = { { T_RY, 9, 0 }, { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B06[] = { { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B10[] = { { T_RY, -9, 0 }, { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B12[] = { { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 11, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B1C[] = { { T_RY, -11, 0 }, { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B1E[] = { { T_SF, 0, 0 }, { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B28[] = { { T_MK, 10, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { T_RY, 11, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B35[] = { { T_RY, 10, 0 }, { T_SF, 0, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B37[] = { { T_SF, 0, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B3D[] = { { T_RY, 1, 0 }, { T_SF, 0, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B3F[] = { { T_SF, 0, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B45[] = { { T_RY, -1, 0 }, { T_SF, 0, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B47[] = { { T_SF, 0, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B4D[] = { { T_RY, -10, 0 }, { T_SF, 0, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B4F[] = { { T_SF, 0, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B55[] = { { T_RY, 10, 0 }, { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B57[] = { { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B61[] = { { T_RY, 1, 0 }, { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B63[] = { { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B6D[] = { { T_RY, -1, 0 }, { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B6F[] = { { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 10, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B79[] = { { T_RY, -10, 0 }, { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B7B[] = { { T_SF, 0, 0 }, { T_MK, 11, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B85[] = { { T_MK, 11, 0 }, { T_MK, -11, 0 }, { T_IO, 10, 9 }, { T_IO, 1, -9 }, { T_RY, 10, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2B9C[] = { { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_IO, 10, 11 }, { T_IO, -1, -11 }, { T_RY, 10, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2BB3[] = { { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_IO, -10, -11 }, { T_IO, 1, 11 }, { T_RY, 10, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2BCA[] = { { T_MK, 11, 0 }, { T_MK, -11, 0 }, { T_IO, -10, -9 }, { T_IO, -1, 9 }, { T_RY, 10, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2BE1[] = { { T_TF, 11, 0 }, { T_TF, 10, 0 }, { T_TF, 9, 0 }, { T_TF, 1, 0 }, { T_TF, -9, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C06[] = { { T_TF, 11, 0 }, { T_TF, 10, 0 }, { T_TF, 9, 0 }, { T_TF, -10, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C1F[] = { { T_TF, 11, 0 }, { T_TF, 10, 0 }, { T_TF, 9, 0 }, { T_TF, 1, 0 }, { T_TF, -9, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C44[] = { { T_TF, 11, 0 }, { T_TF, 1, 0 }, { T_TF, -1, 0 }, { T_TF, -9, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C5D[] = { { T_TF, 9, 0 }, { T_TF, 1, 0 }, { T_TF, -1, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C76[] = { { T_TF, 11, 0 }, { T_TF, 9, 0 }, { T_TF, 1, 0 }, { T_TF, -9, 0 }, { T_TF, -10, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2C9B[] = { { T_TF, 10, 0 }, { T_TF, -9, 0 }, { T_TF, -10, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const TOp	kThrW2CB4[] = { { T_TF, 11, 0 }, { T_TF, 9, 0 }, { T_TF, -1, 0 }, { T_TF, -9, 0 }, { T_TF, -10, 0 }, { T_TF, -11, 0 }, { 0, 0, 0 } };
const HOp	kHndW285C[] = { { H_STEP, 10, kThrW2AB0, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2885[] = { { H_STEP, 11, kThrW2ABE, NULL }, { H_STEP, 10, kThrW2AC3, NULL }, { H_STEP, 9, kThrW2AC7, NULL }, { H_STEP, 1, kThrW2ACC, NULL }, { H_STEP, -1, kThrW2AD0, NULL }, { H_STEP, -10, kThrW2AD4, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2860[] = { { H_LANCE, 10, NULL, kThrW2AB2 }, { 0, 0, NULL, NULL } };
const HOp	kHndW286E[] = { { H_STEP, 21, kThrW2AB5, NULL }, { H_STEP, 19, kThrW2AB5, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2875[] = { { H_STEP, 11, kThrW2AB8, NULL }, { H_STEP, 10, kThrW2AB8, NULL }, { H_STEP, 9, kThrW2AB8, NULL }, { H_STEP, -9, kThrW2AB8, NULL }, { H_STEP, -11, kThrW2AB8, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2898[] = { { H_BRAY, 11, kThrW2ADA, kThrW2AD8 }, { H_BRAY, 9, kThrW2AE2, kThrW2AE0 }, { H_BRAY, -9, kThrW2AEA, kThrW2AE8 }, { H_BRAY, -11, kThrW2AF2, kThrW2AF0 }, { 0, 0, NULL, NULL } };
const HOp	kHndW28DD[] = { { H_SAVE, 0, NULL, NULL }, { H_BRAY, 11, kThrW2AFA, kThrW2AF8 }, { H_BRAY, 9, kThrW2B06, kThrW2B04 }, { H_BRAY, -9, kThrW2B12, kThrW2B10 }, { H_BRAY, -11, kThrW2B1E, kThrW2B1C }, { H_STEP, 10, kThrW2B28, NULL }, { H_STEP, 1, kThrW2B28, NULL }, { H_STEP, -1, kThrW2B28, NULL }, { H_STEP, -10, kThrW2B28, NULL }, { H_RESTORE, 0, NULL, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2990[] = { { H_BRAY, 10, kThrW2B37, kThrW2B35 }, { H_BRAY, 1, kThrW2B3F, kThrW2B3D }, { H_BRAY, -1, kThrW2B47, kThrW2B45 }, { H_BRAY, -10, kThrW2B4F, kThrW2B4D }, { 0, 0, NULL, NULL } };
const HOp	kHndW29D5[] = { { H_SAVE, 0, NULL, NULL }, { H_BRAY, 10, kThrW2B57, kThrW2B55 }, { H_BRAY, 1, kThrW2B63, kThrW2B61 }, { H_BRAY, -1, kThrW2B6F, kThrW2B6D }, { H_BRAY, -10, kThrW2B7B, kThrW2B79 }, { H_STEP, 11, kThrW2B85, NULL }, { H_STEP, 9, kThrW2B9C, NULL }, { H_STEP, -9, kThrW2BB3, NULL }, { H_STEP, -11, kThrW2BCA, NULL }, { H_RESTORE, 0, NULL, NULL }, { 0, 0, NULL, NULL } };
const HOp	kHndW2A88[] = { { H_KSTEP, 11, kThrW2BE1, NULL }, { H_KSTEP, 10, kThrW2C06, NULL }, { H_KSTEP, 9, kThrW2C1F, NULL }, { H_KSTEP, 1, kThrW2C44, NULL }, { H_KSTEP, -1, kThrW2C5D, NULL }, { H_KSTEP, -9, kThrW2C76, NULL }, { H_KSTEP, -10, kThrW2C9B, NULL }, { H_KSTEP, -11, kThrW2CB4, NULL }, { 0, 0, NULL, NULL } };
const HOp	*const kHndW[16] = { kHndW285C, kHndW2885, kHndW2860, kHndW2885, kHndW286E, kHndW2885, kHndW2875, kHndW2885, kHndW2885, kHndToWait, kHndW2898, kHndW28DD, kHndW2990, kHndW29D5, kHndW2A88, kHndW2A88 };

// the flag at `sq`: black writes 1 (4 words), white ORs in $100 (6)
inline void ThrFlag (uint32 sq, bool white)
{
	if (white) { c += 6; WrW(0x5bc + sq, RdW(0x5bc + sq) | 0x100); }
	else { c += 4; WrW(0x5bc + sq, 1); }
}

// the flag and the mover's value in `map` at `sq` (black 11 words, white 15)
inline void ThrMark (uint32 sq, uint32 map, bool white)
{
	ThrFlag(sq, white);
	const uint16	v = RdW(map + sq);
	if (white) { c += 9; WrW(map + sq, Swap((uint16) (Swap(v & 0xff) | s.k))); }
	else { c += 7; WrW(map + sq, (uint16) ((v & 0xff00) | s.k)); }
}

// 217E / 219A / 281E / 283D: a ray from A along DR, marking each square while the other side's net there ($433)
// stays above K, through empty squares; A back from L at the end
void ThrRay (uint32 map, bool white)
{
	s.l = s.a;
	c += 1;
	for (;;)
	{
		c += 1;
		s.a = (uint16) (s.a + FromDR());
		ThrMark(s.a, map, white);
		const uint16	net = RdW(0x433 + s.a);
		s.tr = white ? (uint16) (net & 0xff) : (uint16) (net >> 8);
		c += white ? 9 : 10;
		if (s.k >= s.tr)
			break;
		c += 4;
		if (RdW(s.a) & 0xe0)
			break;
	}
	c += 1;
	s.a = s.l;
}

// a "new attacks" routine, from A (the square moved to), to its RT
void ThrNew (const TOp *p, bool white)
{
	for (; p->op; p++)
		switch (p->op)
		{
			case T_MK:
				c += 1;
				ThrMark((uint32) (s.a + p->off), 0x2aa, white);
				break;
			case T_SF:
				c += 1;
				ThrMark(s.trb, 0x2aa, white);
				break;
			case T_RY:
				c += 1; ToDR((uint16) (int16) p->off);
				c += 1;
				ThrRay(0x2aa, white);
				break;
			case T_TF:
				ThrFlag((uint32) (s.a + p->off), white);
				break;
			case T_IO:
				c += 5;
				if (RdW(s.trb + p->off))
				{
					c += 1;
					ThrMark((uint32) (s.a + p->arg), 0x2aa, white);
				}
				break;
		}
	c += 1;
}

// 20F1 / 276A with its HCALL: TR = the other side's part of the exchange at A; does the mover (K) stay under it?
inline bool ThrSafe (bool white)
{
	const uint16	x = RdW(0x3b0 + s.a);
	s.tr = white ? (uint16) (x & 0xff) : (uint16) (x >> 8);
	c += 1 + (white ? 8 : 9);
	return (s.k < s.tr);
}

void ThrPiece (const HOp *p, bool white)
{
	const uint16	own = white ? 0xa0 : 0xc0, enemy = white ? 0x40 : 0x20;
	for (; p->op; p++)
		switch (p->op)
		{
			case H_STEP:
			case H_KSTEP:
			{
				c += 3 + 5;
				s.a = (uint16) (s.trb + p->off);
				if (RdW(s.a) & own) { c += 1; break; }
				if (p->op == H_KSTEP)
				{
					c += 6;
					if (RdW(0x121 + s.a) & (white ? 0x0080 : 0x8000)) { c += 1; break; }
				}
				else
				{
					const bool	safe = ThrSafe(white);
					c += 1;
					if (!safe) { c += 1; break; }
				}
				c += 1;
				ThrNew(p->empty, white);
				break;
			}
			case H_BRAY:
			case H_LANCE:
			{
				c += p->op == H_BRAY ? 2 : 1;
				s.a = s.trb;
				for (;;)
				{
					s.tr = (uint16) (int16) p->off;
					c += 6;
					s.a = (uint16) (s.a + p->off);
					if (RdW(s.a) & 0xe0)
						break;
					if (p->op == H_LANCE)
						continue;
					const bool	safe = ThrSafe(white);
					c += 1;
					if (safe)
					{
						c += 1;
						ThrNew(p->empty, white);
					}
				}
				c += 3;
				if (RdW(s.a) & enemy)
				{
					const bool	safe = ThrSafe(white);
					c += 1;
					if (safe)
					{
						c += 1;
						ThrNew(p->capture, white);
					}
				}
				break;
			}
			case H_SAVE:
			case H_RESTORE:
			{
				static const int8	kRound[8] = { -11, -10, -9, -1, 1, 9, 10, 11 };
				c += 1;
				for (int i = 0; i < 8; i++)
				{
					const int	off = white ? -kRound[i] : kRound[i];
					c += 6;
					if (p->op == H_SAVE)
						WrW(0x7cf + i, RdW(0x5bc + s.trb + off));
					else
						WrW(0x5bc + s.trb + off, RdW(0x7cf + i));
				}
				s.a = 0x7d7;
				break;
			}
		}
	c += 1;
}

// the drops: each hand piece onto each empty square where the other side's net doesn't beat its value
struct TDrop { uint16 hand, value, first, end; const TOp *ops; };
const TOp	kDropBR[] = { { T_RY, -10, 0 }, { T_RY, -1, 0 }, { T_RY, 1, 0 }, { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kDropBB[] = { { T_RY, -11, 0 }, { T_RY, -9, 0 }, { T_RY, 9, 0 }, { T_RY, 11, 0 }, { 0, 0, 0 } };
const TOp	kDropBG[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, -9, 0 }, { T_MK, -1, 0 }, { T_MK, 1, 0 }, { T_MK, 10, 0 }, { 0, 0, 0 } };
const TOp	kDropBS[] = { { T_MK, -11, 0 }, { T_MK, -10, 0 }, { T_MK, -9, 0 }, { T_MK, 9, 0 }, { T_MK, 11, 0 }, { 0, 0, 0 } };
const TOp	kDropBN[] = { { T_MK, -21, 0 }, { T_MK, -19, 0 }, { 0, 0, 0 } };
const TOp	kDropBL[] = { { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kDropBP[] = { { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kDropWR[] = { { T_RY, 10, 0 }, { T_RY, 1, 0 }, { T_RY, -1, 0 }, { T_RY, -10, 0 }, { 0, 0, 0 } };
const TOp	kDropWB[] = { { T_RY, 11, 0 }, { T_RY, 9, 0 }, { T_RY, -9, 0 }, { T_RY, -11, 0 }, { 0, 0, 0 } };
const TOp	kDropWG[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, 9, 0 }, { T_MK, 1, 0 }, { T_MK, -1, 0 }, { T_MK, -10, 0 }, { 0, 0, 0 } };
const TOp	kDropWS[] = { { T_MK, 11, 0 }, { T_MK, 10, 0 }, { T_MK, 9, 0 }, { T_MK, -9, 0 }, { T_MK, -11, 0 }, { 0, 0, 0 } };
const TOp	kDropWN[] = { { T_MK, 21, 0 }, { T_MK, 19, 0 }, { 0, 0, 0 } };
const TOp	kDropWL[] = { { T_RY, 10, 0 }, { 0, 0, 0 } };
const TOp	kDropWP[] = { { T_MK, 10, 0 }, { 0, 0, 0 } };
const TDrop	kThrDrops[2][7] =
{
	{
		{ 0x89, 0x56, 0x15, 0x6e, kDropBR }, { 0x88, 0x52, 0x15, 0x6e, kDropBB }, { 0x87, 0x3c, 0x15, 0x6e, kDropBG },
		{ 0x86, 0x3a, 0x15, 0x6e, kDropBS }, { 0x85, 0x22, 0x29, 0x6e, kDropBN }, { 0x84, 0x20, 0x1f, 0x6e, kDropBL },
		{ 0x83, 0x08, 0x1f, 0x6e, kDropBP }
	},
	{
		{ 0x91, 0x56, 0x15, 0x6e, kDropWR }, { 0x90, 0x52, 0x15, 0x6e, kDropWB }, { 0x8f, 0x3c, 0x15, 0x6e, kDropWG },
		{ 0x8e, 0x3a, 0x15, 0x6e, kDropWS }, { 0x8d, 0x22, 0x15, 0x5a, kDropWN }, { 0x8c, 0x20, 0x15, 0x64, kDropWL },
		{ 0x8b, 0x08, 0x15, 0x64, kDropWP }
	}
};

// a drop routine (inline marks into the drop map, or rays), to its RT
void ThrDrop (const TOp *p, bool white)
{
	for (; p->op; p++)
		if (p->op == T_MK)
			ThrMark((uint32) (s.a + p->off), 0x32d, white);
		else
		{
			c += 1; ToDR((uint16) (int16) p->off);
			c += 1;
			ThrRay(0x32d, white);
		}
	c += 1;
}

// FALSE: the chip went back to its command wait
bool Threats (bool white)
{
	c += 1;
	if (!white)
	{
		Fill(0x2aa, 0x83, 0x7878);
		Fill(0x32d, 0x83, 0x7878);
		Fill(0x5bc, 0x83, 0);
	}
	c += 2;
	s.trb = RdW(white ? 0x746 : 0x745);
	do
	{
		c += 5;
		if (!RdW(0x9e + s.trb))
		{
			c += 4;
			s.k = RdW(0x63f + s.trb);
			c += 9;
			const HOp	*p = (white ? kHndW : kHndB)[(RdW(s.trb) & 0x1e) >> 1];
			s.tr = white ? 0x504 : 0x4f4;
			if (p == kHndToWait)
				return (false);
			ThrPiece(p, white);
		}
		c += 5;
		s.trb = RdW(0x745 + s.trb);
	} while (s.trb);

	for (int i = 0; i < 7; i++)
	{
		const TDrop	&dr = kThrDrops[white][i];
		c += 4;
		if (!RdW(dr.hand))
			continue;
		c += 2;
		s.k = dr.value;
		for (s.trb = dr.first; s.trb != dr.end; )
		{
			c += 4;
			if (!RdW(s.trb))
			{
				bool	go = true;
				if (dr.hand == (white ? 0x8b : 0x83))
				{
					c += 8;
					go = !RdW((white ? 0x7c6 : 0x7bd) + Rom(0xec + s.trb));
				}
				if (go)
				{
					const uint16	net = RdW(0x433 + s.trb);
					c += white ? 8 : 9;
					if (dr.value < (white ? (uint16) (net & 0xff) : (uint16) (net >> 8)))
					{
						c += 2;
						s.a = s.trb;
						ThrDrop(dr.ops, white);
					}
				}
			}
			c += 6;
			s.trb++;
		}
		s.tr = dr.end;
	}
	c += 1;
	return (true);
}

// ---- 2DC4: after the threats, each square's two cheapest black pieces that reach it (attacking, or threatening to
// by a move or a drop: $7D0, $7D2) and white's ($7D1, $7D3); the attack counts plus the threat flags into $5BC; and
// the exchange as 0CDF had it from those, now in $4B6 + square (white's part << 8)
void Exchange3 (void)
{
	LAB_ENTER(0x2dc4);
	c += 1 + 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		if (!(RdW(s.trb) & 0x8000))
		{
			for (int white = 0; white < 2; white++)
			{
				const uint16	t = RdW(0x2aa + s.trb), d = RdW(0x32d + s.trb), at = RdW((white ? 0x227 : 0x1a4) + s.trb);
				const uint16	a = white ? (uint16) (t >> 8) : (uint16) (t & 0xff), b = white ? (uint16) (d >> 8) : (uint16) (d & 0xff);
				c += white ? 15 : 13;
				s.k = a;
				if (!(a < b)) { c += 1; s.k = b; }
				c += 7;
				const uint16	c1 = at & 0xff, c2 = (uint16) (at >> 8);
				if (s.k < c1)
				{
					c += 3;
					s.l = s.k;
					s.k = c1;
				}
				else
				{
					c += 6;
					s.l = c1;
					if (!(s.k < c2)) { c += 1; s.k = c2; }
				}
				c += 2; WrW(white ? 0x7d1 : 0x7d0, s.l);
				c += 2; WrW(white ? 0x7d3 : 0x7d2, s.k);
			}
			const uint16	sum = (uint16) (RdW(0x539 + s.trb) + RdW(0x5bc + s.trb));
			c += 9; WrW(0x5bc + s.trb, sum);
			const uint16	bt = sum & 0xff, wt = (uint16) (sum >> 8);
			c += 4; ToDR(bt);
			s.tr = wt;
			const uint16	piece = Swap(RdW(s.trb));
			const uint16	bw = Weighted(bt, piece, 14), ww = Weighted(wt, piece, 13);
			c += 1 + 4;
			c += 1; FromDR();
			c += 2; ToDR(bw);
			s.tr = ww;
			c += 3;
			c += 1; FromDR();
			c += 2;
			const uint16	bmin = RdW(0x7d0), wmin = RdW(0x7d1), bsec = RdW(0x7d2), wsec = RdW(0x7d3);
			if (bw < ww)
			{
				c += 2; FromDR();
				c += 1;
				uint16	v;
				if (bw < 3) { c += 1; v = 0; }
				else
				{
					c += 5;
					if (wmin < bsec) { c += 1; v = 0; } else v = (uint16) (wmin - bsec);
				}
				c += 7; WrW(0x4b6 + s.trb, (uint16) (Swap(v) | bmin));
				c += 1;
			}
			else if (bw == ww)
			{
				c += 1;
				c += 2; FromDR();
				c += 1;
				if (bw < 3)
				{
					c += 4; WrW(0x4b6 + s.trb, 0);
					c += 1;
				}
				else
				{
					uint16	v, u;
					c += 5;
					if (bmin < wsec) { c += 1; v = 0; } else v = (uint16) (bmin - wsec);
					c += 5;
					if (wmin < bsec) { c += 1; u = 0; } else u = (uint16) (wmin - bsec);
					c += 6; WrW(0x4b6 + s.trb, (uint16) (v | Swap(u)));
				}
			}
			else
			{
				c += 1 + 3;
				uint16	v;
				if (ww < 3) { c += 1; v = 0; }
				else
				{
					c += 5;
					if (bmin < wsec) { c += 1; v = 0; } else v = (uint16) (bmin - wsec);
				}
				c += 8; WrW(0x4b6 + s.trb, (uint16) (v | Swap(wmin)));
				c += 1;
			}
		}
		c += 6;
		s.trb++;
	}
	c += 1;
}

// ---- 2E71: control. For each square, its ring weight ($7D0 = the ring value << 4, low nibble cleared) against
// each exchange map ($3B0, $433, $4B6): where black's and white's parts differ, a term from the data ROM's curve
// ($514) and the multiplier goes twice into word $98, added when white's side is the stronger, taken off when
// black's.

inline uint16 Shr1 (uint16 v) { return ((uint16) ((v >> 1) | (v & 0x8000))); }
inline void MulMN (uint16 k, uint16 l, uint16 &m, uint16 &n)
{
	const int32	p = (int32) (int16) k * (int16) l;
	m = (uint16) (p >> 15);
	n = (uint16) (p << 1);
}

// 2EFE / 2F03: A into word $98, added (with the HJMP on) or taken off; doubled first for the $3B0 map
inline void ControlAdd (bool twice)
{
	if (twice) s.a = (uint16) (s.a << 1);
	c += twice ? 4 : 3; WrW(0x98, (uint16) (RdW(0x98) + s.a));
	c += 1;
}
inline void ControlSub (bool twice)
{
	if (twice) s.a = (uint16) (s.a << 1);
	c += twice ? 5 : 4; WrW(0x98, (uint16) (RdW(0x98) - s.a));
}

// one side zero: the other's curve value times the ring weight, its middle byte
inline void ControlOne (uint16 part)
{
	s.k = Rom(0x514 + part);
	s.l = RdW(0x7d0);
	uint16	m, n;
	MulMN(s.k, s.l, m, n);
	s.tr = 0xff;
	s.a = (uint16) (Shr1(Swap((uint16) (n | m))) & 0xff);
	c += 14;
}

void ControlTerm (uint32 map, bool twice)
{
	const uint16	x = RdW(map + s.trb);
	const uint16	bk = x & 0xff, wt = (uint16) (x >> 8);
	s.k = bk;
	c += 9;
	if (bk == wt)
		return;
	s.tr = 0x5a;
	c += 4;
	if (bk >= 0x5a)
	{
		c += 3;
		if (wt >= 0x5a)
			return;
		c += 3;
		s.a = RdW(0x7d0);
		ControlSub(twice);
		return;
	}
	c += 3;
	if (wt >= 0x5a)
	{
		c += 2;
		s.a = RdW(0x7d0);
		ControlAdd(twice);
		return;
	}
	c += 3;
	if (!bk)
	{
		ControlOne(wt);
		ControlAdd(twice);
		return;
	}
	c += 2;
	if (!wt)
	{
		ControlOne(bk);
		ControlSub(twice);
		return;
	}

	// both sides: the curve values' product picks a second curve value, the difference times the ring weight
	// scales it
	const uint16	k1 = Rom(0x514 + bk), l1 = Rom(0x514 + wt);
	s.k = k1;
	c += 6; WrW(0x7d1, k1);
	s.l = l1;
	c += 5; WrW(0x7d2, l1);
	uint16	m, n;
	MulMN(k1, l1, m, n);
	uint16	a = (uint16) (0 - n);
	const uint16	b = (uint16) (2 - m - (n != 0));
	a = (uint16) ((a & 0xfe00) | b);
	s.tr = Shr1(Swap(a));
	c += 10;
	c += 5;
	const bool	neg = k1 < l1;
	if (neg)
		c += 2;
	s.k = neg ? (uint16) (l1 - k1) : (uint16) (k1 - l1);
	s.l = RdW(0x7d0);
	MulMN(s.k, s.l, m, n);
	s.k = (uint16) ((Shr1(n) & 0x7fff) | Swap(Shr1(Swap((uint16) (m & 1)))));		// the product's low word
	s.l = Rom(s.tr);
	MulMN(s.k, s.l, m, n);
	s.a = m;
	c += 21;
	if (neg)
		ControlAdd(twice);
	else
		ControlSub(twice);
}

void Control (void)
{
	LAB_ENTER(0x2e71);
	c += 1 + 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		if (!(RdW(s.trb) & 0x8000))
		{
			const uint16	ring = RdW(0x6c2 + s.trb);
			s.tr = 0xf0;
			c += 9; WrW(0x7d0, (uint16) (((ring << 4) | 15) & 0xf0));
			ControlTerm(0x3b0, true);
			ControlTerm(0x433, false);
			ControlTerm(0x4b6, false);
		}
		c += 6;
		s.trb++;
	}
	s.tr = 0x6e;
	c += 1;
}

// ---- 301D: white's best captures. Each white piece's captures (and moves into or within the zone, with a promotion
// bonus per piece, $7CF) get a gain from the exchange maps; the best five go into a sorted list of 5-word records at
// $6C3 (gain, $7D5, from, to, $7D4: the line the ray crossed under black's attack), the best gain's bound into $6C2.
// Then a third record that shares squares with the first two gives way to the fourth or fifth, and the first three
// records' lines are marked in $1A4 (1, 2, 4).

enum { C_CAP = 1, C_PROMO, C_RCAP, C_RPROMO, C_KCAP };
struct COp { int8 op; int8 off; uint8 bonus; };
const COp	kCapToWait[] = { { 0, 0, 0 } };

const COp	kCap32C9[] = { { C_PROMO, 10, 0x12 }, { 0, 0, 0 } };
const COp	kCap32F1[] = { { C_CAP, 11, 0x00 }, { C_CAP, 10, 0x00 }, { C_CAP, 9, 0x00 }, { C_CAP, 1, 0x00 }, { C_CAP, -1, 0x00 }, { C_CAP, -10, 0x00 }, { 0, 0, 0 } };
const COp	kCap32CE[] = { { C_RPROMO, 10, 0x0b }, { 0, 0, 0 } };
const COp	kCap32D3[] = { { C_PROMO, 21, 0x0a }, { C_PROMO, 19, 0x0a }, { 0, 0, 0 } };
const COp	kCap32DC[] = { { C_PROMO, 11, 0x00 }, { C_PROMO, 10, 0x00 }, { C_PROMO, 9, 0x00 }, { C_PROMO, -9, 0x00 }, { C_PROMO, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap32FE[] = { { C_RPROMO, 11, 0x0c }, { C_RPROMO, 9, 0x0c }, { C_RPROMO, -9, 0x0c }, { C_RPROMO, -11, 0x0c }, { 0, 0, 0 } };
const COp	kCap330F[] = { { C_RCAP, 11, 0x00 }, { C_RCAP, 9, 0x00 }, { C_RCAP, -9, 0x00 }, { C_RCAP, -11, 0x00 }, { C_CAP, 10, 0x00 }, { C_CAP, 1, 0x00 }, { C_CAP, -1, 0x00 }, { C_CAP, -10, 0x00 }, { 0, 0, 0 } };
const COp	kCap3320[] = { { C_RPROMO, 10, 0x0c }, { C_RPROMO, 1, 0x0c }, { C_RPROMO, -1, 0x0c }, { C_RPROMO, -10, 0x0c }, { 0, 0, 0 } };
const COp	kCap3331[] = { { C_RCAP, 10, 0x00 }, { C_RCAP, 1, 0x00 }, { C_RCAP, -1, 0x00 }, { C_RCAP, -10, 0x00 }, { C_CAP, 11, 0x00 }, { C_CAP, 9, 0x00 }, { C_CAP, -9, 0x00 }, { C_CAP, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap3342[] = { { C_KCAP, 11, 0x00 }, { C_KCAP, 10, 0x00 }, { C_KCAP, 9, 0x00 }, { C_KCAP, 1, 0x00 }, { C_KCAP, -1, 0x00 }, { C_KCAP, -9, 0x00 }, { C_KCAP, -10, 0x00 }, { C_KCAP, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap339A[] = { { C_CAP, 11, 0x00 }, { 0, 0, 0 } };
const COp	kCap3383[] = { { C_PROMO, 11, 0x00 }, { C_PROMO, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap33AA[] = { { C_RPROMO, 11, 0x0c }, { C_RPROMO, -11, 0x0c }, { 0, 0, 0 } };
const COp	kCap33BC[] = { { C_RCAP, 11, 0x00 }, { C_RCAP, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap33D8[] = { { C_CAP, 11, 0x00 }, { C_CAP, -11, 0x00 }, { 0, 0, 0 } };
const COp	kCap339D[] = { { C_CAP, 10, 0x00 }, { C_CAP, -10, 0x00 }, { 0, 0, 0 } };
const COp	kCap338C[] = { { C_PROMO, 10, 0x00 }, { 0, 0, 0 } };
const COp	kCap33C6[] = { { C_RPROMO, 10, 0x0c }, { C_RPROMO, -10, 0x0c }, { 0, 0, 0 } };
const COp	kCap33DD[] = { { C_RCAP, 10, 0x00 }, { C_RCAP, -10, 0x00 }, { 0, 0, 0 } };
const COp	kCap33A2[] = { { C_CAP, 9, 0x00 }, { 0, 0, 0 } };
const COp	kCap3391[] = { { C_PROMO, 9, 0x00 }, { C_PROMO, -9, 0x00 }, { 0, 0, 0 } };
const COp	kCap33B3[] = { { C_RPROMO, 9, 0x0c }, { C_RPROMO, -9, 0x0c }, { 0, 0, 0 } };
const COp	kCap33C1[] = { { C_RCAP, 9, 0x00 }, { C_RCAP, -9, 0x00 }, { 0, 0, 0 } };
const COp	kCap33E2[] = { { C_CAP, 9, 0x00 }, { C_CAP, -9, 0x00 }, { 0, 0, 0 } };
const COp	kCap33A5[] = { { C_CAP, 1, 0x00 }, { C_CAP, -1, 0x00 }, { 0, 0, 0 } };
const COp	kCap33CF[] = { { C_RPROMO, 1, 0x0c }, { C_RPROMO, -1, 0x0c }, { 0, 0, 0 } };
const COp	kCap33E7[] = { { C_RCAP, 1, 0x00 }, { C_RCAP, -1, 0x00 }, { 0, 0, 0 } };
const COp	*const kCapTable[80] =	// data ROM $56E
{
	kCap32C9, kCap32F1, kCap32CE, kCap32F1, kCap32D3, kCap32F1, kCap32DC, kCap32F1, kCap32F1, kCapToWait, kCap32FE, kCap330F, kCap3320, kCap3331, kCap3342, kCapToWait,
	NULL, kCap339A, NULL, kCap339A, NULL, kCap339A, kCap3383, kCap339A, kCap339A, kCapToWait, kCap33AA, kCap33BC, NULL, kCap33D8, NULL, kCapToWait,
	kCap32C9, kCap339D, kCap32CE, kCap339D, NULL, kCap339D, kCap338C, kCap339D, kCap339D, kCapToWait, NULL, kCap339D, kCap33C6, kCap33DD, NULL, kCapToWait,
	NULL, kCap33A2, NULL, kCap33A2, NULL, kCap33A2, kCap3391, kCap33A2, kCap33A2, kCapToWait, kCap33B3, kCap33C1, NULL, kCap33E2, NULL, kCapToWait,
	NULL, kCap33A5, NULL, kCap33A5, NULL, kCap33A5, NULL, kCap33A5, kCap33A5, kCapToWait, NULL, kCap33A5, kCap33CF, kCap33E7, NULL, kCapToWait,
};

// a record's 5 words copied one place down (4 words each)
inline void CapShift (uint32 from, uint32 to)
{
	for (uint32 i = 0; i < 5; i++)
	{
		c += 4;
		WrW(to + i, RdW(from + i));
	}
}

// the new record at `at`: from TRB, to A, gain L, then $7D4 and $7D5, and the RT
inline void CapRecord (uint32 at)
{
	c += 2; WrW(at + 2, s.trb);
	c += 2; WrW(at + 3, s.a);
	c += 2; WrW(at, s.l);
	c += 4; WrW(at + 4, RdW(0x7d4));
	c += 4; WrW(at + 1, RdW(0x7d5));
	c += 1;
}

// 3130: the gain L into the list, if it beats the fifth
void CapInsert (void)
{
	c += 4;
	if (!(RdW(0x6d7) < s.l)) { c += 1; return; }
	c += 4;
	if (!(RdW(0x6d2) < s.l)) { CapRecord(0x6d7); return; }
	c += 4;
	if (!(RdW(0x6cd) < s.l)) { CapShift(0x6d2, 0x6d7); CapRecord(0x6d2); return; }
	c += 4;
	if (!(RdW(0x6c8) < s.l)) { CapShift(0x6d2, 0x6d7); CapShift(0x6cd, 0x6d2); CapRecord(0x6cd); return; }
	c += 4;
	if (!(RdW(0x6c3) < s.l)) { CapShift(0x6d2, 0x6d7); CapShift(0x6cd, 0x6d2); CapShift(0x6c8, 0x6cd); CapRecord(0x6c8); return; }
	// a new best: its bound ($7D3) kept when not negative and there's no line, else 0, in $6C2
	c += 4;
	if (!(RdW(0x7d3) & 0x8000))
	{
		c += 4;
		if (!RdW(0x7d4))
			goto keep;
	}
	c += 2; WrW(0x7d3, 0);
keep:
	c += 4; WrW(0x6c2, RdW(0x7d3));
	CapShift(0x6d2, 0x6d7); CapShift(0x6cd, 0x6d2); CapShift(0x6c8, 0x6cd); CapShift(0x6c3, 0x6c8);
	CapRecord(0x6c3);
}

// 30F6: the gain of the capture TRB x A (L = the target's value, K = the taker's), with DR's bonus on both
void CapGain (void)
{
	c += 2; WrW(0x7d3, s.l);
	c += 3;
	if (s.l >= 0x78)
	{
		CapInsert();
		return;
	}
	c += 2; s.k = (uint16) (s.k + FromDR());
	c += 1;
	c += 2; s.l = (uint16) (s.l + FromDR());
	c += 1;
	c += 3;
	const uint16	bonus = FromDR();
	c += 1; WrW(0x7d3, (uint16) (RdW(0x7d3) + bonus));
	c += 2; WrW(0x7d5, FromDR());
	c += 1; ToDR(s.k);
	c += 7; WrW(0x7d1, RdW(0x3b0 + s.a) & 0xff);
	c += 7; WrW(0x7d2, RdW(0x4b6 + s.a) & 0xff);
	c += 3;
	if (!(RdW(0x7d2) < RdW(0x7d1)))
	{
		c += 4;
		WrW(0x7d2, RdW(0x7d1));
	}
	// the taker less white's cheapest there, the target less that
	c += 5;
	const uint16	w1 = RdW(0x7d1);
	const bool		under = s.k < w1;
	s.k = (uint16) (s.k - w1);
	if (!under)
	{
		c += 4;
		s.l = (uint16) (s.l - s.k);
		if (s.l & 0x8000) { c += 1; return; }
	}
	c += 1; FromDR();
	c += 3;
	const uint16	w2 = RdW(0x7d2);
	const bool		under2 = cdr < w2;
	ToDR((uint16) (cdr - w2));
	c += 1;
	if (!under2)
	{
		c += 3; FromDR();
		c += 2; WrW(0x7d3, (uint16) (RdW(0x7d3) - cdr));
	}
	CapInsert();
}

// 30ED: a plain capture of A from TRB
inline void CapPlain (void)
{
	s.l = RdW(0x63f + s.a);
	s.k = RdW(0x63f + s.trb);
	c += 9; ToDR(0);
	CapGain();
}

// 3265 / 3277 (via their HJMP or HCALL): a capture, or a move, touching the zone takes the bonus
inline bool InZone (void) { return (s.trb > 0x50 || s.a > 0x50); }
void CapPromo (bool capture)
{
	c += 3;
	if (!(s.trb > 0x50))
	{
		c += 3;
		if (!(s.a > 0x50))
		{
			if (capture) { c += 1; CapPlain(); }
			else c += 1;
			return;
		}
	}
	if (capture)
	{
		c += 4; s.l = RdW(0x63f + s.a);
	}
	else
	{
		c += 1; s.l = 0;
	}
	c += 4; s.k = RdW(0x63f + s.trb);
	c += 2; ToDR(RdW(0x7cf));
	c += 1;
	CapGain();
}

void CapRay (int dir, bool promo)
{
	s.tr = (uint16) dir;
	c += 2; WrW(0x7d4, 0);
	c += 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		if (promo)
		{
			c += 1;
			CapPromo(false);
		}
		c += 6;
		if (RdW(0x539 + s.a) & 0xff)
		{
			c += 2; WrW(0x7d4, s.tr);
		}
	}
	c += 3;
	if (!(RdW(s.a) & 0x40)) { c += 1; return; }
	if (promo)
	{
		c += 1;
		CapPromo(true);
		return;
	}
	c += 1;
	CapPlain();
	c += 1;
}

// FALSE: the chip went back to its command wait
bool Captures (void)
{
	LAB_ENTER(0x301d);
	c += 1;
	Fill(0x6c2, 0x1a, 0);
	c += 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		if (RdW(s.trb) & 0x20)
		{
			c += 14;
			const uint32	idx = (uint32) (((RdW(s.trb) & 0x1e) | RdW(0x9e + s.trb)) >> 1);
			s.tr = 0x56e;
			const COp		*p = idx < 80 ? kCapTable[idx] : kCapToWait;
			if (idx >= 80)
				s.unknown = 1;
			if (p == kCapToWait)
				return (false);
			if (p)
			{
				for (; p->op; p++)
					switch (p->op)
					{
						case C_CAP:
						case C_KCAP:
							if (p->op == C_KCAP)
							{
								c += 6;
								if (RdW(0x121 + s.trb + p->off) & 0x80)
									break;
							}
							c += 2 + 3;
							s.a = (uint16) (s.trb + p->off);
							WrW(0x7d4, 0);
							c += 4;
							if (RdW(s.a) & 0x40)
							{
								c += 1;
								CapPlain();
							}
							c += 1;
							break;
						case C_PROMO:
						{
							c += 2; WrW(0x7cf, p->bonus);
							c += 2 + 3;
							s.a = (uint16) (s.trb + p->off);
							WrW(0x7d4, 0);
							c += 4;
							const uint16	v = RdW(s.a);
							if (v & 0xa0) { c += 1; break; }
							c += 3;
							c += 1;
							CapPromo((v & 0x40) != 0);
							break;
						}
						case C_RCAP:
							c += 2;
							CapRay(p->off, false);
							break;
						case C_RPROMO:
							c += 2; WrW(0x7cf, p->bonus);
							c += 2;
							CapRay(p->off, true);
							break;
					}
				c += 1;
			}
		}
		c += 6;
		s.trb++;
	}

	// a third choice sharing a square with each of the first two gives way to the fourth, or else the fifth
	c += 4;
	s.k = RdW(0x6cf); s.l = RdW(0x6d0);
	c += 4;
	if (s.k == RdW(0x6c5) || (c += 4, s.l == RdW(0x6c6)))
	{
		c += 4;
		if (s.k == RdW(0x6ca) || (c += 4, s.l == RdW(0x6cb)))
		{
			c += 4;
			s.k = RdW(0x6d4); s.l = RdW(0x6d5);
			c += 4;
			bool	use = true;
			if (s.k == RdW(0x6c5) || (c += 4, s.l == RdW(0x6c6)))
			{
				c += 4;
				if (s.k == RdW(0x6ca) || (c += 4, s.l == RdW(0x6cb)))
					use = false;
			}
			if (use)
			{
				c += 2; WrW(0x6cf, s.k);
				c += 2; WrW(0x6d0, s.l);
				c += 4; WrW(0x6cd, RdW(0x6d2));
				c += 4; WrW(0x6ce, RdW(0x6d3));
				c += 4; WrW(0x6d1, RdW(0x6d6));
				c += 1;
			}
			else
			{
				c += 4;
				s.k = RdW(0x6d9); s.l = RdW(0x6da);
				c += 4;
				bool	use5 = true;
				if (s.k == RdW(0x6c5) || (c += 4, s.l == RdW(0x6c6)))
				{
					c += 4;
					if (s.k == RdW(0x6ca) || (c += 4, s.l == RdW(0x6cb)))
						use5 = false;
				}
				if (use5)
				{
					c += 2; WrW(0x6cf, s.k);
					c += 2; WrW(0x6d0, s.l);
					c += 4; WrW(0x6cd, RdW(0x6d7));
					c += 4; WrW(0x6ce, RdW(0x6d8));
					c += 4; WrW(0x6d1, RdW(0x6db));
				}
			}
		}
	}

	// the first three records' from-squares and lines, in $1A4: 1, 2, 4
	Fill(0x1a4, 0x83, 0);
	static const struct { uint16 from, line, bit; } kLines[3] = { { 0x6c5, 0x6c7, 1 }, { 0x6ca, 0x6cc, 2 }, { 0x6cf, 0x6d1, 4 } };
	for (int i = 0; i < 3; i++)
	{
		c += 4;
		s.a = RdW(kLines[i].from);
		if (!s.a)
			break;
		if (i == 0) { c += 4; WrW(0x1a4 + s.a, 1); }
		else { c += 6; WrW(0x1a4 + s.a, RdW(0x1a4 + s.a) | kLines[i].bit); }
		c += 4;
		const uint16	dir = RdW(kLines[i].line);
		if (!dir)
			continue;
		s.k = dir;
		c += 1;
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			if (i == 0) { c += 4; WrW(0x1a4 + s.a, 1); }
			else { c += 6; WrW(0x1a4 + s.a, RdW(0x1a4 + s.a) | kLines[i].bit); }
			c += 1;
		}
	}
	c += 1;
	return (true);
}

// ---- 33EC: black's moves. Each black piece's moves (promoting ones with a bonus per piece, $7CF) get a gain from
// the exchange map, cut where they'd leave white's best captures (301D's records) standing, and go into two
// lists: three 5-word records by score at $6DC (score, K, L, from, to; no two sharing a from- or to-square) and
// three 3-word ones by gain at $6EB (gain, from, to). $6DC starts as white's best gain less max($6C2, the second).

enum { BM_PSTEP = 1, BM_PRAY, BM_STEP, BM_RAY, BM_KSTEP };
struct BOp { int8 op; int8 off; uint8 bonus; };
const BOp	kBMToWait[] = { { 0, 0, 0 } };

const BOp	kBM35C0[] = { { BM_PSTEP, -10, 0x12 }, { 0, 0, 0 } };
const BOp	kBM35E9[] = { { BM_STEP, -11, 0x00 }, { BM_STEP, -10, 0x00 }, { BM_STEP, -9, 0x00 }, { BM_STEP, -1, 0x00 }, { BM_STEP, 1, 0x00 }, { BM_STEP, 10, 0x00 }, { 0, 0, 0 } };
const BOp	kBM35C5[] = { { BM_PRAY, -10, 0x0b }, { 0, 0, 0 } };
const BOp	kBM35CB[] = { { BM_PSTEP, -21, 0x0a }, { BM_PSTEP, -19, 0x0a }, { 0, 0, 0 } };
const BOp	kBM35D4[] = { { BM_PSTEP, -11, 0x00 }, { BM_PSTEP, -10, 0x00 }, { BM_PSTEP, -9, 0x00 }, { BM_PSTEP, 9, 0x00 }, { BM_PSTEP, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3614[] = { { BM_PRAY, -11, 0x0c }, { BM_PRAY, -9, 0x0c }, { BM_PRAY, 9, 0x0c }, { BM_PRAY, 11, 0x0c }, { 0, 0, 0 } };
const BOp	kBM3629[] = { { BM_RAY, -11, 0x00 }, { BM_RAY, -9, 0x00 }, { BM_RAY, 9, 0x00 }, { BM_RAY, 11, 0x00 }, { BM_STEP, -10, 0x00 }, { BM_STEP, -1, 0x00 }, { BM_STEP, 1, 0x00 }, { BM_STEP, 10, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3652[] = { { BM_PRAY, -10, 0x0c }, { BM_PRAY, -1, 0x0c }, { BM_PRAY, 1, 0x0c }, { BM_PRAY, 10, 0x0c }, { 0, 0, 0 } };
const BOp	kBM3667[] = { { BM_RAY, -10, 0x00 }, { BM_RAY, -1, 0x00 }, { BM_RAY, 1, 0x00 }, { BM_RAY, 10, 0x00 }, { BM_STEP, -11, 0x00 }, { BM_STEP, -9, 0x00 }, { BM_STEP, 9, 0x00 }, { BM_STEP, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3690[] = { { BM_KSTEP, -11, 0x00 }, { BM_KSTEP, -10, 0x00 }, { BM_KSTEP, -9, 0x00 }, { BM_KSTEP, -1, 0x00 }, { BM_KSTEP, 1, 0x00 }, { BM_KSTEP, 9, 0x00 }, { BM_KSTEP, 10, 0x00 }, { BM_KSTEP, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3710[] = { { BM_STEP, -11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM36F9[] = { { BM_PSTEP, -11, 0x00 }, { BM_PSTEP, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM373E[] = { { BM_PRAY, -11, 0x0c }, { BM_PRAY, 11, 0x0c }, { 0, 0, 0 } };
const BOp	kBM3754[] = { { BM_RAY, -11, 0x00 }, { BM_RAY, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3778[] = { { BM_STEP, -11, 0x00 }, { BM_STEP, 11, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3718[] = { { BM_STEP, -10, 0x00 }, { BM_STEP, 10, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3702[] = { { BM_PSTEP, -10, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3762[] = { { BM_PRAY, -10, 0x0c }, { BM_PRAY, 10, 0x0c }, { 0, 0, 0 } };
const BOp	kBM3787[] = { { BM_RAY, -10, 0x00 }, { BM_RAY, 10, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3727[] = { { BM_STEP, -9, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3707[] = { { BM_PSTEP, -9, 0x00 }, { BM_PSTEP, 9, 0x00 }, { 0, 0, 0 } };
const BOp	kBM3749[] = { { BM_PRAY, -9, 0x0c }, { BM_PRAY, 9, 0x0c }, { 0, 0, 0 } };
const BOp	kBM375B[] = { { BM_RAY, -9, 0x00 }, { BM_RAY, 9, 0x00 }, { 0, 0, 0 } };
const BOp	kBM378E[] = { { BM_STEP, -9, 0x00 }, { BM_STEP, 9, 0x00 }, { 0, 0, 0 } };
const BOp	kBM372F[] = { { BM_STEP, -1, 0x00 }, { BM_STEP, 1, 0x00 }, { 0, 0, 0 } };
const BOp	kBM376D[] = { { BM_PRAY, -1, 0x0c }, { BM_PRAY, 1, 0x0c }, { 0, 0, 0 } };
const BOp	kBM379D[] = { { BM_RAY, -1, 0x00 }, { BM_RAY, 1, 0x00 }, { 0, 0, 0 } };
const BOp	*const kBMTable[80] =	// data ROM $5BE
{
	kBM35C0, kBM35E9, kBM35C5, kBM35E9, kBM35CB, kBM35E9, kBM35D4, kBM35E9, kBM35E9, kBMToWait, kBM3614, kBM3629, kBM3652, kBM3667, kBM3690, kBMToWait,
	NULL, kBM3710, NULL, kBM3710, NULL, kBM3710, kBM36F9, kBM3710, kBM3710, kBMToWait, kBM373E, kBM3754, NULL, kBM3778, NULL, kBMToWait,
	kBM35C0, kBM3718, kBM35C5, kBM3718, NULL, kBM3718, kBM3702, kBM3718, kBM3718, kBMToWait, NULL, kBM3718, kBM3762, kBM3787, NULL, kBMToWait,
	NULL, kBM3727, NULL, kBM3727, NULL, kBM3727, kBM3707, kBM3727, kBM3727, kBMToWait, kBM3749, kBM375B, NULL, kBM378E, NULL, kBMToWait,
	NULL, kBM372F, NULL, kBM372F, NULL, kBM372F, NULL, kBM372F, kBM372F, kBMToWait, NULL, kBM372F, kBM376D, kBM379D, NULL, kBMToWait,
};

// n words copied up a record (4 instructions each)
inline void BmCopy (uint32 from, uint32 to, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		c += 4;
		WrW(to + i, RdW(from + i));
	}
}

inline void BmRecord (uint32 at)
{
	c += 2; WrW(at, s.tr);
	c += 2; WrW(at + 1, s.k);
	c += 2; WrW(at + 2, s.l);
	c += 2; WrW(at + 3, s.trb);
	c += 2; WrW(at + 4, s.a);
}

// does the record at `at` share the move's from- or to-square? (4 words each test)
inline bool BmShares (uint32 at)
{
	c += 4;
	if (RdW(at + 3) == s.trb)
		return (true);
	c += 4;
	return (RdW(at + 4) == s.a);
}

// 342B on: the gain L less K, then the lists
void BmTail (void)
{
	c += 3;
	s.l = (uint16) (s.l - s.k);

	// what the move leaves of white's best capture: on one of their lines ($1A4 bits) that record is gone; the
	// piece moving away from a record's target takes it out too
	c += 6;
	if (!(RdW(0x1a4 + s.a) & 1))
	{
		c += 6;
		s.tr = RdW(0x6c3);
		if (RdW(0x6c6) != s.trb)
			goto line_done;
	}
	c += 6;
	if (!(RdW(0x1a4 + s.a) & 2))
	{
		c += 6;
		s.tr = RdW(0x6c8);
		if (RdW(0x6cb) != s.trb)
			goto first_check;
	}
	c += 6;
	if (!(RdW(0x1a4 + s.a) & 4))
	{
		c += 6;
		s.tr = RdW(0x6cd);
		if (RdW(0x6d0) != s.trb)
			goto first_check;
	}
	c += 1;
	s.tr = 0;
first_check:
	c += 4;
	if (RdW(0x6c6) == s.trb)
	{
		c += 4;
		if (RdW(0x6c5) != s.a)
		{
			c += 4;
			if (s.tr < RdW(0x6c4))
			{
				c += 1;
				s.tr = RdW(0x6c4);
			}
		}
	}
line_done:
	c += 5; WrW(0x7d0, (uint16) (RdW(0x6c3) - s.tr));
	c += 3;
	if (!(s.tr < s.k)) { c += 1; s.tr = s.k; }
	c += 5;
	s.tr = (uint16) (s.tr + RdW(0x7d0) + s.l);

	// the score list
	c += 1;
	if (!(s.tr & 0x8000) && (c += 1, s.tr != 0))
	{
		c += 3;
		const uint16	x3 = RdW(0x6e6);
		bool			in = false;
		if (!(s.tr < x3))
		{
			c += 1;
			if (s.tr != x3)
				in = true;
			else
			{
				c += 4;
				const uint16	l3 = RdW(0x6e8);
				if (!(s.l < l3))
				{
					c += 1;
					in = s.l != l3;
				}
			}
		}
		if (in)
		{
			// above the third: against the second, then the first (a tie goes on to the L)
			int	place = 3;
			c += 4;
			const uint16	x2 = RdW(0x6e1);
			if (!(s.tr < x2))
			{
				c += 1;
				bool	above2 = s.tr != x2;
				if (!above2)
				{
					c += 4;
					const uint16	l2 = RdW(0x6e3);
					if (!(s.l < l2))
					{
						c += 1;
						above2 = s.l != l2;
					}
				}
				if (above2)
				{
					place = 2;
					c += 4;
					const uint16	x1 = RdW(0x6dc);
					if (!(s.tr < x1))
					{
						c += 1;
						bool	above1 = s.tr != x1;
						if (!above1)
						{
							c += 4;
							const uint16	l1 = RdW(0x6de);
							if (!(s.l < l1))
							{
								c += 1;
								above1 = s.l != l1;
							}
						}
						if (above1)
							place = 1;
					}
				}
			}
			if (place == 3)
			{
				if (!BmShares(0x6dc) && !BmShares(0x6e1))
				{
					BmRecord(0x6e6);
					c += 1;
				}
			}
			else if (place == 2)
			{
				if (!BmShares(0x6dc))
				{
					if (!BmShares(0x6e1))
						BmCopy(0x6e1, 0x6e6, 5);
					BmRecord(0x6e1);
					c += 1;
				}
			}
			else
			{
				if (!BmShares(0x6dc))
				{
					if (BmShares(0x6e1))
						BmCopy(0x6dc, 0x6e1, 5);
					else
					{
						BmCopy(0x6e1, 0x6e6, 5);
						BmCopy(0x6dc, 0x6e1, 5);
						c += 1;
					}
				}
				BmRecord(0x6dc);
			}
		}
	}

	// the gain list
	c += 3;
	if (s.l & 0x8000) { c += 1; return; }
	c += 1;
	s.tr = s.l;
	c += 3;
	const uint16	y3 = RdW(0x6f1);
	if (s.l < y3 || (c += 1, s.l == y3)) { c += 1; return; }
	c += 4;
	const uint16	y2 = RdW(0x6ee);
	if (s.l < y2 || (c += 1, s.l == y2))
	{
		c += 2; WrW(0x6f1, s.tr);
		c += 2; WrW(0x6f2, s.trb);
		c += 2; WrW(0x6f3, s.a);
		c += 1;
		return;
	}
	c += 4;
	const uint16	y1 = RdW(0x6eb);
	if (s.l < y1 || (c += 1, s.l == y1))
	{
		BmCopy(0x6ee, 0x6f1, 3);
		c += 2; WrW(0x6ee, s.tr);
		c += 2; WrW(0x6ef, s.trb);
		c += 2; WrW(0x6f0, s.a);
		c += 1;
		return;
	}
	BmCopy(0x6ee, 0x6f1, 3);
	BmCopy(0x6eb, 0x6ee, 3);
	c += 2; WrW(0x6eb, s.tr);
	c += 2; WrW(0x6ec, s.trb);
	c += 2; WrW(0x6ed, s.a);
	c += 1;
}

// 3418: the move TRB to A: L = the target's value, K = what the mover would lose there (its value over white's part
// of the exchange)
void BmEval (void)
{
	s.k = 0;
	s.l = RdW(0x63f + s.a);
	s.tr = (uint16) (RdW(0x3b0 + s.a) >> 8);
	const uint16	v = RdW(0x63f + s.trb);
	c += 18;
	if (!(v < s.tr)) { c += 1; s.k = (uint16) (v - s.tr); }
	BmTail();
}

// 3579: a move touching black's zone (words up to $32) takes the bonus on both values, else plain
void BmPromo (void)
{
	c += 3;
	if (!(s.trb <= 0x32))
	{
		c += 3;
		if (!(s.a <= 0x32))
		{
			c += 1;
			BmEval();
			return;
		}
	}
	const uint16	bonus = RdW(0x7cf);
	s.l = (uint16) (RdW(0x63f + s.a) + bonus);
	s.tr = (uint16) (RdW(0x3b0 + s.a) >> 8);
	const uint16	v = (uint16) (RdW(0x63f + s.trb) + bonus);
	c += 7 + 7 + 8;
	if (v < s.tr) { c += 1; s.k = 0; }
	else s.k = (uint16) (v - s.tr);
	c += 2;
	BmTail();
}

// 35A0 / 35B0: a ray along $7D4, each empty square a move, then the white piece ending it
void BmRay (bool promo)
{
	const uint16	dir = RdW(0x7d4);
	c += 1;
	s.a = s.trb;
	for (;;)
	{
		c += 7;
		s.a = (uint16) (s.a + dir);
		const uint16	v = RdW(s.a);
		if (v & 0x8000) { c += 1; return; }
		c += 1;
		if (v)
		{
			c += 3;
			if (!(v & 0x20)) { c += 1; return; }
			c += 1;
			if (promo) BmPromo();
			else { BmEval(); c += 1; }
			return;
		}
		c += 1;
		if (promo) BmPromo(); else BmEval();
		c += 1;
	}
}

// FALSE: the chip went back to its command wait
bool BlackMoves (void)
{
	LAB_ENTER(0x33ec);
	c += 1;
	Fill(0x6dc, 0x18, 0);
	c += 6;
	s.a = RdW(0x6c2);
	if (s.a < RdW(0x6c8)) { c += 1; s.a = RdW(0x6c8); }
	c += 5; WrW(0x6dc, (uint16) (RdW(0x6c3) - s.a));
	c += 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		if (RdW(s.trb) & 0x40)
		{
			c += 14;
			const uint32	idx = (uint32) (((RdW(s.trb) & 0x1e) | RdW(0x9e + s.trb)) >> 1);
			s.tr = 0x5be;
			const BOp		*p = idx < 80 ? kBMTable[idx] : kBMToWait;
			if (idx >= 80)
				s.unknown = 1;
			if (p == kBMToWait)
				return (false);
			if (p)
			{
				for (; p->op; p++)
					switch (p->op)
					{
						case BM_PSTEP:
						{
							c += 2; WrW(0x7cf, p->bonus);
							c += 2 + 5;
							s.a = (uint16) (s.trb + p->off);
							if (RdW(s.a) & 0xc0) { c += 1; break; }
							c += 1;
							BmPromo();
							break;
						}
						case BM_PRAY:
							c += 2; WrW(0x7cf, p->bonus);
							c += 2; WrW(0x7d4, (uint16) (int16) p->off);
							c += 1;
							BmRay(true);
							break;
						case BM_RAY:
							c += 2; WrW(0x7d4, (uint16) (int16) p->off);
							c += 1;
							BmRay(false);
							break;
						case BM_KSTEP:
						case BM_STEP:
							if (p->op == BM_KSTEP)
							{
								c += 6;
								if (RdW(0x121 + s.trb + p->off) & 0x8000)
									break;
							}
							c += 6;
							s.a = (uint16) (s.trb + p->off);
							if (RdW(s.a) & 0xc0)
								break;
							c += 1;
							BmEval();
							break;
					}
				c += 1;
			}
		}
		c += 6;
		s.a = ++s.trb;
	}
	c += 1;
	return (true);
}

// ---- 37A4: black's choice. Each of the top three moves (while its score is positive) is played against the lists
// by 3903 into a 4-word outcome; the best by $99 - $9A + $9B stays in $99-$9C, and a white piece it takes has its
// attacks taken off the map at $5BC (high byte) by the piece's program (data ROM $60E).

enum { U_STEP = 1, U_RAY };
struct UOp { int8 op; int8 off; uint16 tbl; };
const UOp	kUnToWait[] = { { 0, 0, 0 } };

const UOp	kUn3878[] = { { U_STEP, 10, 0x000 }, { 0, 0, 0 } };
const UOp	kUn388F[] = { { U_STEP, 11, 0x000 }, { U_STEP, 10, 0x000 }, { U_STEP, -10, 0x000 }, { U_STEP, 9, 0x000 }, { U_STEP, 1, 0x000 }, { U_STEP, -1, 0x000 }, { 0, 0, 0 } };
const UOp	kUn387B[] = { { U_RAY, 10, 0x226 }, { 0, 0, 0 } };
const UOp	kUn387F[] = { { U_STEP, 21, 0x000 }, { U_STEP, 19, 0x000 }, { 0, 0, 0 } };
const UOp	kUn3884[] = { { U_STEP, 11, 0x000 }, { U_STEP, -11, 0x000 }, { U_STEP, 10, 0x000 }, { U_STEP, 9, 0x000 }, { U_STEP, -9, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38A4[] = { { U_RAY, 11, 0x256 }, { U_RAY, -11, 0x246 }, { U_RAY, 9, 0x256 }, { U_RAY, -9, 0x246 }, { 0, 0, 0 } };
const UOp	kUn389C[] = { { U_STEP, 10, 0x000 }, { U_STEP, -10, 0x000 }, { U_STEP, 1, 0x000 }, { U_STEP, -1, 0x000 }, { U_RAY, 11, 0x256 }, { U_RAY, -11, 0x246 }, { U_RAY, 9, 0x256 }, { U_RAY, -9, 0x246 }, { 0, 0, 0 } };
const UOp	kUn38B9[] = { { U_RAY, 10, 0x226 }, { U_RAY, -10, 0x216 }, { U_RAY, 1, 0x276 }, { U_RAY, -1, 0x276 }, { 0, 0, 0 } };
const UOp	kUn38B1[] = { { U_STEP, 11, 0x000 }, { U_STEP, -11, 0x000 }, { U_STEP, 9, 0x000 }, { U_STEP, -9, 0x000 }, { U_RAY, 10, 0x226 }, { U_RAY, -10, 0x216 }, { U_RAY, 1, 0x276 }, { U_RAY, -1, 0x276 }, { 0, 0, 0 } };
const UOp	kUn38CE[] = { { U_STEP, 11, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38C6[] = { { U_STEP, 11, 0x000 }, { U_STEP, -11, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38D9[] = { { U_RAY, 11, 0x256 }, { U_RAY, -11, 0x246 }, { 0, 0, 0 } };
const UOp	kUn38F1[] = { { U_STEP, 11, 0x000 }, { U_STEP, -11, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38D1[] = { { U_STEP, 10, 0x000 }, { U_STEP, -10, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38CB[] = { { U_STEP, 10, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38E0[] = { { U_STEP, 10, 0x000 }, { U_STEP, -10, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38EA[] = { { U_RAY, 10, 0x226 }, { U_RAY, -10, 0x216 }, { 0, 0, 0 } };
const UOp	kUn38D6[] = { { U_STEP, 9, 0x000 }, { 0, 0, 0 } };
const UOp	kUn388A[] = { { U_STEP, 9, 0x000 }, { U_STEP, -9, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38AA[] = { { U_RAY, 9, 0x256 }, { U_RAY, -9, 0x246 }, { 0, 0, 0 } };
const UOp	kUn38F6[] = { { U_STEP, 9, 0x000 }, { U_STEP, -9, 0x000 }, { 0, 0, 0 } };
const UOp	kUn3897[] = { { U_STEP, 1, 0x000 }, { U_STEP, -1, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38E5[] = { { U_STEP, 1, 0x000 }, { U_STEP, -1, 0x000 }, { 0, 0, 0 } };
const UOp	kUn38BF[] = { { U_RAY, 1, 0x276 }, { U_RAY, -1, 0x276 }, { 0, 0, 0 } };
const UOp	*const kUnTable[80] =	// data ROM $60E
{
	kUn3878, kUn388F, kUn387B, kUn388F, kUn387F, kUn388F, kUn3884, kUn388F, kUn388F, kUnToWait, kUn38A4, kUn389C, kUn38B9, kUn38B1, NULL, kUnToWait,
	NULL, kUn38CE, NULL, kUn38CE, NULL, kUn38CE, kUn38C6, kUn38CE, kUn38CE, kUnToWait, kUn38D9, kUn38D9, NULL, kUn38F1, NULL, kUnToWait,
	kUn3878, kUn38D1, kUn387B, kUn38D1, NULL, kUn38D1, kUn38CB, kUn38D1, kUn38D1, kUnToWait, NULL, kUn38E0, kUn38EA, kUn38EA, NULL, kUnToWait,
	NULL, kUn38D6, NULL, kUn38D6, NULL, kUn38D6, kUn388A, kUn38D6, kUn38D6, kUnToWait, kUn38AA, kUn38AA, NULL, kUn38F6, NULL, kUnToWait,
	NULL, kUn3897, NULL, kUn3897, NULL, kUn3897, NULL, kUn3897, kUn3897, kUnToWait, NULL, kUn38E5, kUn38BF, kUn38BF, NULL, kUnToWait,
};

// 38FB: n words from `from` copied to from + delta (7 instructions each), with the caller's 4
inline void Move (uint32 from, int32 delta, uint32 n)
{
	c += 4;
	for (uint32 i = 0; i < n; i++)
	{
		s.tr = RdW(from + i);
		c += 6; WrW(from + i + delta, s.tr);
		c += 1;
	}
	c += 1;
	s.a = (uint16) (from + n);
	s.trb = (uint16) delta;
}

// 4 instructions: the word at `from` copied to `to`
inline void Word (uint32 from, uint32 to)
{
	c += 4; WrW(to, RdW(from));
}

inline void Clear0 (uint32 at)
{
	c += 2; WrW(at, 0);
}

// a white record's 3 words (gain, from, to) over the first
inline void WhiteFirst (uint32 rec)
{
	Word(rec, 0x6c3);
	Word(rec + 2, 0x6c5);
	Word(rec + 3, 0x6c6);
}

// 3903: the move in record 1 ($6DF -> $6E0) against the lists: the gain records it disturbs go, white's best capture
// left over, then the outcome: $99 = its K + L, $9A / $9B what each side takes next, $9C the score's margin
void Outcome (void)
{
	LAB_ENTER(0x3903);
	c += 1;
	Clear0(0x99); Clear0(0x9a); Clear0(0x9b); Clear0(0x9c);
	c += 4;
	s.k = RdW(0x6df);
	s.l = RdW(0x6e0);
	c += 4;
	if (s.k == RdW(0x6f2) || (c += 4, s.l == RdW(0x6f3)))
	{
		Clear0(0x6f1); Clear0(0x6f2); Clear0(0x6f3);
	}
	c += 4;
	if (s.k == RdW(0x6ec) || (c += 4, s.l == RdW(0x6ed)))
	{
		c += 4;
		if (s.k == RdW(0x6ef) || (c += 4, s.l == RdW(0x6f0)))
		{
			Word(0x6f1, 0x6eb); Clear0(0x6ee); Clear0(0x6f1);
			Word(0x6f2, 0x6ec); Clear0(0x6ef); Clear0(0x6f2);
			Word(0x6f3, 0x6ed); Clear0(0x6f0); Clear0(0x6f3);
		}
		else
		{
			for (uint32 w = 0x6eb; w < 0x6f1; w++)
				Word(w + 3, w);
			Clear0(0x6f1); Clear0(0x6f2); Clear0(0x6f3);
			c += 1;
		}
	}
	else
	{
		c += 4;
		if (s.k == RdW(0x6ef) || (c += 4, s.l == RdW(0x6f0)))
		{
			Word(0x6f1, 0x6ee); Clear0(0x6f1);
			Word(0x6f2, 0x6ef); Clear0(0x6f2);
			Word(0x6f3, 0x6f0); Clear0(0x6f3);
			c += 1;
		}
	}

	c += 3;
	if (!s.k && (c += 3, !s.l))
	{
		// no move: white's first record gives way to the next on another line
		c += 4;
		s.a = RdW(0x6c7);
		if (!s.a)
		{
			Word(0x6c2, 0x6c3);
			c += 3;
			if (!(RdW(0x6c2) < RdW(0x6c8)))
				goto outcome;
			WhiteFirst(0x6c8);
			c += 1;
			goto outcome;
		}
		c += 6;
		s.k = RdW(0x6c5);
		if (s.k != RdW(0x6ca) || (c += 4, s.a != RdW(0x6cc)))
		{
			WhiteFirst(0x6c8);
			c += 1;
		}
		else if (c += 4, s.k != RdW(0x6cf) || (c += 4, s.a != RdW(0x6d1)))
		{
			WhiteFirst(0x6cd);
			c += 1;
		}
		else
		{
			Clear0(0x6c3); Clear0(0x6c5); Clear0(0x6c6);
			c += 1;
		}
		goto outcome;
	}

	// a move K -> L: a white record whose line it lands on goes, one whose target it moves keeps only its bound, and
	// the best left becomes the first
	c += 6;
	if (RdW(0x1a4 + s.l) & 1)
	{
		Clear0(0x6c3); Clear0(0x6c5); Clear0(0x6c6);
	}
	else
	{
		c += 4;
		if (s.k != RdW(0x6c6))
			goto outcome;
		Word(0x6c4, 0x6c3); Clear0(0x6c5); Clear0(0x6c6);
		c += 1;
	}
	c += 6;
	if (!(RdW(0x1a4 + s.l) & 2))
	{
		c += 4;
		if (s.k == RdW(0x6cb))
		{
			Word(0x6c9, 0x6c8); Clear0(0x6ca); Clear0(0x6cb);
		}
		c += 5;
		if (RdW(0x6c3) < RdW(0x6c8))
			WhiteFirst(0x6c8);
	}
	c += 6;
	if (!(RdW(0x1a4 + s.l) & 4))
	{
		c += 4;
		if (s.k == RdW(0x6d0))
		{
			Word(0x6ce, 0x6cd); Clear0(0x6cf); Clear0(0x6d0);
		}
		c += 5;
		if (RdW(0x6c3) < RdW(0x6cd))
			WhiteFirst(0x6cd);
	}

outcome:
	// 3A22: $7D4 = the score, $7D2 = K, $99 = K + L; $7D0 = black's best gain less the first not touched by white's
	// best capture
	Word(0x6dc, 0x7d4);
	Word(0x6dd, 0x7d2);
	c += 4; WrW(0x99, (uint16) (RdW(0x6dd) + RdW(0x6de)));
	c += 4;
	s.k = RdW(0x6c5);
	s.l = RdW(0x6c6);
	c += 4;
	s.a = RdW(0x6eb);
	WrW(0x7d0, s.a);
	{
		int	i = 0;
		for (;;)
		{
			const uint32	rec = 0x6eb + i * 3;
			if (i)
			{
				c += 2;
				s.a = RdW(rec);
			}
			c += 4;
			if (s.l != RdW(rec + 1))
			{
				c += 4;
				if (s.k != RdW(rec + 2))
					break;
			}
			if (++i == 3)
			{
				c += 1;
				s.a = 0;
				break;
			}
		}
	}
	c += 4; WrW(0x7d0, (uint16) (RdW(0x7d0) - s.a));
	Word(0x6c3, 0x7d6);
	c += 5;
	if ((uint16) (RdW(0x6c3) + RdW(0x7d0)) < RdW(0x7d2))
	{
		c += 6; WrW(0x9c, (uint16) (RdW(0x7d4) - RdW(0x7d6)));
		Word(0x7d2, 0x6c3); Clear0(0x6c5); Clear0(0x6c6);
		c += 1;
	}
	else
	{
		c += 6; WrW(0x9c, (uint16) (RdW(0x7d4) - RdW(0x7d2)));
		c += 4;
		s.k = RdW(0x6c5);
		s.l = RdW(0x6c6);
		c += 4;
		if (s.l == RdW(0x6ec) || (c += 4, s.k == RdW(0x6ed)))
		{
			Word(0x6c3, 0x9a);
			c += 4;
			if (s.l == RdW(0x6ef) || (c += 4, s.k == RdW(0x6f0)))
			{
				c += 4;
				if (s.l == RdW(0x6f2) || (c += 4, s.k == RdW(0x6f3)))
				{
					c += 1;
					return;
				}
				Word(0x6f1, 0x9b);
				c += 1;
				return;
			}
			Word(0x6ee, 0x9b);
			c += 1;
			return;
		}
	}

	// 3A9F: black's best gain against white's next, with black's next gain not from the same square in $7D4
	c += 2;
	s.l = RdW(0x6ed);
	Word(0x6c3, 0x7d0);
	Word(0x6eb, 0x7d2);
	c += 2;
	s.a = RdW(0x6ee);
	c += 4;
	if (s.l == RdW(0x6f0))
	{
		c += 2;
		s.a = RdW(0x6f1);
		c += 4;
		if (s.l == RdW(0x6f3))
		{
			c += 1;
			s.a = 0;
		}
	}
	c += 2; WrW(0x7d4, s.a);
	c += 4;
	const uint16	g = RdW(0x7d2);
	if (g < s.a || (c += 3, (uint16) (g - s.a) < RdW(0x7d0)))
	{
		Word(0x7d0, 0x9a);
		Word(0x7d2, 0x9b);
		c += 1;
		return;
	}
	Clear0(0x9a);
	Word(0x7d4, 0x9b);
	c += 1;
}

// 385C: K off the map at $5BC on square A + off
inline void Unmark (int off)
{
	const uint32	at = (uint32) (0x5bc + off + s.a);
	c += 5; WrW(at, (uint16) (RdW(at) - s.k));
}

// 3861: a ray from A along dir, through what data ROM tbl[type] lets pass
void UnmarkRay (int dir, uint32 tbl)
{
	c += 1;
	s.l = s.a;
	for (;;)
	{
		c += 3;
		s.a = (uint16) (s.a + dir);
		Unmark(0);
		c += 4;
		const uint16	v = RdW(s.a);
		if (!v)
			continue;
		c += 1;
		if (v & 0x8000)
			break;
		c += 8;
		const uint16	pass = (uint16) (Rom(tbl + ((v & 0x3e) >> 1)) - 1);
		if (pass & 0x8000)
			break;
		c += 1;
		if (pass)
			continue;
		c += 3;
		s.a = (uint16) (s.a + dir);
		Unmark(0);
		break;
	}
	c += 2;
	s.a = s.l;
}

// a 16-bit difference's sign: the firmware's JS0B after SUB
inline bool Below (uint16 x, uint16 y) { return (((uint16) (x - y)) & 0x8000) != 0; }

// FALSE: the chip went back to its command wait
bool BlackChoice (void)
{
	LAB_ENTER(0x37a4);
	c += 1;
	Move(0x6dc, 0x18, 0x18);
	Move(0x6c2, 0x4a, 0x1a);
	Outcome();
	Move(0x99, 0x74e, 4);
	c += 4;
	if (!(RdW(0x6e1) & 0x8000) && (c += 1, RdW(0x6e1)))
	{
		Move(0x6f4, -0x18, 0x18);
		Move(0x70c, -0x4a, 0x1a);
		Move(0x6e1, -5, 5);
		Outcome();
		Move(0x99, 0x752, 4);
		c += 4;
		if (!(RdW(0x6e6) & 0x8000) && (c += 1, RdW(0x6e6)))
		{
			Move(0x6f4, -0x18, 0x18);
			Move(0x70c, -0x4a, 0x1a);
			Move(0x6e6, -10, 5);
			Outcome();
			Move(0x99, 0x756, 4);
		}
	}
	Move(0x6f4, -0x18, 0x18);
	Move(0x70c, -0x4a, 0x1a);
	c += 8; WrW(0x7d0, (uint16) (RdW(0x7e7) - RdW(0x7e8) + RdW(0x7e9)));
	c += 8; WrW(0x7d2, (uint16) (RdW(0x7eb) - RdW(0x7ec) + RdW(0x7ed)));
	c += 8; WrW(0x7d4, (uint16) (RdW(0x7ef) - RdW(0x7f0) + RdW(0x7f1)));

	// the best of the three: the first unless a later one's sum beats it. zb is B's zero flag from the last test,
	// which 3846's JZB reads after its AND on A
	int		best = 1;
	bool	zb;
	c += 4;
	const uint16	r2 = RdW(0x6e1), r3 = RdW(0x6e6);
	if ((r2 & 0x8000) || (c += 1, !r2))
		zb = !r2;
	else
	{
		c += 7;
		if (Below(RdW(0x7d0), RdW(0x7d2)))
			best = 2;
		const uint16	lead = RdW(best == 2 ? 0x7d2 : 0x7d0);
		c += 4;
		if ((r3 & 0x8000) || (c += 1, !r3))
			zb = !r3;
		else
		{
			c += 7;
			const uint16	d = (uint16) (lead - RdW(0x7d4));
			if (d & 0x8000)
				best = 3;
			zb = !d;
		}
	}
	if (best == 1)
	{
		Word(0x7e7, 0x99); Word(0x7e8, 0x9a); Word(0x7e9, 0x9b); Word(0x7ea, 0x9c);
		c += 2 + 1;
		s.a = RdW(0x6e0);
	}
	else if (best == 2)
	{
		Word(0x7eb, 0x99); Word(0x7ec, 0x9a); Word(0x7ed, 0x9b); Word(0x7ee, 0x9c);
		c += 2;
		s.a = RdW(0x6e5);
	}
	else
		s.a = RdW(0x6ea);

	// 3846: a white piece on the target loses its attacks
	c += 2;
	if (zb) { c += 1; return (true); }
	c += 4;
	if (!(RdW(s.a) & 0x20)) { c += 1; return (true); }
	c += 15;
	const uint32	idx = (uint32) (((RdW(s.a) & 0x1e) | RdW(0x9e + s.a)) >> 1);
	s.tr = 0x60e;
	s.k = 0x100;
	const UOp		*p = idx < 80 ? kUnTable[idx] : kUnToWait;
	if (idx >= 80)
		s.unknown = 1;
	if (p == kUnToWait)
		return (false);
	if (p)
		for (; p->op; p++)
			if (p->op == U_STEP)
			{
				c += 2;
				Unmark(p->off);
			}
			else
			{
				c += 3;
				s.tr = (uint16) (int16) p->off;
				s.trb = p->tbl;
				UnmarkRay(p->off, p->tbl);
			}
	c += 1;
	return (true);
}

// ---- 1295: the kings' shelters. Each of a king's 8 neighbours (black's at $93, white's at $94) adds a data ROM
// value ($65E ahead, $673 beside, $688 behind) by its attack balance (clamped 0-6, +7 on its own piece, +14 on the
// enemy's) to $7F3 / $7F4, and room ($7F5 / $7F6, from 4 in check or 14) if it's neither own nor attacked.

struct KBlock { int8 d; uint16 tbl; uint8 inc; };
const KBlock	kShelter[2][8] =
{
	{ { -11, 0x65e, 7 }, { -10, 0x65e, 7 }, { -9, 0x65e, 7 }, { -1, 0x673, 5 }, { 1, 0x673, 5 }, { 9, 0x688, 3 }, { 10, 0x688, 3 }, { 11, 0x688, 3 } },
	{ { 11, 0x65e, 7 }, { 10, 0x65e, 7 }, { 9, 0x65e, 7 }, { 1, 0x673, 5 }, { -1, 0x673, 5 }, { -9, 0x688, 3 }, { -10, 0x688, 3 }, { -11, 0x688, 3 } },
};

void Shelter (bool white)
{
	const uint32	score = white ? 0x7f4 : 0x7f3, room = white ? 0x7f6 : 0x7f5;
	const uint16	own = white ? 0x20 : 0x40, enemy = white ? 0x00ff : 0xff00;
	c += 9;
	s.trb = RdW(white ? 0x94 : 0x93);
	uint16	start = 4;
	if (!(RdW(0x5bc + s.trb) & enemy)) { c += 1; start = 14; }
	if (white) s.l = start; else s.k = start;
	c += 2; WrW(room, start);
	for (int i = 0; i < 8; i++)
	{
		const KBlock	&b = kShelter[white][i];
		const uint32	sq = (uint32) (s.trb + b.d);
		c += 5;
		if (RdW(sq) & 0x8000)
			continue;
		const uint16	m = RdW(0x5bc + sq);
		const int		bal = white ? (m & 0xff) - (m >> 8) : (m >> 8) - (m & 0xff);
		c += 11;
		s.tr = 3;
		uint16			a = (uint16) (bal + 3);
		if (a & 0x8000) { c += 1; a = 0; }
		c += 4;
		s.tr = 6;
		if (!(a < 6)) { c += 1; a = 6; }
		c += 5;
		const uint16	piece = RdW(sq);
		if (piece)
		{
			c += 5;
			s.tr = 7;
			a += 7;
			if (!(piece & own)) { c += 1; a += 7; }
		}
		s.a = a;
		c += 7; WrW(score, (uint16) (RdW(score) + Rom(b.tbl + a)));
		c += 6;
		if (piece & own)
			continue;
		c += 6;
		if (m & enemy)
			continue;
		c += 4; WrW(room, (uint16) (RdW(room) + b.inc));
	}
}

// $9D = 8 x white's shelter / room less the same for black (data ROM $000-$010: 32768 / n)
bool Shelters (void)
{
	c += 1;
	c += 2; WrW(0x7f3, 0);
	c += 2; WrW(0x7f4, 0);
	Shelter(false);
	Shelter(true);
	uint16	m, n, r;
	c += 6;
	s.k = (uint16) (RdW(0x7f4) << 3);
	c += 6;
	s.tr = 0x10;
	r = RdW(0x7f6);
	if (!(r < 0x10)) { c += 1; r = 0x10; }
	s.l = Rom(r);
	MulMN(s.k, s.l, m, n);
	c += 8; WrW(0x9d, m);
	c += 6;
	s.k = (uint16) (RdW(0x7f3) << 3);
	c += 6;
	r = RdW(0x7f5);
	if (!(r < 0x10)) { c += 1; r = 0x10; }
	s.l = Rom(r);
	MulMN(s.k, s.l, m, n);
	s.a = (uint16) (RdW(0x9d) - m);
	c += 9; WrW(0x9d, s.a);
	c += 1;
	return (true);
}

// ---- 012A: the result. $99-$9B less one (not below 0); $96 = 2 x old $97 + 16 x ($99 - $9A + $9B) + $9D + $98 / 24
// - $9C / 2 through the multiplier, $97 the same less 2 x $9C. 0176 ($0F) turns both round after.
inline uint16 MulM (uint16 k, uint16 l) { uint16 m, n; MulMN(k, l, m, n); return (m); }
inline uint16 MulN (uint16 k, uint16 l) { uint16 m, n; MulMN(k, l, m, n); return (n); }

void Score (void)
{
	c += 1;
	c += 1;
	s.tr = 1;
	for (uint32 w = 0x99; w <= 0x9b; w++)
	{
		c += 4;
		s.a = (uint16) (RdW(w) - 1);
		if (!RdW(w)) { c += 1; s.a = 0; }
		c += 1; WrW(w, s.a);
	}
	c += 1;
	s.a = 0;
	static const struct { uint16 at, l; } kTerms[4] = { { 0x97, 2 }, { 0x99, 0x10 }, { 0x9a, 0xfff0 }, { 0x9b, 0x10 } };
	for (int i = 0; i < 4; i++)
	{
		c += 7;
		s.k = RdW(kTerms[i].at);
		s.l = kTerms[i].l;
		s.a = (uint16) (s.a + Shr1(MulN(s.k, s.l)));
	}
	c += 2;
	s.a = (uint16) (s.a + RdW(0x9d));
	c += 6;
	s.k = RdW(0x98);
	s.l = 0x555;
	s.a = (uint16) (s.a + MulM(s.k, s.l));
	c += 1;
	s.tr = s.a;
	c += 6;
	s.k = RdW(0x9c);
	s.l = 0xc000;
	s.a = (uint16) (s.a + MulM(s.k, s.l));
	c += 2; WrW(0x96, s.a);
	c += 1;
	s.a = s.tr;
	c += 7;
	s.l = 0xfffe;
	s.a = (uint16) (s.a + Shr1(MulN(s.k, s.l)));
	c += 2; WrW(0x97, s.a);
	c += 1;
}

void ScoreTurned (void)
{
	c += 1;
	Score();
	c += 4; WrW(0x96, (uint16) -RdW(0x96));
	c += 3; WrW(0x97, (uint16) -RdW(0x97));
}

// The whole evaluation, from the call the dispatch lands on. FALSE: the chip went back to its command wait.
bool Evaluate (bool turned)
{
	if (turned)
		Flip();
	KingRings();
	PawnFiles(0x7bd, 0x40);
	PawnFiles(0x7c6, 0x20);
	Pinners(false);
	Pinners(true);
	if (!AttackMaps())
		return (false);
	PieceValues();
	ValueLists();
	if (!Support(false) || !Support(true))
		return (false);
	Exchange();
	if (!Mobility(false) || !Mobility(true))
		return (false);
	Material();
	ValueLists();
	if (!Support(false) || !Support(true))
		return (false);
	Exchange2();
	if (!Threats(false) || !Threats(true))
		return (false);
	Exchange3();
	Control();
	if (!Captures())
		return (false);
	if (!BlackMoves())
		return (false);
	if (!BlackChoice())
		return (false);
	Shelters();
	if (turned)
		ScoreTurned();
	else
		Score();
	c += 1;		// LJMP to the wait
	return (true);
}
