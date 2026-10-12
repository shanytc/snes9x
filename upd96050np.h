/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The kit a native uPD96050 chip is written with, included once inside each chip
// file's unnamed namespace. A command is a program that resumes where it last
// stopped: at an access to the shared RAM the clock hasn't reached yet. NT_BURN
// owes the instructions the firmware spends between two accesses, and NT_AT pays
// them and the access's own, so the CPU sees every result at the chip's clock.
//
// The chip file's State has `line`, `sub` (a subroutine's own resume point) and `due`.

uint16	*dr, *sr;
uint8	*ram;			// the shared RAM: 2048 words, low byte first
uint64	budget;			// instructions left in this run
uint64	executed;		// instructions since reset (the lab's clock)
bool	waiting;		// this run ended where only the CPU's next DR access can move it on

#ifdef UPD96050_LAB
void	(*lab_event) (char kind, uint64 at, uint32 word, uint16 v);
#define NT_EVENT(kind, w, v)	do { if (lab_event) lab_event(kind, executed, (w) & 0x7ff, v); } while (0)
#else
#define NT_EVENT(kind, w, v)	do { } while (0)
#endif

inline uint16 Rd (uint32 w)
{
	const uint8		*p = ram + ((w & 0x7ff) << 1);
	const uint16	v = (uint16) (p[0] | (p[1] << 8));
	NT_EVENT('R', w, v);
	return (v);
}

inline void Wr (uint32 w, uint16 v)
{
	uint8	*p = ram + ((w & 0x7ff) << 1);
	p[0] = (uint8) v;
	p[1] = (uint8) (v >> 8);
	NT_EVENT('W', w, v);
}

// pays what's owed, or as much as the budget allows
inline bool Pay (uint32 &due)
{
	if (budget < due)
	{
		due -= (uint32) budget;
		executed += budget;
		budget = 0;
		return (false);
	}
	budget -= due;
	executed += due;
	due = 0;
	return (true);
}

// The multiplier: K x L, M = the sign and top 15 bits, N = the low 15 and a zero.
struct Product { uint16 m, n; };
inline Product Mul (uint16 k, uint16 l)
{
	const int32	p = (int16) k * (int16) l;
	return { (uint16) (p >> 15), (uint16) (p << 1) };
}

// CMP then INC: the two's complement, as the chip takes an absolute value
inline uint16 Abs (uint16 v)
{
	return ((v & 0x8000) ? (uint16) (~v + 1) : v);
}

// Resume points carry fixed ids, so a savestate outlives edits around them.
#define NT_BEGIN		switch (s.line) { case 0:
#define NT_END			} s.line = 0; return (true);
#define NT_BURN(k)		(s.due += (uint32) (k))
// the access instruction itself, once the clock gets there
#define NT_AT(id)		s.due += 1; s.line = (id); case (id): if (!Pay(s.due)) return (false)
// a subroutine, with its own resume points in s.sub
#define NT_CALL(id, fn)	s.line = (id); case (id): if (!fn()) return (false)
#define NT_SUB_BEGIN	switch (s.sub) { case 0:
#define NT_SUB_END		} s.sub = 0; return (true);
#define NT_SUB_AT(id)	s.due += 1; s.sub = (id); case (id): if (!Pay(s.due)) return (false)
// a loop the program never leaves: it only burns
#define NT_FOREVER(id)	do { s.line = (id); case (id): executed += budget; budget = 0; waiting = true; return (false); } while (0)
#define NT_SUB_FOREVER(id)	do { s.sub = (id); case (id): executed += budget; budget = 0; waiting = true; return (false); } while (0)
