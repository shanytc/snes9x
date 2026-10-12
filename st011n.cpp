/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native ST011 (Hayazashi Nidan Morita Shougi): the shogi chip's commands on its own
// clock, with no dump. The CPU sees the chip only through DR and SR while a command
// runs (the game reads and writes the shared RAM only between commands), so every
// DR and SR change happens at the instruction the firmware makes it, while the RAM
// work in between is done as it comes, its instructions owed in one go.

#include <stddef.h>
#include <string.h>
#include "snes9x.h"
#include "upd96050n.h"
#include "st011n_rom.h"

namespace {

// Saved as-is up to the events in use, so the layout is fixed.
struct State
{
	int32	line;		// where the program resumes (0: power-on)
	int32	sub;		// where a routine resumes, inside a command
	uint32	due;		// instructions owed before the next DR/SR event
	int32	i, j;		// loop counters
	uint16	a, b, tr, trb, k, l;
	uint16	dp, rp;
	uint16	cmd;
	uint16	v[7];
	uint8	unknown;	// a command the native chip doesn't do yet (the lab's alarm)
	uint8	merged;		// more DR/SR changes than ev[] holds: the last ones were merged (the lab's alarm)
	uint8	pad[2];
	// a compute command's DR and SR changes, played back at their instruction counts (an evaluation makes ~1100)
	uint32	total, played;
	uint32	nev;
	struct Ev { uint32 at; uint16 dr, sr; } ev[8192];
};
static_assert(offsetof(State, ev) == 56 + 12 && sizeof(State::Ev) == 8, "savestate layout");
const uint32	kEvMax = sizeof(State::ev) / sizeof(State::Ev);

State	s;

#include "upd96050np.h"

const uint16	SR_RQM = 0x8000, SR_DRS = 0x1000, SR_DRC = 0x0400, SR_FIXED = 0x907c;

// LD SR: RQM, DRS and the unused bits keep what they were
inline void SetSR (uint16 v) { *sr = (uint16) ((*sr & SR_FIXED) | (v & ~SR_FIXED)); }
// any write of DR raises RQM
inline void SetDR (uint16 v) { *dr = v; *sr |= SR_RQM; }
// DR as a source raises RQM too; DRNF doesn't
inline uint16 GetDR (void) { *sr |= SR_RQM; return (*dr); }

// ---- commands that only work on the RAM: done at once, their instructions counted into `c` and owed after

uint32	c;		// instructions the command in hand has taken
uint16	cdr, csr;	// DR and SR as it leaves them so far

// DR or SR changed by the instruction counted last: noted for playback
inline void Ev (uint16 ndr, uint16 nsr)
{
	if (ndr == cdr && nsr == csr)
		return;
	cdr = ndr;
	csr = nsr;
	if (s.nev < sizeof(s.ev) / sizeof(s.ev[0]))
		s.ev[s.nev++] = { c, ndr, nsr };
	else
	{
		s.ev[s.nev - 1].dr = ndr;
		s.ev[s.nev - 1].sr = nsr;
		s.merged = 1;
	}
}

inline void ToDR (uint16 v) { Ev(v, csr | SR_RQM); }			// X->DR
inline uint16 FromDR (void) { Ev(cdr, csr | SR_RQM); return (cdr); }	// DR->X

inline uint16 RdW (uint32 w)
{
	const uint8	*p = ram + ((w & 0x7ff) << 1);
	return ((uint16) (p[0] | (p[1] << 8)));
}

#ifdef UPD96050_LAB
}
void	(*S9xST011LabWrite) (uint32 at, uint32 word, uint16 v) = NULL;	// a compute command's RAM writes, by count
void	(*S9xST011LabEnter) (uint32 pc) = NULL;	// an evaluator routine starting (the firmware's address)
namespace {
#define LAB_ENTER(pc)	do { if (S9xST011LabEnter) S9xST011LabEnter(pc); } while (0)
#else
#define LAB_ENTER(pc)	do { } while (0)
#endif

inline void WrW (uint32 w, uint16 v)
{
	uint8	*p = ram + ((w & 0x7ff) << 1);
	p[0] = (uint8) v;
	p[1] = (uint8) (v >> 8);
#ifdef UPD96050_LAB
	if (S9xST011LabWrite)
		S9xST011LabWrite(c, w & 0x7ff, v);
#endif
}

// Clears the pin table: words $9E-$120.
void ClearPins (void)
{
	c += 3 + 3 * 0x83;
	for (uint32 w = 0x9e; w < 0x9e + 0x83; w++)
		WrW(w, 0);
	s.a = 0x9e + 0x83; s.b = 0; s.tr = 0; s.dp = 0x9e + 0x82;
}

// From the king at `kingw` along `dir`: the first piece; if it is ours (`own` bit), on to the next one, and if
// that is the slider `slider` (its type, the low two bits aside) or, for `alt`, that exact piece, the first one
// is pinned: its pin table word gets `mark`. `lead` is the instructions before the king's square is read.
void Pin (uint16 kingw, uint16 dir, uint16 own, uint16 slider, uint16 alt, uint16 mark, uint32 lead)
{
	s.tr = dir; s.trb = slider;
	c += lead + 3;
	s.dp = kingw;
	s.a = RdW(kingw);
	uint16	v;
	do
	{
		c += 4;
		s.a = (uint16) (s.a + dir);
		s.dp = s.a & 0x7ff;
		v = RdW(s.dp);
	} while (!v);
	c += 3;
	s.b = v & own;
	if (!s.b)
	{
		c += 1;
		return;
	}
	s.k = s.a;
	c += 2;
	do
	{
		c += 4;
		s.a = (uint16) (s.a + dir);
		s.dp = s.a & 0x7ff;
		v = RdW(s.dp);
	} while (!v);
	if (alt)
	{
		c += 3;
		if (v == alt)
			goto pinned;
	}
	c += 4;
	s.b = (uint16) ((v & 0xfc) - slider);
	if (s.b)
	{
		c += 1;
		return;
	}
pinned:
	c += 4 + 1;
	s.dp = (0x9e + s.k) & 0x7ff;
	s.b = 0;
	WrW(s.dp, mark);
}

// Commands 6 (black, king in word $93, own pieces $40) and 7 (white, $94, $20): the pinned pieces.
void Pins (bool white)
{
	ClearPins();
	c += 4;
	const uint16	kingw = white ? 0x94 : 0x93;
	s.dp = kingw;
	s.a = RdW(kingw);
	if (!s.a)
	{
		c += 1;		// RT
		return;
	}
	struct Ray { uint16 dir, slider, mark; };
	static const Ray	black[7] = { { 0xfff5, 0x34, 0x20 }, { 0xfff7, 0x34, 0x60 }, { 0xffff, 0x38, 0x80 }, { 0x0001, 0x38, 0x80 },
	                                 { 0x0009, 0x34, 0x60 }, { 0x000a, 0x38, 0x40 }, { 0x000b, 0x34, 0x20 } };
	static const Ray	whitep[7] = { { 0x000b, 0x54, 0x20 }, { 0x0009, 0x54, 0x60 }, { 0x0001, 0x58, 0x80 }, { 0xffff, 0x58, 0x80 },
	                                  { 0xfff7, 0x54, 0x60 }, { 0xfff6, 0x58, 0x40 }, { 0xfff5, 0x54, 0x20 } };
	const Ray		*rays = white ? whitep : black;
	const uint16	own = white ? 0x20 : 0x40;
	for (int i = 0; i < 7; i++)
	{
		s.l = rays[i].mark;
		Pin(kingw, rays[i].dir, own, rays[i].slider, 0, rays[i].mark, 3 + 1);
		if (i == 0)		// straight ahead: a rook or the lance ($24 / $44) behind the piece pins it
			Pin(kingw, white ? 0x000a : 0xfff6, own, 0x38, white ? 0x44 : 0x24, 0x40, 1 + 2);
	}
	c += 1;
}

// ---- commands 4 and 5: the squares each side attacks, in the map at words $121-$1A3 (one per board word $00-$82)

// One square a piece attacks: its map word gets the attacker, which waits in DR (LD B; ADD; SUB|DP; DR->MEM).
inline void Mark (int off)
{
	c += 4;
	const uint16	w = (uint16) (0x121 + off + s.a);
	s.dp = w & 0x7ff;
	s.b = 0;
	WrW(s.dp, FromDR());
}

// A sliding piece's ray from square A along `dir`: the next square gets the attacker, each square on past it the
// attacked flag, up to the first piece; the enemy king (`king`) there is checked, the square behind it marked too.
void Ray (uint16 dir, uint16 king, bool white)
{
	(void) white;
	s.tr = dir;
	c += 2;			// LD TR, LCALL
	c += 5;			// ADD A,TR; LD B; ADD B,A; SUB|DP; DR->MEM
	s.a = (uint16) (s.a + dir);
	s.dp = (0x121 + s.a) & 0x7ff;
	WrW(s.dp, FromDR());
	c += 4;			// A->DP; MEM->B; AND; JZB
	s.dp = s.a & 0x7ff;
	s.b = RdW(s.dp);
	bool	beyond;
	if (s.b)
	{
		c += 3;		// LD B; SUB B,RAM; JZB
		s.b = (uint16) (king - RdW(s.dp));
		if (s.b)
		{
			c += 1;	// RT TRB->A
			s.a = s.trb;
			return;
		}
		beyond = true;
	}
	else
	{
		do
		{
			c += 11;
			s.a = (uint16) (s.a + dir);
			s.dp = (0x121 + s.a) & 0x7ff;
			s.b = 0;
			WrW(s.dp, RdW(s.dp) | 0x8000);
			s.dp = s.a & 0x7ff;
			s.b = RdW(s.dp);
		} while (!s.b);
		c += 3;		// LD B; SUB B,RAM; JNZB
		s.b = (uint16) (king - RdW(s.dp));
		if (s.b)
		{
			c += 1;
			s.a = s.trb;
			return;
		}
		// the king, at the end of an open line: in check, from TRB along TR
		c += 7;
		s.k++;
		s.b = s.k;
		WrW(0x96, s.trb);
		WrW(0x97, s.tr);
		s.dp = 0x97;
		beyond = true;
	}
	if (beyond)
	{
		c += 8;		// ADD A,TR; LD B; ADD; SUB|DP; LD B; OR; B->MEM; RT TRB->A
		s.a = (uint16) (s.a + dir);
		s.dp = (0x121 + s.a) & 0x7ff;
		s.b = RdW(s.dp) | 0x8000;
		WrW(s.dp, s.b);
		s.a = s.trb;
	}
}

// One side's attacks. FALSE: a piece of type 9 sent the chip back to its command wait.
bool Attacks (bool white)
{
	const uint16	own = white ? 0x20 : 0x40;
	const uint16	kingw = white ? 0x93 : 0x94;		// the enemy king's square
	const uint16	king = white ? 0x5c : 0x3c;			// and the enemy king
	const int		fwd = white ? 10 : -10;				// a step forward

	c += 1;			// LCALL
	c += 5;
	WrW(0x96, 0); WrW(0x97, 0); s.k = 0;
	c += 4;
	s.dp = kingw;
	s.a = RdW(kingw);
	if (!s.a)
	{
		c += 1;		// RT
		return (true);
	}
	c += 3 + 3 * 0x83;
	for (uint32 w = 0x121; w < 0x121 + 0x83; w++)
		WrW(w, 0);
	s.b = 0; s.tr = 0; s.dp = 0x1a3;
	c += 1;
	for (s.a = 0x15; s.a != 0x6e; )
	{
		c += 4;
		s.dp = s.a;
		const uint16	piece = RdW(s.a);
		s.b = piece & own;
		if (s.b)
		{
			c += 3;			// LD B; OR|A->TRB; B->DR
			s.trb = s.a;
			s.b = 0x8000 | s.a;
			ToDR(s.b);
			c += 8;			// LD B; AND; SHR1; LD TR; ADD; B->RP; RO->SOL; JMPSO
			const uint16	type = (piece & 0x1e) >> 1;
			s.tr = white ? 0x6ac : 0x69d;
			s.b = (uint16) (type + s.tr);
			s.rp = s.b & 0x7ff;
			switch (type)
			{
				case 0:			// pawn
					Mark(fwd);
					c += 1;
					break;
				case 1: case 3: case 5: case 7: case 8:		// gold, and the promoted minor pieces
					if (white) { Mark(11); Mark(10); Mark(9); Mark(1); Mark(-1); Mark(-10); }
					else { Mark(-11); Mark(-10); Mark(-9); Mark(-1); Mark(1); Mark(10); }
					c += 1;
					break;
				case 2:			// lance
					Ray((uint16) fwd, king, white);
					c += 1;
					break;
				case 4:			// knight
					if (white) { Mark(21); Mark(19); }
					else { Mark(-21); Mark(-19); }
					c += 1;
					break;
				case 6:			// silver
					if (white) { Mark(11); Mark(10); Mark(9); Mark(-9); Mark(-11); }
					else { Mark(-11); Mark(-10); Mark(-9); Mark(9); Mark(11); }
					c += 1;
					break;
				case 11:		// horse: one step orthogonally, then the bishop's rays
					if (white) { Mark(10); Mark(1); Mark(-1); Mark(-10); }
					else { Mark(-10); Mark(-1); Mark(1); Mark(10); }
					// fall through
				case 10:		// bishop
					if (white) { Ray(11, king, white); Ray(9, king, white); Ray(0xfff7, king, white); Ray(0xfff5, king, white); }
					else { Ray(0xfff5, king, white); Ray(0xfff7, king, white); Ray(9, king, white); Ray(11, king, white); }
					c += 1;
					break;
				case 13:		// dragon: one step diagonally, then the rook's rays
					if (white) { Mark(11); Mark(9); Mark(-9); Mark(-11); }
					else { Mark(-11); Mark(-9); Mark(9); Mark(11); }
					// fall through
				case 12:		// rook
					if (white) { Ray(10, king, white); Ray(1, king, white); Ray(0xffff, king, white); Ray(0xfff6, king, white); }
					else { Ray(0xfff6, king, white); Ray(0xffff, king, white); Ray(1, king, white); Ray(10, king, white); }
					c += 1;
					break;
				case 14:		// king
					if (white) { Mark(11); Mark(10); Mark(9); Mark(1); Mark(-1); Mark(-9); Mark(-10); Mark(-11); }
					else { Mark(-11); Mark(-10); Mark(-9); Mark(-1); Mark(1); Mark(9); Mark(10); Mark(11); }
					c += 1;
					break;
				case 9:			// no such piece: the jump table sends the chip to its command wait
					return (false);
				default:		// type 15 runs into other commands' code: not native
					s.unknown = 1;
					return (false);
			}
		}
		c += 4;
		s.a++;
		s.b = (uint16) (0x6e - s.a);
	}

	// the enemy king attacked from next door: in check from that square
	c += 8;
	s.dp = kingw;
	const uint16	kpos = RdW(kingw);
	s.dp = (0x121 + kpos) & 0x7ff;
	s.b = 0;
	s.a = RdW(s.dp) & 0xff;
	if (s.a)
	{
		c += 5;
		WrW(0x96, s.a);
		s.k++;
		s.b = s.k;
		s.dp = 0x96;
	}
	c += 3;
	s.b = (uint16) (2 - s.k);
	if (s.b == 0)
	{
		c += 2;		// double check: word $96 = $FFFF
		WrW(0x96, 0xffff);
		s.dp = 0x96;
	}
	c += 1;			// RT
	return (true);
}

// The map's words byte-swapped, $1A3 down to $121.
void SwapMap (void)
{
	c += 1 + 2 + 5 * 0x83 + 1;		// LCALL; LD RP, LD A; the loop; RT
	for (uint32 w = 0x1a3; w > 0x120; w--)
	{
		const uint16	v = RdW(w);
		s.b = (uint16) ((v << 8) | (v >> 8));
		WrW(w, s.b);
	}
	s.a = 0; s.rp = 0x120; s.dp = 0x121;
}

// ---- command $0E: white's moves. From-squares (0-6: a piece from the hand) at words $2AB+, to-squares at $4B7+
// ($80 up: promoting); DR counts them, and word $96 gets the count at the end.

// A move from `from` to A (10 instructions, the RT included): LD; ADD|DR; SUB|DP; ->MEM; LD; ADD|DR; SUB|DP;
// DR->B; INC|A->MEM; B->DR.
inline void Add (uint16 from)
{
	const uint16	n = cdr;
	c += 2; FromDR();
	c += 6;
	WrW(0x2ab + n, from);
	WrW(0x4b7 + n, s.a);
	s.b = (uint16) (n + 1);
	c += 2; ToDR(s.b);
}

// A promoting move from TRB to A (12): LD; ADD|DR; SUB|DP; DR->B; INC|TRB->MEM; B->DR; LD; ADD|DR; SUB|DP; LD; ADD; ->MEM.
inline void AddPromo (void)
{
	const uint16	n = cdr;
	c += 2; FromDR();
	c += 4;
	WrW(0x2ab + n, s.trb);
	ToDR((uint16) (n + 1));
	c += 6;
	s.b = (uint16) (0x80 + s.a);
	WrW(0x4b7 + n, s.b);
}

// $F1's test word: the address's low byte, its complement above it
inline uint16 TestWord (uint16 a) { return ((uint16) (((~a & 0xff) << 8) | (a & 0xff))); }

inline uint16 Rom (uint32 i) { i &= 0x7ff; return (i < sizeof(kDataRom) / sizeof(kDataRom[0]) ? kDataRom[i] : 0); }

inline bool Blocked (void) { return ((RdW(s.a) & 0xa0) != 0); }	// our own piece or the edge ($A0)

// The steps and rays the generator's piece programs are made of, each with its caller's LD and HCALL.
enum { G_STEP = 1, G_STEPZ, G_STEPB, G_PAWN, G_KNIGHT, G_FREE, G_FWD, G_BACK, G_LANCE };
struct Op { uint8 op; int8 arg; };

// 3B0F: one step, plain
void GStep (uint16 off)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Blocked()) { c += 1; return; }
	Add(s.trb);
}

// 3B15 / 3B20: one step, also promoting into the zone (past word $50), going there (3B15) or leaving it (3B20)
void GStepZ (uint16 off, bool from)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Blocked()) { c += 1; return; }
	c += 3;
	if ((from ? s.trb : s.a) <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
	c += 1; Add(s.trb);
}

// 3B2B (pawn: in the zone only promoting) and 3B35 (knight: also plain short of the last two ranks)
void GPawn (uint16 off, bool knight)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Blocked()) { c += 1; return; }
	c += 3;
	if (s.a <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
	if (!knight)
		return;
	c += 3;
	if (s.a <= 0x5a) { Add(s.trb); return; }
	c += 1;
}

// 3B42: a ray, plain (the horse's and dragon's)
void GFree (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1; Add(s.trb);
	}
	c += 3;
	if (!Blocked()) { Add(s.trb); return; }
	c += 1;
}

// 3B4E: a ray forward, promoting only in the zone
void GFwd (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 3;
		if (s.a <= 0x50) { c += 1; Add(s.trb); }
		else { c += 1; AddPromo(); c += 1; }
	}
	c += 3;
	if (Blocked()) { c += 1; return; }
	c += 3;
	if (s.a <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
}

// 3B63: a ray back or sideways, every move promoting when the piece starts in the zone
void GBack (uint16 dir)
{
	s.tr = dir;
	c += 2 + 4;
	s.a = s.trb;
	const bool	zone = s.trb > 0x50;
	if (!zone)
		c += 1;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1;
		if (zone) AddPromo(); else Add(s.trb);
	}
	c += 3;
	if (Blocked()) { c += 1; return; }
	if (zone) AddPromo(); else Add(s.trb);
}

// 3B7C: the lance's ray, plain short of the zone, promoting in it and plain too short of the last two ranks
void GLance (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 3;
		if (s.a <= 0x50) { c += 1; Add(s.trb); continue; }
		c += 1; AddPromo();
		c += 3;
		if (s.a > 0x5a)
			continue;
		c += 1; Add(s.trb);
	}
	c += 3;
	if (Blocked()) { c += 1; return; }
	c += 3;
	if (s.a <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
	c += 3;
	if (s.a <= 0x5a) { Add(s.trb); return; }
	c += 1;
}

const int8	kKingSteps[8] = { 11, 10, 9, 1, -1, -9, -10, -11 };

// 3BED: the king's steps, onto squares black doesn't attack (the map at $121)
void GKing (void)
{
	for (int i = 0; i < 8; i++)
	{
		c += 1 + 5;
		s.a = (uint16) (s.trb + kKingSteps[i]);
		if (Blocked())
			continue;
		c += 5;
		if (RdW(0x121 + s.a))
			continue;
		c += 1; Add(s.trb);
	}
	c += 1;
}

// The generator's piece programs (data ROM $786: 16 piece types for each of the 5 pin marks)
const Op	gPawn[] = { { G_PAWN, 10 }, { 0, 0 } };
const Op	gGold[] = { { G_STEP, 11 }, { G_STEP, 10 }, { G_STEP, 9 }, { G_STEP, 1 }, { G_STEP, -1 }, { G_STEP, -10 }, { 0, 0 } };
const Op	gLance[] = { { G_LANCE, 10 }, { 0, 0 } };
const Op	gKnight[] = { { G_KNIGHT, 21 }, { G_KNIGHT, 19 }, { 0, 0 } };
const Op	gSilver[] = { { G_STEPZ, 11 }, { G_STEPZ, 10 }, { G_STEPZ, 9 }, { G_STEPB, -9 }, { G_STEPB, -11 }, { 0, 0 } };
const Op	gBishop[] = { { G_FWD, 11 }, { G_FWD, 9 }, { G_BACK, -9 }, { G_BACK, -11 }, { 0, 0 } };
const Op	gHorse[] = { { G_FREE, 11 }, { G_FREE, 9 }, { G_FREE, -9 }, { G_FREE, -11 },
					 { G_STEP, 10 }, { G_STEP, 1 }, { G_STEP, -1 }, { G_STEP, -10 }, { 0, 0 } };
const Op	gRook[] = { { G_FWD, 10 }, { G_BACK, 1 }, { G_BACK, -1 }, { G_BACK, -10 }, { 0, 0 } };
const Op	gDragon[] = { { G_FREE, 10 }, { G_FREE, 1 }, { G_FREE, -1 }, { G_FREE, -10 },
					  { G_STEP, 11 }, { G_STEP, 9 }, { G_STEP, -9 }, { G_STEP, -11 }, { 0, 0 } };
// pinned on the 11 diagonal, the file, the 9 diagonal, the rank
const Op	gGold1[] = { { G_STEP, 11 }, { 0, 0 } };
const Op	gSilver1[] = { { G_STEPZ, 11 }, { G_STEPB, -11 }, { 0, 0 } };
const Op	gBishop1[] = { { G_FWD, 11 }, { G_BACK, -11 }, { 0, 0 } };
const Op	gFree1[] = { { G_FREE, 11 }, { G_FREE, -11 }, { 0, 0 } };
const Op	gStep1[] = { { G_STEP, 11 }, { G_STEP, -11 }, { 0, 0 } };
const Op	gStep2[] = { { G_STEP, 10 }, { G_STEP, -10 }, { 0, 0 } };
const Op	gSilver2[] = { { G_STEPZ, 10 }, { 0, 0 } };
const Op	gRook2[] = { { G_FWD, 10 }, { G_BACK, -10 }, { 0, 0 } };
const Op	gFree2[] = { { G_FREE, 10 }, { G_FREE, -10 }, { 0, 0 } };
const Op	gGold3[] = { { G_STEP, 9 }, { 0, 0 } };
const Op	gSilver3[] = { { G_STEPZ, 9 }, { G_STEPB, -9 }, { 0, 0 } };
const Op	gBishop3[] = { { G_FWD, 9 }, { G_BACK, -9 }, { 0, 0 } };
const Op	gFree3[] = { { G_FREE, 9 }, { G_FREE, -9 }, { 0, 0 } };
const Op	gStep3[] = { { G_STEP, 9 }, { G_STEP, -9 }, { 0, 0 } };
const Op	gStep4[] = { { G_STEP, 1 }, { G_STEP, -1 }, { 0, 0 } };
const Op	gRook4[] = { { G_BACK, 1 }, { G_BACK, -1 }, { 0, 0 } };
const Op	gFree4[] = { { G_FREE, 1 }, { G_FREE, -1 }, { 0, 0 } };
const Op	gKingMark[] = { { 0, 0 } };		// stands for the king's own code
const Op	gToWait[] = { { 0, 0 } };		// stands for a jump to the command wait (types 9 and 15)

const Op	*const kGen[5][16] =
{
	{ gPawn, gGold, gLance, gGold, gKnight, gGold, gSilver, gGold, gGold, gToWait, gBishop, gHorse, gRook, gDragon, gKingMark, gToWait },
	{ NULL, gGold1, NULL, gGold1, NULL, gGold1, gSilver1, gGold1, gGold1, gToWait, gBishop1, gFree1, NULL, gStep1, NULL, gToWait },
	{ gPawn, gStep2, gLance, gStep2, NULL, gStep2, gSilver2, gStep2, gStep2, gToWait, NULL, gStep2, gRook2, gFree2, NULL, gToWait },
	{ NULL, gGold3, NULL, gGold3, NULL, gGold3, gSilver3, gGold3, gGold3, gToWait, gBishop3, gFree3, NULL, gStep3, NULL, gToWait },
	{ NULL, gStep4, NULL, gStep4, NULL, gStep4, NULL, gStep4, gStep4, gToWait, NULL, gStep4, gRook4, gFree4, NULL, gToWait },
};

// One white piece's moves (TRB = its square). FALSE: the chip went back to its command wait.
bool GenPiece (const Op *p)
{
	if (p == gToWait)
		return (false);
	if (p == gKingMark)
	{
		GKing();
		return (true);
	}
	for (; p->op; p++)
	{
		const uint16	arg = (uint16) (int16) p->arg;
		switch (p->op)
		{
			case G_STEP:	GStep(arg); break;
			case G_STEPZ:	GStepZ(arg, false); break;
			case G_STEPB:	GStepZ(arg, true); break;
			case G_PAWN:	GPawn(arg, false); break;
			case G_KNIGHT:	GPawn(arg, true); break;
			case G_FREE:	GFree(arg); break;
			case G_FWD:		GFwd(arg); break;
			case G_BACK:	GBack(arg); break;
			case G_LANCE:	GLance(arg); break;
		}
	}
	c += 1;		// HJMP to the loop
	return (true);
}

// 031C (white, $20, words $7C6+) and 02D8 (black, $40, $7BD+): a file's word = 1 where that side's pawn stands in
// it, else 0 (two pawns in a file is illegal)
void PawnFiles (uint32 base, uint16 pawn)
{
	c += 1 + 3;
	for (uint32 w = base; w < base + 9; w++)
	{
		c += 2; WrW(w, 1);
		c += 1;
	}
	c += 1;
	for (int f = 8; f >= 0; f--)
	{
		int	r;
		for (r = 0; r < 9; r++)
		{
			c += 6;
			if (RdW(0x15 + 10 * r + f) == pawn)
				break;
		}
		if (r == 9)
		{
			c += 4;
			WrW(base + f, 0);
		}
		c += 2;
	}
	c += 1;
	s.a = 0xffff;
}

// 3F6C: drops from white's hand (words $8B-$91: pawn, lance, knight, silver, gold, bishop, rook) onto empty squares
void Drops (void)
{
	c += 1 + 4;
	if (RdW(0x8b))
	{
		// pawns: files without one (word $7B1 + square), ranks 1-8 (word $7CF, the edge's file, is read too)
		c += 3;
		s.tr = 10; s.trb = 0x50;
		s.a = 0x15;
		do
		{
			c += 5;
			if (!RdW(0x7b1 + s.a))
			{
				do
				{
					c += 4;
					if (!RdW(s.a)) { c += 2; s.k = 0; Add(0); }
					c += 4;
					s.a = (uint16) (s.a + 10);
				} while (s.a <= 0x64);
				c += 1;
				s.a = (uint16) (s.a - 0x50);
			}
			c += 4;
			s.a++;
		} while (s.a <= 0x1e);
	}
	static const struct { uint16 w, last, k; } kHand[6] =
	{
		{ 0x8c, 0x64, 1 }, { 0x8d, 0x5a, 2 }, { 0x8e, 0x6e, 3 }, { 0x8f, 0x6e, 4 }, { 0x90, 0x6e, 5 }, { 0x91, 0x6e, 6 }
	};
	for (int i = 0; i < 6; i++)
	{
		c += 4;
		if (!RdW(kHand[i].w))
			continue;
		c += 1;
		s.a = 0x15;
		do
		{
			c += 4;
			if (!RdW(s.a)) { c += 2; s.k = kHand[i].k; Add(s.k); }
			c += 4;
			s.a++;
		} while (s.a <= kHand[i].last);
	}
	c += 1;
}

// ---- the evasions, when white's king is in check (word $96: the checking square, $FFFF for two)

// 3DAB: is TRB + off on the check (the map word's $200)?
inline bool OnCheck (uint16 off)
{
	c += 6;
	s.a = (uint16) (s.trb + off);
	return ((RdW(0x121 + s.a) & 0x200) != 0);
}

// 3E1F with its caller's LD TR and HCALL: is the first piece along `dir` the checking one?
inline bool RayToCheck (uint16 dir)
{
	s.tr = dir;
	c += 2 + 1;
	s.a = s.trb;
	do
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
	} while (!RdW(s.a));
	c += 5;
	return ((RdW(0x121 + s.a) & 0x200) != 0);
}

enum { E_PLAIN = 1, E_ZONE, E_ZONEB, E_PAWN, E_KNIGHT };

// 7D6's capture codes, after the JNZB that found the checking piece: the moves, then on to the next square
void Capture (int how)
{
	switch (how)
	{
		case E_PLAIN:
			Add(s.trb);
			c += 1;
			return;
		case E_ZONE:
		case E_ZONEB:
			c += 3;
			if ((how == E_ZONEB ? s.trb : s.a) > 0x50)
				AddPromo();
			Add(s.trb);
			c += 1;
			return;
		default:
			c += 3;
			if (s.a > 0x50)
			{
				AddPromo();
				c += 3;
				if (s.a > (how == E_PAWN ? 0x64 : 0x5a))
					return;
			}
			Add(s.trb);
			c += 1;
			return;
	}
}

// One piece's captures of a single checking piece (data ROM $7D6); its first capture ends it
struct Cap { uint8 ray; int8 arg; uint8 how; };
const Cap	cPawn[] = { { 0, 10, E_PAWN }, { 0, 0, 0 } };
const Cap	cGold[] = { { 0, 11, E_PLAIN }, { 0, 10, E_PLAIN }, { 0, 9, E_PLAIN }, { 0, 1, E_PLAIN }, { 0, -1, E_PLAIN }, { 0, -10, E_PLAIN }, { 0, 0, 0 } };
const Cap	cLance[] = { { 1, 10, E_PAWN }, { 0, 0, 0 } };
const Cap	cKnight[] = { { 0, 21, E_KNIGHT }, { 0, 19, E_KNIGHT }, { 0, 0, 0 } };
const Cap	cSilver[] = { { 0, 11, E_ZONE }, { 0, 10, E_ZONE }, { 0, 9, E_ZONE }, { 0, -9, E_ZONEB }, { 0, -11, E_ZONEB }, { 0, 0, 0 } };
const Cap	cBishop[] = { { 1, 11, E_ZONE }, { 1, 9, E_ZONE }, { 1, -9, E_ZONEB }, { 1, -11, E_ZONEB }, { 0, 0, 0 } };
const Cap	cHorse[] = { { 1, 11, E_PLAIN }, { 1, 9, E_PLAIN }, { 1, -9, E_PLAIN }, { 1, -11, E_PLAIN },
					 { 0, 10, E_PLAIN }, { 0, 1, E_PLAIN }, { 0, -1, E_PLAIN }, { 0, -10, E_PLAIN }, { 0, 0, 0 } };
const Cap	cRook[] = { { 1, 10, E_ZONE }, { 1, 1, E_ZONEB }, { 1, -1, E_ZONEB }, { 1, -10, E_ZONEB }, { 0, 0, 0 } };
const Cap	cDragon[] = { { 1, 10, E_PLAIN }, { 1, 1, E_PLAIN }, { 1, -1, E_PLAIN }, { 1, -10, E_PLAIN },
					  { 0, 11, E_PLAIN }, { 0, 9, E_PLAIN }, { 0, -9, E_PLAIN }, { 0, -11, E_PLAIN }, { 0, 0, 0 } };
const Cap	cNone[] = { { 0, 0, 0 } };		// the king: straight on
const Cap	cToWait[] = { { 0, 0, 0 } };
const Cap	cSlider[] = { { 0, 0, 0 } };	// type 15 lands on the pawn's code of the other table, in the other loop

const Cap	*const kCap[16] =
{
	cPawn, cGold, cLance, cGold, cKnight, cGold, cSilver, cGold, cGold, cToWait, cBishop, cHorse, cRook, cDragon, cNone, cSlider
};

enum { B_STEP = 1, B_STEPZ, B_STEPB, B_PAWN, B_KNIGHT, B_FREE, B_FWD, B_BACK, B_LANCE };

// 3E2A, 3E2E, 3E36, 3E3E, 3E48 (with the caller's LD A and HCALL): a step onto the check, capturing or blocking
void BStep (uint16 off, int kind)
{
	c += 1;		// the HCALL of 3DAB
	if (!OnCheck(off))
	{
		c += 2;
		return;
	}
	c += 1;
	if (kind == B_STEP)
	{
		c += 1; Add(s.trb);
		return;
	}
	c += 3;
	if ((kind == B_STEPB ? s.trb : s.a) <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
	if (kind == B_STEPZ || kind == B_STEPB)
	{
		c += 1; Add(s.trb);
		return;
	}
	c += 3;
	if (s.a <= (kind == B_PAWN ? 0x64 : 0x5a)) { Add(s.trb); return; }
	c += 1;
}

// 3E52, 3E5F, 3E71 (with the caller's LD TR and HCALL): a ray up to the check or the first piece
void BRay (uint16 dir, int kind)
{
	s.tr = dir;
	c += 2 + 1;
	s.a = s.trb;
	for (;;)
	{
		c += 7;
		s.a = (uint16) (s.a + dir);
		if (RdW(0x121 + s.a) & 0x200)
			break;
		c += 4;
		if (RdW(s.a)) { c += 1; return; }
	}
	if (kind == B_FREE) { Add(s.trb); return; }
	c += 3;
	if ((kind == B_BACK ? s.trb : s.a) <= 0x50) { Add(s.trb); return; }
	c += 1; AddPromo();
	c += 1; Add(s.trb);
}

// 3F04: the lance's ray up to the check (inline in its program, back to the loop itself)
void BLance (void)
{
	c += 2;
	s.tr = 10;
	s.a = s.trb;
	for (;;)
	{
		c += 7;
		s.a = (uint16) (s.a + 10);
		if (RdW(0x121 + s.a) & 0x200)
			break;
		c += 4;
		if (RdW(s.a)) { c += 2; return; }
	}
	c += 3;
	if (s.a > 0x50)
	{
		c += 1; AddPromo();
		c += 3;
		if (s.a > 0x64) { c += 1; return; }
	}
	c += 1; Add(s.trb);
	c += 1;
}

// One piece's captures and blocks of a ranging check (data ROM $7E5)
const Op	bPawn[] = { { B_PAWN, 10 }, { 0, 0 } };
const Op	bGold[] = { { B_STEP, 11 }, { B_STEP, 10 }, { B_STEP, 9 }, { B_STEP, 1 }, { B_STEP, -1 }, { B_STEP, -10 }, { 0, 0 } };
const Op	bKnight[] = { { B_KNIGHT, 21 }, { B_KNIGHT, 19 }, { 0, 0 } };
const Op	bSilver[] = { { B_STEPZ, 11 }, { B_STEPZ, 10 }, { B_STEPZ, 9 }, { B_STEPB, -9 }, { B_STEPB, -11 }, { 0, 0 } };
const Op	bBishop[] = { { B_FWD, 11 }, { B_FWD, 9 }, { B_BACK, -9 }, { B_BACK, -11 }, { 0, 0 } };
const Op	bHorse[] = { { B_FREE, 11 }, { B_FREE, 9 }, { B_FREE, -9 }, { B_FREE, -11 },
					 { B_STEP, 10 }, { B_STEP, 1 }, { B_STEP, -1 }, { B_STEP, -10 }, { 0, 0 } };
const Op	bRook[] = { { B_FWD, 10 }, { B_BACK, 1 }, { B_BACK, -1 }, { B_BACK, -10 }, { 0, 0 } };
const Op	bDragon[] = { { B_FREE, 10 }, { B_FREE, 1 }, { B_FREE, -1 }, { B_FREE, -10 },
					  { B_STEP, 11 }, { B_STEP, 9 }, { B_STEP, -9 }, { B_STEP, -11 }, { 0, 0 } };
const Op	bLance[] = { { B_LANCE, 0 }, { 0, 0 } };
const Op	bNone[] = { { 0, 0 } };

const Op	*const kBlock[16] =
{
	bPawn, bGold, bLance, bGold, bKnight, bGold, bSilver, bGold, bGold, gToWait, bBishop, bHorse, bRook, bDragon, bNone, gToWait
};

// One white piece's answers to a ranging check. FALSE: the chip went back to its command wait.
bool BlockPiece (const Op *p)
{
	if (p == gToWait)
		return (false);
	if (p == bNone)
		return (true);
	if (p == bLance)
	{
		BLance();
		return (true);
	}
	for (; p->op; p++)
	{
		const uint16	arg = (uint16) (int16) p->arg;
		if (p->op <= B_KNIGHT)
			c += 2, BStep(arg, p->op);
		else
			BRay(arg, p->op);
	}
	c += 1;		// HJMP to the loop
	return (true);
}

// The white pieces that may answer the check (not pinned), from square `from` on: 7D6's captures of a single
// checking piece, or 7E5's answers to a ranging one. 0: done; 1: back to the command wait; 2: type 15 in 7D6,
// which carries on in 7E5's loop from the next square.
int AnswerLoop (bool ranging, uint16 from)
{
	for (s.trb = from; s.trb != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.trb);
		if (piece & 0x20)
		{
			c += 5;
			if (!RdW(0x9e + s.trb))
			{
				c += 9;
				const int	type = (piece & 0x1e) >> 1;
				s.tr = ranging ? 0x7e5 : 0x7d6;
				if (ranging)
				{
					if (!BlockPiece(kBlock[type]))
						return (1);
				}
				else if (kCap[type] == cToWait)
					return (1);
				else if (kCap[type] == cSlider)
				{
					c += 2; BStep(10, B_PAWN);
					c += 1;
					return (2);
				}
				else
				{
					const Cap	*p;
					for (p = kCap[type]; p->how; p++)
					{
						const uint16	arg = (uint16) (int16) p->arg;
						bool			hit;
						if (p->ray)
							hit = RayToCheck(arg);
						else
						{
							c += 2;		// LD A, HCALL
							hit = OnCheck(arg);
						}
						c += 1;		// JNZB
						if (hit)
						{
							Capture(p->how);
							break;
						}
					}
					if (!p->how && kCap[type] != cNone)
						c += 1;
				}
			}
		}
		c += 6;
		s.trb++;
	}
	return (0);
}

// 3CB3: the king's steps, then captures of the checking piece, and for a ranging check the blocks and drops
// between. FALSE: the chip went back to its command wait.
bool Evasions (void)
{
	c += 1; ToDR(0);
	c += 4;
	WrW(0x2aa, 0); WrW(0x4b6, 0);
	c += 2;
	s.trb = RdW(0x94);
	for (int i = 0; i < 8; i++)
	{
		c += 2 + 5;
		s.a = (uint16) (s.trb + kKingSteps[i]);
		if (Blocked()) { c += 1; continue; }
		c += 5;
		if (RdW(0x121 + s.a)) { c += 1; continue; }
		c += 1; Add(s.trb);
	}
	c += 4;
	s.a = RdW(0x96);
	if (!(s.a & 0x8000))
	{
		c += 4;
		WrW(0x121 + s.a, 0x200);
		c += 4;
		int	how;
		if (!RdW(0x97))
		{
			c += 1;
			how = AnswerLoop(false, 0x15);
			if (how == 1)
				return (false);
			if (how == 0)
				goto done;
			c += 6;
			how = AnswerLoop(true, (uint16) (s.trb + 1));
		}
		else
		{
			// the line from the king back to the ranging piece is on the check too
			c += 4;
			s.a = RdW(0x94);
			s.tr = RdW(0x97);
			uint32	guard = 0;
			do
			{
				c += 9;
				s.a = (uint16) (s.a - s.tr);
				WrW(0x121 + s.a, 0x200);
				if (++guard > 0x800) { s.unknown = 1; return (false); }
			} while (!RdW(s.a));
			c += 1;
			how = AnswerLoop(true, 0x15);
		}
		if (how == 1)
			return (false);

		// drops onto the empty squares between
		c += 5;
		s.a = RdW(0x94);
		s.tr = RdW(0x97);
		uint32	guard = 0;
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a - s.tr);
			if (RdW(s.a))
				break;
			if (++guard > 0x800) { s.unknown = 1; return (false); }
			c += 3;
			if (s.a <= 0x64)
			{
				c += 3;
				if (s.a <= 0x5a)
				{
					c += 4;
					if (RdW(0x8d)) Add(2);
				}
				c += 4;
				if (RdW(0x8b))
				{
					c += 8;
					if (!RdW(0x7c6 + Rom(0xec + s.a)))
						Add(0);
				}
				c += 4;
				if (RdW(0x8c)) Add(1);
			}
			for (uint16 k = 3; k <= 6; k++)
			{
				c += 4;
				if (RdW(0x8b + k)) Add(k);
			}
		}
	}
done:
	c += 2; WrW(0x96, FromDR());
	c += 1 + 1;		// RT, LJMP to the wait
	return (true);
}

// The whole command, from the HCALL the dispatch lands on
void MoveGen (void)
{
	c += 1;
	PawnFiles(0x7c6, 0x20);
	c += 1;
	Pins(true);
	if (!Attacks(false))
		return;
	c += 4;
	if (RdW(0x96))
	{
		c += 1;
		Evasions();
		return;
	}
	c += 1; ToDR(0);
	c += 4;
	WrW(0x2aa, 0); WrW(0x4b6, 0);
	c += 1;
	for (s.trb = 0x15; s.trb != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.trb);
		if (piece & 0x20)
		{
			c += 14;
			const uint32	idx = ((piece & 0x1e) | RdW(0x9e + s.trb)) >> 1;
			s.tr = 0x786;
			const Op		*p = idx < 80 ? kGen[idx >> 4][idx & 15] : gToWait;
			if (idx >= 80)
				s.unknown = 1;
			if (p && !GenPiece(p))
				return;
		}
		c += 6;
		s.trb++;
	}
	Drops();
	c += 2; WrW(0x96, FromDR());
	c += 1 + 1;		// RT, LJMP to the wait
}

// ---- command $09: is white mated? Word $96 = 1 when it isn't in check, the check is a ranging one, the king has a
// free square, or a piece (not pinned) takes a single checking piece; else 0.

struct Hit { uint8 ray; int8 arg; };	// ray 2 ends a program
const Hit	mPawn[] = { { 0, 10 }, { 2, 0 } };
const Hit	mGold[] = { { 0, 11 }, { 0, 9 }, { 0, 1 }, { 0, -1 }, { 0, -10 }, { 0, 10 }, { 2, 0 } };
const Hit	mLance[] = { { 1, 10 }, { 2, 0 } };
const Hit	mKnight[] = { { 0, 21 }, { 0, 19 }, { 2, 0 } };
const Hit	mSilver[] = { { 0, 11 }, { 0, 10 }, { 0, 9 }, { 0, -9 }, { 0, -11 }, { 2, 0 } };
const Hit	mBishop[] = { { 1, 11 }, { 1, 9 }, { 1, -9 }, { 1, -11 }, { 2, 0 } };
const Hit	mHorse[] = { { 0, 10 }, { 0, 1 }, { 0, -1 }, { 0, -10 }, { 1, 11 }, { 1, 9 }, { 1, -9 }, { 1, -11 }, { 2, 0 } };
const Hit	mRook[] = { { 1, 1 }, { 1, -1 }, { 1, -10 }, { 1, 10 }, { 2, 0 } };
const Hit	mDragon[] = { { 0, 11 }, { 0, 9 }, { 0, -9 }, { 0, -11 }, { 1, 1 }, { 1, -1 }, { 1, -10 }, { 1, 10 }, { 2, 0 } };
const Hit	mNone[] = { { 2, 0 } };
const Hit	mToWait[] = { { 2, 0 } };
const Hit	mOther[] = { { 2, 0 } };		// type 15 lands in $0B's code

const Hit	*const kHit[16] =		// data ROM $6BB
{
	mPawn, mGold, mLance, mGold, mKnight, mGold, mSilver, mGold, mGold, mToWait, mBishop, mHorse, mRook, mDragon, mNone, mOther
};

// 1907: can a white piece (not pinned) take the checking piece on square K? 0: no, 1: yes, 2: back to the wait.
int TakesCheck (void)
{
	c += 3;
	s.k = RdW(0x96);
	for (s.a = 0x15; s.a != 0x6e; )
	{
		c += 4;
		const uint16	piece = RdW(s.a);
		if (piece & 0x20)
		{
			c += 5;
			if (!RdW(0x9e + s.a))
			{
				c += 10;
				s.trb = s.a;
				s.tr = 0x6bb;
				const Hit	*p = kHit[(piece & 0x1e) >> 1];
				if (p == mToWait)
					return (2);
				if (p == mOther)
				{
					s.unknown = 1;
					return (2);
				}
				if (p != mNone)
				{
					for (; p->ray != 2; p++)
					{
						const uint16	arg = (uint16) (int16) p->arg;
						if (!p->ray)
						{
							c += 4;
							if ((uint16) (s.a + arg) == s.k)
								return (1);
							continue;
						}
						s.tr = arg;
						c += 2;
						do
						{
							c += 4;
							s.a = (uint16) (s.a + arg);
						} while (!RdW(s.a));
						c += 2;
						if (s.a == s.k)
							return (1);
						c += 1;
						s.a = s.trb;
					}
					c += 1;		// LJMP to the loop
				}
			}
		}
		c += 4;
		s.a++;
	}
	return (0);
}

void Mated (void)
{
	c += 1 + 1;		// LCALL 19C8, LCALL 18A8
	Pins(true);
	if (!Attacks(false))
		return;
	c += 4;
	const uint16	chk = RdW(0x96);
	if (chk)
	{
		c += 1;
		if ((chk & 0x8000) || (c += 4, !RdW(0x97)))
		{
			c += 2;
			s.trb = RdW(0x94);
			int		i;
			for (i = 0; i < 8; i++)
			{
				c += 6;
				s.a = (uint16) (s.trb + kKingSteps[i]);
				if (Blocked())
					continue;
				c += 5;
				if (!RdW(0x121 + s.a))
					break;
			}
			if (i == 8)
			{
				c += 4;
				if (RdW(0x96) & 0x8000)
				{
					c += 3 + 1;
					WrW(0x96, 0);
					return;
				}
				c += 1;
				const int	r = TakesCheck();
				if (r == 2)
					return;
				c += 3 + 1 + 1;
				WrW(0x96, (uint16) r);
				return;
			}
		}
	}
	c += 3 + 1;
	WrW(0x96, 1);
}

// ---- commands $0B and $0C: black's checking moves, into the lists $0E fills ($0B: the direct checks, drops next to
// the king; $0C: discovered checks too, the pawn drop and drops along the lines)

// 1A98: from white's king along `dir`, each square up to the first piece gets `k` in the checking map ($1A4); if a
// black ranging piece `trb` (its type, the low two bits aside) stands behind that first piece, the piece's word in
// the discovery map ($227) gets `l`.
void CheckRay (uint16 dir, uint16 k, uint16 l, uint16 trb)
{
	s.tr = dir; s.k = k; s.l = l; s.trb = trb;
	c += 4 + 1 + 2;
	s.a = RdW(0x94);
	do
	{
		c += 8;
		s.a = (uint16) (s.a + dir);
		WrW(0x1a4 + s.a, k);
		s.b = RdW(s.a);
	} while (!s.b);
	c += 1;
	if (s.b & 0x8000) { c += 1; return; }
	c += 1; ToDR(s.a);
	do
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		s.b = RdW(s.a);
	} while (!s.b);
	c += 4;
	if ((s.b & 0xfc) != trb) { c += 1; return; }
	c += 2; FromDR();
	c += 2; WrW(0x227 + cdr, l);
	c += 1;
}

// 1AB2: the same straight down the file from the king ($42), the piece behind being black's lance ($44) or $38-$3B
void CheckFile (void)
{
	s.tr = 10; s.trb = 0x38;
	c += 1 + 4;
	s.a = RdW(0x94);
	do
	{
		c += 8;
		s.a = (uint16) (s.a + 10);
		WrW(0x1a4 + s.a, 0x42);
		s.b = RdW(s.a);
	} while (!s.b);
	c += 1;
	if (s.b & 0x8000) { c += 1; return; }
	c += 1; ToDR(s.a);
	c += 1;
	do
	{
		c += 4;
		s.a = (uint16) (s.a + 10);
		s.b = RdW(s.a);
	} while (!s.b);
	c += 3;
	if (s.b != 0x44)
	{
		c += 4;
		if ((s.b & 0xfc) != 0x38) { c += 1; return; }
	}
	c += 2; FromDR();
	c += 2; WrW(0x227 + cdr, 0x40);
	c += 1;
}

// 1A39: both maps, cleared, then the lines from white's king and the squares next to it
void CheckMaps (void)
{
	c += 1 + 2 * (3 + 3 * 0x83);
	for (uint32 w = 0x1a4; w < 0x1a4 + 2 * 0x83; w++)
		WrW(w, 0);
	s.tr = 0;
	c += 4;
	if (!RdW(0x94))
	{
		c += 1;
		return;
	}
	struct R { int16 dir; uint16 k, l, trb; };
	static const R	rays[7] =
	{
		{ 11, 0x20, 0x20, 0x54 }, { 9, 0x20, 0x60, 0x54 }, { 1, 0x40, 0x80, 0x58 }, { -1, 0x40, 0x80, 0x58 },
		{ -9, 0x20, 0x60, 0x54 }, { -10, 0x40, 0x40, 0x58 }, { -11, 0x20, 0x20, 0x54 }
	};
	for (int i = 0; i < 7; i++)
	{
		CheckRay((uint16) rays[i].dir, rays[i].k, rays[i].l, rays[i].trb);
		if (i == 0)
			CheckFile();
	}
	static const struct { int8 off; uint16 v; } kNext[10] =
	{
		{ 21, 0x04 }, { 19, 0x04 }, { 11, 0x38 }, { 9, 0x38 }, { 10, 0x5b }, { 1, 0x50 }, { -1, 0x50 }, { -10, 0x50 }, { -9, 0x28 }, { -11, 0x28 }
	};
	c += 2;
	s.a = RdW(0x94);
	for (int i = 0; i < 10; i++)
	{
		c += 4;
		WrW(0x1a4 + s.a + kNext[i].off, kNext[i].v);
	}
	c += 1;
}

// 1B74 / 1D75 and 1B79 / 1D7A: the checking map's word at A against the piece's bits (K) or its promoted bits (L)
inline bool ChecksK (void) { c += 5; return ((RdW(0x1a4 + s.a) & s.k) != 0); }
inline bool ChecksL (void) { c += 5; return ((RdW(0x1a4 + s.a) & s.l) != 0); }
// 1B7E / 1D7F: the move to A if it checks
inline void AddIfK (void)
{
	c += 6;
	if (RdW(0x1a4 + s.a) & s.k)
		Add(s.trb);
	else
		c += 1;
}
inline bool Own (void) { return ((RdW(s.a) & 0xc0) != 0); }	// black's own piece or the edge ($C0)
inline bool Zone (uint16 sq) { return (sq <= 0x32); }			// black promotes on words up to $32

enum { K_STEP = 1, K_STEPZ, K_PAWN, K_KNIGHT, K_FREE, K_FWD, K_BACK, K_LANCE, K_KING };
struct KOp { uint8 op; int8 arg; uint8 mark; };

// $0B's programs (data ROM $6CA)

// 1B85: a step that checks
void BStepK (uint16 off)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	AddIfK();
}

// 1B8B: a step, promoting when the promoted piece checks and the move touches the zone, then plain if that checks
void BStepZK (uint16 off)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (l)
	{
		c += 3;
		if (Zone(s.a) || (c += 3, Zone(s.trb)))
		{
			c += 1; AddPromo();
			c += 1;
		}
	}
	AddIfK();
}

// 1B9B (pawn) and 1BA7 (knight): only when the promoted piece checks; the pawn promotes in the zone, else plain if
// that checks; the knight promotes in the zone, else nothing
void BPawnK (uint16 off, bool knight)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (!l)
	{
		if (knight) AddIfK(); else c += 1;
		return;
	}
	c += 3;
	if (Zone(s.a)) { AddPromo(); return; }
	if (knight) { c += 1; return; }
	c += 1; AddIfK();
}

// 1BB2: a ray, plain, each square that checks
void BFreeK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1; AddIfK();
	}
	c += 3;
	if (!Own()) { AddIfK(); return; }
	c += 1;
}

// 1BBE: a ray towards the zone: plain outside it, promoting in it when the promoted piece checks
void BFwdK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 3;
		if (!Zone(s.a)) { c += 1; AddIfK(); continue; }
		c += 1;
		const bool	l = ChecksL();
		c += 1;
		if (l) { c += 1; AddPromo(); c += 1; }
	}
	c += 3;
	if (Own()) { c += 1; return; }
	c += 3;
	if (!Zone(s.a)) { AddIfK(); return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (l) { AddPromo(); return; }
	c += 1;
}

// 1BD6: a ray away from the zone: from inside it every move promotes (when that checks); from outside, plain, the
// first checking square ending it
void BBackK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 4;
	s.a = s.trb;
	if (!Zone(s.trb))
	{
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 1;
			const bool	k = ChecksK();
			c += 1;
			if (k) { Add(s.trb); return; }
		}
		c += 3;
		if (!Own()) { AddIfK(); return; }
		c += 1;
		return;
	}
	c += 1;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1;
		const bool	l = ChecksL();
		c += 1;
		if (l) { c += 1; AddPromo(); }
	}
	c += 3;
	if (Own()) { c += 1; return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (l) { AddPromo(); return; }
	c += 1;
}

// 1BF7: the lance (its program, back to the loop itself): promoting along the empty squares in the zone, then the
// capture, promoting (in the zone, when that checks) and plain
void BLanceK (void)
{
	s.tr = 0xfff6;
	c += 3;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a - 10);
		if (RdW(s.a))
			break;
		c += 3;
		if (!Zone(s.a))
			continue;
		c += 1;
		const bool	l = ChecksL();
		c += 1;
		if (l) { c += 1; AddPromo(); }
	}
	c += 3;
	if (!Own())
	{
		c += 3;
		if (Zone(s.a))
		{
			c += 1;
			const bool	l = ChecksL();
			c += 1;
			if (l) { c += 1; AddPromo(); }
		}
		c += 1; AddIfK();
	}
	c += 1;
}

const KOp	kbPawn[] = { { K_PAWN, -10, 0 }, { 0, 0, 0 } };
const KOp	kbGold[] = { { K_STEP, -11, 0 }, { K_STEP, 10, 0 }, { K_STEP, -10, 0 }, { K_STEP, -9, 0 }, { K_STEP, -1, 0 }, { K_STEP, 1, 0 }, { 0, 0, 0 } };
const KOp	kbLance[] = { { K_LANCE, 0, 0 }, { 0, 0, 0 } };
const KOp	kbKnight[] = { { K_KNIGHT, -21, 0 }, { K_KNIGHT, -19, 0 }, { 0, 0, 0 } };
const KOp	kbSilver[] = { { K_STEPZ, -11, 0 }, { K_STEPZ, 11, 0 }, { K_STEPZ, -10, 0 }, { K_STEPZ, -9, 0 }, { K_STEPZ, 9, 0 }, { 0, 0, 0 } };
const KOp	kbBishop[] = { { K_FWD, -11, 0 }, { K_BACK, 11, 0 }, { K_FWD, -9, 0 }, { K_BACK, 9, 0 }, { 0, 0, 0 } };
const KOp	kbHorse[] = { { K_STEP, -10, 0 }, { K_STEP, -1, 0 }, { K_STEP, 1, 0 }, { K_STEP, 10, 0 },
						  { K_FREE, -11, 0 }, { K_FREE, 11, 0 }, { K_FREE, -9, 0 }, { K_FREE, 9, 0 }, { 0, 0, 0 } };
const KOp	kbRook[] = { { K_FWD, -10, 0 }, { K_BACK, 10, 0 }, { K_BACK, -1, 0 }, { K_BACK, 1, 0 }, { 0, 0, 0 } };
const KOp	kbDragon[] = { { K_STEP, -11, 0 }, { K_STEP, -9, 0 }, { K_STEP, 9, 0 }, { K_STEP, 11, 0 },
						   { K_FREE, -10, 0 }, { K_FREE, 10, 0 }, { K_FREE, -1, 0 }, { K_FREE, 1, 0 }, { 0, 0, 0 } };
const KOp	kbGold1[] = { { K_STEP, -11, 0 }, { 0, 0, 0 } };
const KOp	kbSilver1[] = { { K_STEPZ, -11, 0 }, { K_STEPZ, 11, 0 }, { 0, 0, 0 } };
const KOp	kbBishop1[] = { { K_FWD, -11, 0 }, { K_BACK, 11, 0 }, { 0, 0, 0 } };
const KOp	kbFree1[] = { { K_FREE, -11, 0 }, { K_FREE, 11, 0 }, { 0, 0, 0 } };
const KOp	kbStep1[] = { { K_STEP, -11, 0 }, { K_STEP, 11, 0 }, { 0, 0, 0 } };
const KOp	kbStep2[] = { { K_STEP, -10, 0 }, { K_STEP, 10, 0 }, { 0, 0, 0 } };
const KOp	kbSilver2[] = { { K_STEPZ, -10, 0 }, { 0, 0, 0 } };
const KOp	kbRook2[] = { { K_FWD, -10, 0 }, { K_BACK, 10, 0 }, { 0, 0, 0 } };
const KOp	kbFree2[] = { { K_FREE, -10, 0 }, { K_FREE, 10, 0 }, { 0, 0, 0 } };
const KOp	kbGold3[] = { { K_STEP, -9, 0 }, { 0, 0, 0 } };
const KOp	kbSilver3[] = { { K_STEPZ, -9, 0 }, { K_STEPZ, 9, 0 }, { 0, 0, 0 } };
const KOp	kbBishop3[] = { { K_FWD, -9, 0 }, { K_BACK, 9, 0 }, { 0, 0, 0 } };
const KOp	kbFree3[] = { { K_FREE, -9, 0 }, { K_FREE, 9, 0 }, { 0, 0, 0 } };
const KOp	kbStep3[] = { { K_STEP, -9, 0 }, { K_STEP, 9, 0 }, { 0, 0, 0 } };
const KOp	kbStep4[] = { { K_STEP, -1, 0 }, { K_STEP, 1, 0 }, { 0, 0, 0 } };
const KOp	kbRook4[] = { { K_BACK, -1, 0 }, { K_BACK, 1, 0 }, { 0, 0, 0 } };
const KOp	kbFree4[] = { { K_FREE, -1, 0 }, { K_FREE, 1, 0 }, { 0, 0, 0 } };
const KOp	kToWait[] = { { 0, 0, 0 } };

const KOp	*const kChkB[5][16] =
{
	{ kbPawn, kbGold, kbLance, kbGold, kbKnight, kbGold, kbSilver, kbGold, kbGold, kToWait, kbBishop, kbHorse, kbRook, kbDragon, NULL, kToWait },
	{ NULL, kbGold1, NULL, kbGold1, NULL, kbGold1, kbSilver1, kbGold1, kbGold1, kToWait, kbBishop1, kbFree1, NULL, kbStep1, NULL, kToWait },
	{ kbPawn, kbStep2, kbLance, kbStep2, NULL, kbStep2, kbSilver2, kbStep2, kbStep2, kToWait, NULL, kbStep2, kbRook2, kbFree2, NULL, kToWait },
	{ NULL, kbGold3, NULL, kbGold3, NULL, kbGold3, kbSilver3, kbGold3, kbGold3, kToWait, kbBishop3, kbFree3, NULL, kbStep3, NULL, kToWait },
	{ NULL, kbStep4, NULL, kbStep4, NULL, kbStep4, NULL, kbStep4, kbStep4, kToWait, NULL, kbStep4, kbRook4, kbFree4, NULL, kToWait },
};

// $0C's programs (data ROM $736): the same moves, each also added when the piece stands in a discovery line
// (word $7D1 = its mark there) and leaves it (a step's own line mark differs, `mark`)

// 1D7F-1D93 tail: a discovered check, the piece leaving its line
inline bool Leaves (uint16 mark) { c += 4; const uint16 m = RdW(0x7d1); if (!m) return (false); c += 2; return (m != mark); }

// 1D86: a step, plain
void CStepK (uint16 off, uint16 mark)
{
	s.tr = mark;
	c += 3 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	c += 1;
	const bool	k = ChecksK();
	c += 1;
	if (k || Leaves(mark)) { Add(s.trb); return; }
	c += 1;
}

// 1D94: a step that may promote
void CStepZK (uint16 off, uint16 mark)
{
	s.tr = mark;
	c += 3 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	if (Leaves(mark))
	{
		// discovered: promoting where it may, and plain
		c += 3;
		if (Zone(s.a) || (c += 3, Zone(s.trb)))
		{
			c += 1; AddPromo();
			c += 1;
		}
		Add(s.trb);
		return;
	}
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (l)
	{
		c += 3;
		if (Zone(s.a) || (c += 3, Zone(s.trb)))
		{
			c += 1; AddPromo();
			c += 1;
		}
	}
	AddIfK();
}

// 1DB2: the pawn
void CPawnK (uint16 off, uint16 mark)
{
	s.tr = mark;
	c += 3 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	if (Leaves(mark))
	{
		c += 3;
		if (!Zone(s.a)) { Add(s.trb); return; }
		c += 1; AddPromo();
		c += 3;
		if (s.a > 0x1e) { Add(s.trb); return; }
		c += 1;
		return;
	}
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (!l) { c += 1; return; }
	c += 3;
	if (!Zone(s.a)) { AddIfK(); return; }
	c += 1; AddPromo();
	c += 1; AddIfK();
}

// 1DCC: the knight
void CKnightK (uint16 off)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (Own()) { c += 1; return; }
	c += 4;
	if (RdW(0x7d1))
	{
		c += 3;
		if (!Zone(s.a)) { Add(s.trb); return; }
		c += 1; AddPromo();
		c += 3;
		if (s.a > 0x28) { Add(s.trb); return; }
		c += 1;
		return;
	}
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (!l) { AddIfK(); return; }
	c += 3;
	if (Zone(s.a)) { AddPromo(); return; }
	c += 1;
}

// 1DE3: a ray, plain; for a piece in a discovery line every move
void CFreeK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 5;
	s.a = s.trb;
	const bool	disc = RdW(0x7d1) != 0;
	if (!disc)
		c += 1;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1;
		if (disc) Add(s.trb); else AddIfK();
	}
	c += 3;
	if (Own()) { c += 1; return; }
	if (disc) Add(s.trb); else AddIfK();
}

// 1DFD: a ray towards the zone
void CFwdK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 5;
	s.a = s.trb;
	if (RdW(0x7d1))
	{
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 3;
			if (Zone(s.a)) { c += 1; AddPromo(); }
			c += 1; Add(s.trb);
		}
		c += 3;
		if (Own()) { c += 1; return; }
		c += 3;
		if (Zone(s.a)) { c += 1; AddPromo(); c += 1; }
		Add(s.trb);
		return;
	}
	c += 1;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 3;
		if (Zone(s.a))
		{
			c += 1;
			const bool	l = ChecksL();
			c += 1;
			if (!l)
				continue;
			c += 1; AddPromo();
		}
		c += 1; AddIfK();
	}
	c += 3;
	if (Own()) { c += 1; return; }
	c += 3;
	if (!Zone(s.a)) { AddIfK(); return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (!l) { c += 1; return; }
	c += 1; AddPromo();
	c += 1; AddIfK();
}

// 1E2C: a ray away from the zone
void CBackK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 4;
	s.a = s.trb;
	const bool	zone = Zone(s.trb);
	c += 4;
	const bool	disc = RdW(0x7d1) != 0;
	if (zone && !disc)
	{
		c += 1;
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 1;
			const bool	l = ChecksL();
			c += 1;
			if (!l)
				continue;
			c += 1; AddIfK();
			c += 1; AddPromo();
		}
		c += 3;
		if (Own()) { c += 1; return; }
		c += 1;
		bool	l = ChecksL();
		c += 1;
		if (!l) { c += 1; return; }
		c += 1;
		const bool	k = ChecksK();
		c += 1;
		if (!k) { AddPromo(); return; }
		c += 1; AddPromo();
		c += 1; Add(s.trb);
		return;
	}
	if (zone)
	{
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 1; AddPromo();
			c += 1; Add(s.trb);
		}
		c += 3;
		if (Own()) { c += 1; return; }
		c += 1; AddPromo();
		c += 1; Add(s.trb);
		return;
	}
	if (!disc)
	{
		c += 1;
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 1;
			const bool	k = ChecksK();
			c += 1;
			if (k) { Add(s.trb); return; }
		}
		c += 3;
		if (!Own()) { AddIfK(); return; }
		c += 1;
		return;
	}
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 1; Add(s.trb);
	}
	c += 3;
	if (!Own()) { Add(s.trb); return; }
	c += 1;
}

// 1E6C: the lance; one standing in the file's discovery line ($40) has no discovered moves
void CLanceK (uint16 dir)
{
	s.tr = dir;
	c += 2 + 5;
	s.a = s.trb;
	const uint16	m = RdW(0x7d1);
	if (m)
	{
		c += 3;
		if (m == 0x40) { c += 1; return; }
		for (;;)
		{
			c += 5;
			s.a = (uint16) (s.a + dir);
			if (RdW(s.a))
				break;
			c += 3;
			if (Zone(s.a))
			{
				c += 1; AddPromo();
				c += 3;
				if (s.a <= 0x1e) { c += 1; return; }
			}
			c += 1; Add(s.trb);
		}
		c += 3;
		if (Own()) { c += 1; return; }
		c += 3;
		if (!Zone(s.a)) { Add(s.trb); return; }
		c += 1; AddPromo();
		c += 3;
		if (s.a > 0x1e) { Add(s.trb); return; }
		c += 1;
		return;
	}
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		c += 3;
		if (!Zone(s.a))
			continue;
		c += 1;
		const bool	l = ChecksL();
		c += 1;
		if (l) { c += 1; AddPromo(); }
	}
	c += 3;
	if (Own()) { c += 1; return; }
	c += 3;
	if (!Zone(s.a)) { AddIfK(); return; }
	c += 1;
	const bool	l = ChecksL();
	c += 1;
	if (!l) { AddIfK(); return; }
	c += 1; AddPromo();
	c += 1; AddIfK();
}

// 1EA2: a king in a discovery line steps off it along `dir`, both ways, onto squares white doesn't attack
void CKingStep (uint16 dir)
{
	s.tr = dir;
	c += 2 + 1;
	for (int side = 0; side < 2; side++)
	{
		if (side)
			c += 1;
		c += 5;
		s.a = (uint16) (s.trb + (side ? -dir : dir));
		if (Own())
			continue;
		c += 5;
		if (RdW(0x121 + s.a))
			continue;
		c += 1; Add(s.trb);
	}
	c += 1;
}

// 1F26: the king (its program): when in a discovery line, off it along the other three
void CKingK (void)
{
	c += 4;
	if (!RdW(0x7d1))
		return;
	static const struct { uint16 mark, dir; } kOff[4] = { { 0x20, 11 }, { 0x40, 10 }, { 0x60, 9 }, { 0x80, 1 } };
	for (int i = 0; i < 4; i++)
	{
		c += 4;
		if (RdW(0x7d1) != kOff[i].mark)
			CKingStep(kOff[i].dir);
	}
	c += 1;
}

const KOp	kcPawn[] = { { K_PAWN, -10, 0x40 }, { 0, 0, 0 } };
const KOp	kcGold[] = { { K_STEP, -11, 0x20 }, { K_STEP, 10, 0x40 }, { K_STEP, -10, 0x40 }, { K_STEP, -9, 0x60 }, { K_STEP, -1, 0x80 }, { K_STEP, 1, 0x80 }, { 0, 0, 0 } };
const KOp	kcLance[] = { { K_LANCE, -10, 0 }, { 0, 0, 0 } };
const KOp	kcKnight[] = { { K_KNIGHT, -21, 0 }, { K_KNIGHT, -19, 0 }, { 0, 0, 0 } };
const KOp	kcSilver[] = { { K_STEPZ, -11, 0x20 }, { K_STEPZ, 11, 0x20 }, { K_STEPZ, -10, 0x40 }, { K_STEPZ, -9, 0x60 }, { K_STEPZ, 9, 0x60 }, { 0, 0, 0 } };
const KOp	kcBishop[] = { { K_FWD, -11, 0 }, { K_BACK, 11, 0 }, { K_FWD, -9, 0 }, { K_BACK, 9, 0 }, { 0, 0, 0 } };
const KOp	kcHorse[] = { { K_STEP, -10, 0x40 }, { K_STEP, -1, 0x80 }, { K_STEP, 1, 0x80 }, { K_STEP, 10, 0x40 },
						  { K_FREE, -11, 0 }, { K_FREE, 11, 0 }, { K_FREE, -9, 0 }, { K_FREE, 9, 0 }, { 0, 0, 0 } };
const KOp	kcRook[] = { { K_FWD, -10, 0 }, { K_BACK, 10, 0 }, { K_BACK, -1, 0 }, { K_BACK, 1, 0 }, { 0, 0, 0 } };
const KOp	kcDragon[] = { { K_STEP, -11, 0x20 }, { K_STEP, -9, 0x60 }, { K_STEP, 9, 0x60 }, { K_STEP, 11, 0x20 },
						   { K_FREE, -10, 0 }, { K_FREE, 10, 0 }, { K_FREE, -1, 0 }, { K_FREE, 1, 0 }, { 0, 0, 0 } };
const KOp	kcKing[] = { { K_KING, 0, 0 }, { 0, 0, 0 } };
const KOp	kcGold1[] = { { K_STEP, -11, 0x20 }, { 0, 0, 0 } };
const KOp	kcSilver1[] = { { K_STEPZ, -11, 0x20 }, { K_STEPZ, 11, 0x20 }, { 0, 0, 0 } };
const KOp	kcBishop1[] = { { K_FWD, -11, 0 }, { K_BACK, 11, 0 }, { 0, 0, 0 } };
const KOp	kcFree1[] = { { K_FREE, -11, 0 }, { K_FREE, 11, 0 }, { 0, 0, 0 } };
const KOp	kcStep1[] = { { K_STEP, -11, 0x20 }, { K_STEP, 11, 0x20 }, { 0, 0, 0 } };
const KOp	kcStep2[] = { { K_STEP, -10, 0x40 }, { K_STEP, 10, 0x40 }, { 0, 0, 0 } };
const KOp	kcSilver2[] = { { K_STEPZ, -10, 0x40 }, { 0, 0, 0 } };
const KOp	kcRook2[] = { { K_FWD, -10, 0 }, { K_BACK, 10, 0 }, { 0, 0, 0 } };
const KOp	kcFree2[] = { { K_FREE, -10, 0 }, { K_FREE, 10, 0 }, { 0, 0, 0 } };
const KOp	kcGold3[] = { { K_STEP, -9, 0x60 }, { 0, 0, 0 } };
const KOp	kcSilver3[] = { { K_STEPZ, -9, 0x60 }, { K_STEPZ, 9, 0x60 }, { 0, 0, 0 } };
const KOp	kcBishop3[] = { { K_FWD, -9, 0 }, { K_BACK, 9, 0 }, { 0, 0, 0 } };
const KOp	kcFree3[] = { { K_FREE, -9, 0 }, { K_FREE, 9, 0 }, { 0, 0, 0 } };
const KOp	kcStep3[] = { { K_STEP, -9, 0x60 }, { K_STEP, 9, 0x60 }, { 0, 0, 0 } };
const KOp	kcStep4[] = { { K_STEP, -1, 0x80 }, { K_STEP, 1, 0x80 }, { 0, 0, 0 } };
const KOp	kcRook4[] = { { K_BACK, -1, 0 }, { K_BACK, 1, 0 }, { 0, 0, 0 } };
const KOp	kcFree4[] = { { K_FREE, -1, 0 }, { K_FREE, 1, 0 }, { 0, 0, 0 } };

const KOp	*const kChkC[5][16] =
{
	{ kcPawn, kcGold, kcLance, kcGold, kcKnight, kcGold, kcSilver, kcGold, kcGold, kToWait, kcBishop, kcHorse, kcRook, kcDragon, kcKing, kToWait },
	{ NULL, kcGold1, NULL, kcGold1, NULL, kcGold1, kcSilver1, kcGold1, kcGold1, kToWait, kcBishop1, kcFree1, NULL, kcStep1, NULL, kToWait },
	{ kcPawn, kcStep2, kcLance, kcStep2, NULL, kcStep2, kcSilver2, kcStep2, kcStep2, kToWait, NULL, kcStep2, kcRook2, kcFree2, NULL, kToWait },
	{ NULL, kcGold3, NULL, kcGold3, NULL, kcGold3, kcSilver3, kcGold3, kcGold3, kToWait, kcBishop3, kcFree3, NULL, kcStep3, NULL, kToWait },
	{ NULL, kcStep4, NULL, kcStep4, NULL, kcStep4, NULL, kcStep4, kcStep4, kToWait, NULL, kcStep4, kcRook4, kcFree4, NULL, kToWait },
};

// One black piece's checking moves. FALSE: the chip went back to its command wait.
bool CheckPiece (const KOp *p, bool all)
{
	if (p == kToWait)
		return (false);
	if (p->op == K_LANCE && !all)
	{
		BLanceK();
		return (true);
	}
	if (p->op == K_KING)
	{
		CKingK();
		return (true);
	}
	for (; p->op; p++)
	{
		const uint16	arg = (uint16) (int16) p->arg;
		if (all)
			switch (p->op)
			{
				case K_STEP:	CStepK(arg, p->mark); break;
				case K_STEPZ:	CStepZK(arg, p->mark); break;
				case K_PAWN:	CPawnK(arg, p->mark); break;
				case K_KNIGHT:	CKnightK(arg); break;
				case K_FREE:	CFreeK(arg); break;
				case K_FWD:		CFwdK(arg); break;
				case K_BACK:	CBackK(arg); break;
				case K_LANCE:	CLanceK(arg); break;
			}
		else
			switch (p->op)
			{
				case K_STEP:	BStepK(arg); break;
				case K_STEPZ:	BStepZK(arg); break;
				case K_PAWN:	BPawnK(arg, false); break;
				case K_KNIGHT:	BPawnK(arg, true); break;
				case K_FREE:	BFreeK(arg); break;
				case K_FWD:		BFwdK(arg); break;
				case K_BACK:	BBackK(arg); break;
			}
	}
	c += 1;		// LJMP to the loop
	return (true);
}

// 1C9C / 1AD1: a drop of piece K next to the king (A + TRB), onto an empty square
void DropAt (int off)
{
	c += 2 + 5;
	s.a = (uint16) (s.trb + off);
	if (RdW(s.a)) { c += 1; return; }
	Add(s.k);
}

// 1CAC: drops of piece K along a line from the king, each empty square up to the first piece
void DropRay (uint16 dir)
{
	s.tr = dir;
	c += 2 + 2;
	s.a = s.trb;
	for (;;)
	{
		c += 5;
		s.a = (uint16) (s.a + dir);
		if (RdW(s.a))
			break;
		Add(s.k);
	}
	c += 1;
}

void Checks (bool all)
{
	c += 1;
	if (all)
		PawnFiles(0x7bd, 0x40);
	c += 1;
	Pins(false);
	if (!Attacks(true))
		return;
	CheckMaps();
	c += 1; ToDR(0);
	c += 4;
	WrW(0x2aa, 0); WrW(0x4b6, 0);
	c += 4;
	if (!RdW(0x96))
	{
		c += 1;
		for (s.trb = 0x15; s.trb != 0x6e; )
		{
			c += 4;
			const uint16	piece = RdW(s.trb);
			if (piece & 0x40)
			{
				if (all)
				{
					c += 6;
					WrW(0x7d1, RdW(0x227 + s.trb));
				}
				c += all ? 24 : 23;
				const uint16	type = (piece & 0x1e) >> 1;
				s.k = Rom(0x71a + type);
				s.l = Rom(0x728 + type);
				const uint32	idx = (RdW(0x9e + s.trb) >> 1) | type;
				s.tr = all ? 0x736 : 0x6ca;
				const KOp		*p = idx < 80 ? (all ? kChkC : kChkB)[idx >> 4][idx & 15] : kToWait;
				if (idx >= 80)
					s.unknown = 1;
				if (p && !CheckPiece(p, all))
					return;
			}
			c += 6;
			s.trb++;
		}

		// drops from black's hand (words $83-$89: pawn, lance, knight, silver, gold, bishop, rook)
		c += 2;
		s.trb = RdW(0x94);
		if (all)
		{
			c += 4;
			if (RdW(0x83))
			{
				c += 6;
				s.a = (uint16) (s.trb + 10);
				if (!RdW(s.a))
				{
					c += 8;
					if (!RdW(0x7bd + Rom(0xec + s.a)))
						Add(0);
				}
			}
		}
		static const int8	kSilverAt[5] = { 11, 10, 9, -9, -11 }, kGoldAt[6] = { 11, 10, 9, 1, -1, -10 },
							kBishopAt[4] = { 11, 9, -9, -11 }, kRookAt[4] = { 10, 1, -1, -10 };
		c += 4;
		if (RdW(0x84))
		{
			s.k = 1;
			if (all) { c += 1; DropRay(10); }
			else { c += 1; DropAt(10); }
		}
		c += 4;
		if (RdW(0x85))
		{
			s.k = 2;
			c += 1;
			DropAt(21);
			DropAt(19);
		}
		c += 4;
		if (RdW(0x86))
		{
			s.k = 3;
			c += 1;
			for (int i = 0; i < 5; i++) DropAt(kSilverAt[i]);
		}
		c += 4;
		if (RdW(0x87))
		{
			s.k = 4;
			c += 1;
			for (int i = 0; i < 6; i++) DropAt(kGoldAt[i]);
		}
		c += 4;
		if (RdW(0x88))
		{
			s.k = 5;
			c += 1;
			for (int i = 0; i < 4; i++)
				if (all) DropRay((uint16) (int16) kBishopAt[i]);
				else DropAt(kBishopAt[i]);
		}
		c += 4;
		if (RdW(0x89))
		{
			s.k = 6;
			c += 1;
			for (int i = 0; i < 4; i++)
				if (all) DropRay((uint16) (int16) kRookAt[i]);
				else DropAt(kRookAt[i]);
		}
	}
	c += 2; WrW(0x96, FromDR());
	c += 1 + 1;		// RT, LJMP to the wait
}

#include "st011n_eval.h"

// a JRQM on itself: waits while RQM is up, then runs once more as it falls through
#define NT_RQM(id)	do {															\
	s.due += 1; s.line = (id); case (id):										\
	if (!Pay(s.due)) return (false);											\
	if (*sr & SR_RQM) { s.due = 1; executed += budget; budget = 0; waiting = true; return (false); }	\
	} while (0)

bool Main (void)
{
	NT_BEGIN

	// the command wait: DR = 0, SR = $C400 (RQM, user flag 1, 8-bit DR), until the CPU writes DR
	for (;;)
	{
		NT_AT(1); SetDR(0);
		NT_AT(2); SetSR(0xc400);
		NT_RQM(3);
		NT_AT(4); SetSR(0x0400);
		// DR is read again at each instruction that takes the command: the CPU may write it meanwhile
		NT_AT(5); s.cmd = *dr;
		NT_BURN(3);
		if (s.cmd >= 0x10)
		{
			// $F1-$F3: diagnostics; anything else from $10 up is ignored
			NT_AT(6); s.cmd = *dr;
			NT_BURN(3);
			if (s.cmd < 0xf0 || s.cmd > 0xf3 || s.cmd == 0xf0)
			{
				NT_BURN(s.cmd < 0xf0 ? 1 : 7);
				continue;
			}
			NT_BURN(2 * (s.cmd - 0xf0));
			if (s.cmd != 0xf3)
			{
				if (s.cmd == 0xf1)
				{
					// $F1, a RAM test: every word gets its address's low byte with the complement above it
					NT_BURN(2);
					s.k = 0xff00;
					for (s.a = 0; s.a != 0x800; )
					{
						NT_BURN(5);
						NT_AT(50); s.tr = s.a & 0xff; Wr(s.a, TestWord(s.a)); s.a++;
						NT_BURN(3);
					}
					NT_BURN(1);
				}
				else
				{
					// $F2 checks what $F1 left, then fills every word with $FFFF and checks that
					NT_AT(51); SetDR(0);
					NT_BURN(2);
					s.k = 0xff00;
				}
				for (s.a = 0; ; )
				{
					NT_BURN(5);
					NT_AT(52); s.tr = s.a & 0xff; s.i = Rd(s.a) != TestWord(s.a);
					NT_BURN(1);
					if (s.i)
						break;
					NT_BURN(4);
					if (++s.a == 0x800)
						break;
				}
				if (s.i)
				{
					// failed: DR = $FF ($F1) or 1, then (16-bit) the word
					NT_AT(53); SetDR(s.cmd == 0xf1 ? 0xff : 1);
					NT_RQM(54);
					NT_AT(55); SetSR(0);
					NT_AT(56); SetDR(s.a);
					NT_RQM(57);
					NT_BURN(1);
					continue;
				}
				if (s.cmd == 0xf1)
				{
					NT_AT(58); SetDR(0);
					NT_RQM(59);
					NT_BURN(1);
					continue;
				}
				NT_BURN(3);
				s.a = 0; s.b = 0x800; s.tr = 0xffff;
			}
			else
			{
				// $F3 sends the data ROM's first word (16-bit), then runs on into $F2's fill from word 1 with
				// whatever TR the last command left
				NT_AT(60); SetSR(0);
				NT_BURN(3);
				NT_AT(61); SetDR(Rom(0)); s.a = 1; s.b = 0x7ff;
				NT_RQM(62);
				NT_BURN(1);
			}
			do
			{
				NT_BURN(1);
				NT_AT(63); Wr(s.a, s.tr); s.a++; s.b--;
				NT_BURN(1);
			} while (s.b);
			NT_BURN(1);
			for (s.a = 0; ; )
			{
				NT_BURN(2);
				NT_AT(64); s.i = Rd(s.a) != 0xffff; s.a++;
				NT_BURN(1);
				if (s.i || (NT_BURN(3), s.a == 0x800))
					break;
			}
			NT_AT(65); SetDR(s.i ? 2 : 0);
			NT_RQM(66);
			if (s.i)
			{
				NT_AT(67); SetSR(0);
				NT_AT(68); SetDR(s.a);
				NT_RQM(69);
			}
			NT_BURN(1);
			continue;
		}

		// $00-$0F through the jump table: word $95 = the command
		NT_BURN(1);
		NT_AT(7); Wr(0x95, *dr);
		NT_BURN(1);
		NT_AT(14); s.cmd = *dr;
		NT_BURN(3);
		if (s.cmd >= 0x10)
		{
			// read past the jump table's end: anywhere in the program
			s.unknown = 1;
			NT_FOREVER(15);
		}
		switch (s.cmd)
		{
			case 0x0: case 0x8: case 0xa: case 0xd:
				NT_BURN(1);		// LJMP to the wait
				continue;
		}
		if (s.cmd == 1)
		{
			// the board: 128 bytes from DR into words $15-$94, then the border round it
			NT_BURN(1 + 2);
			NT_AT(10); SetDR(0);
			NT_AT(11); SetSR(0xa400);
			s.a = 0x15;
			for (s.i = 0; s.i < 128; s.i++)
			{
				NT_BURN(1);
				s.dp = s.a++;
				NT_RQM(12);
				NT_AT(13); Wr(s.dp, GetDR());
				NT_BURN(1);
			}
			NT_BURN(3 + 3 * 20);
			for (uint32 w = 0; w < 20; w++)
				Wr(w, 0xff80);
			NT_BURN(1 + 2 * 9);
			for (uint32 w = 0x14; w <= 0x64; w += 10)
				Wr(w, 0xff80);
			NT_BURN(3 + 3 * 21);
			for (uint32 w = 0x6e; w <= 0x82; w++)
				Wr(w, 0xff80);
			s.tr = 0xff80;
			NT_BURN(1 + 1);	// RT, LJMP
			continue;
		}
		if (s.cmd == 2)
		{
			// a block of RAM out through DR: the address, then the count, in 16-bit words, then the low bytes
			NT_BURN(1);
			NT_AT(20); SetSR(0x0000);
			NT_AT(21); SetDR(0);
			NT_RQM(22);
			NT_AT(23); s.a = GetDR();
			NT_RQM(24);
			NT_AT(25); s.b = GetDR();
			NT_AT(26); SetSR(0x0400);
			NT_BURN(1);
			s.dp = s.a & 0x7ff;
			s.a++;
			NT_AT(27); SetDR(Rd(s.dp)); s.b--;
			NT_AT(28); SetSR(0xa400);
			do
			{
				NT_BURN(1);
				s.dp = s.a & 0x7ff;
				s.a++;
				NT_RQM(29);
				NT_AT(30); SetDR(Rd(s.dp)); s.b--;
				NT_BURN(1);
			} while (s.b);
			NT_BURN(1 + 1);	// RT, LJMP
			continue;
		}

		if (s.cmd == 3 || s.cmd == 4 || s.cmd == 5 || s.cmd == 9 || s.cmd == 0xb || s.cmd == 0xc || s.cmd == 0xe || s.cmd == 0xf)
		{
			c = 0;
			cdr = *dr; csr = *sr;
			s.nev = 0;
			if (s.cmd == 3 || s.cmd == 0xf)
				Evaluate(s.cmd == 0xf);
			else if (s.cmd == 0xe)
				MoveGen();
			else if (s.cmd == 9)
				Mated();
			else if (s.cmd == 0xb || s.cmd == 0xc)
				Checks(s.cmd == 0xc);
			else if (Attacks(s.cmd == 5))
			{
				SwapMap();
				c += 1;		// LJMP to the wait
			}
			s.total = c;
			// the DR and SR changes at their counts, then what's left of the command
			for (s.played = 0, s.i = 0; s.i < (int32) s.nev; s.i++)
			{
				NT_BURN(s.ev[s.i].at - s.played - 1);
				NT_AT(40); *dr = s.ev[s.i].dr; *sr = s.ev[s.i].sr;
				s.played = s.ev[s.i].at;
			}
			NT_BURN(s.total - s.played);
			if (s.unknown)
				NT_FOREVER(41);
			continue;
		}

		if (s.cmd == 6 || s.cmd == 7)
		{
			NT_BURN(1);		// LCALL
			c = 0;
			Pins(s.cmd == 7);
			NT_BURN(c + 1);	// and the LJMP to the wait
			continue;
		}

		// not native yet
		s.unknown = 1;
		NT_FOREVER(8);
	}
	NT_END
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
}

uint64 Run (uint64 n)
{
	budget = n;
	waiting = false;
	while (budget)
		Main();
	return (waiting ? 0 : s.due);
}

inline uint32 Used (uint32 nev) { return ((uint32) offsetof(State, ev) + nev * (uint32) sizeof(State::Ev)); }

uint32 StateSize (void)
{
	return (Used(s.nev) + sizeof(executed));
}

void StateSave (uint8 *buf)
{
	memcpy(buf, &s, Used(s.nev));
	memcpy(buf + Used(s.nev), &executed, sizeof(executed));
}

bool8 StateLoad (const uint8 *buf, uint32 size)
{
	uint32	nev;
	if (size < Used(0) + sizeof(executed))
		return (FALSE);
	memcpy(&nev, buf + offsetof(State, nev), sizeof(nev));
	if (nev > kEvMax || size != Used(nev) + sizeof(executed))
		return (FALSE);
	memcpy(&s, buf, Used(nev));
	memcpy(&executed, buf + Used(nev), sizeof(executed));
	return (TRUE);
}

#ifdef UPD96050_LAB
void LabEvents (void (*fn) (char kind, uint64 at, uint32 word, uint16 v))
{
	lab_event = fn;
}
#endif

} // namespace

const S9xUPD96050Native	S9xST011Native =
{
	Attach, Reset, Idle, Run, StateSize, StateSave, StateLoad,
#ifdef UPD96050_LAB
	LabEvents,
#endif
};

#ifdef UPD96050_LAB
bool8 S9xST011NativeUnknown (void)
{
	return (s.unknown);
}

uint32 S9xST011NativeEvents (bool8 *merged)
{
	*merged = s.merged;
	return (s.nev);
}

void S9xST011NativeTR (uint16 *tr, uint16 *trb)
{
	*tr = s.tr;
	*trb = s.trb;
}
#endif
