/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The kit a native uPD77C25 chip is written with, included once inside each chip
// file's unnamed namespace. A command is a program that resumes where it last
// stopped: at a bus access the clock hasn't reached yet, or at a wait on the CPU.
// NT_BURN owes the instructions the firmware spends between two accesses, so the
// CPU sees every result and status change at the same clock as on the chip.
//
// The chip file supplies Boot (power-on to its command wait), Idle (the command
// wait), Program (cmd -> its program), ChipReset (variant) and ChipLoaded (after
// a state load); the kit runs them. It defines NT_IDLE_WAIT, Idle's wait on the CPU.

enum
{
	SR_RQM = 0x8000, SR_DRS = 0x1000, SR_DRC = 0x0400,
	SR_FIXED = 0x907c	// what the DSP's own SR writes leave alone
};

// Saved as-is, so the layout is fixed.
struct State
{
	int16	in[16];
	int16	out[16];
	int32	line;		// where the current program resumes (0 = its start)
	int32	i;
	uint32	due;		// instructions owed before the next bus access
	uint16	acc;		// the accumulator a command leaves (the DSP-2 shows it in DR)
	uint16	reg[9];		// registers whose leftovers a later command uses (the chip file's own)
	uint8	cmd;
	uint8	running;	// 0: the command wait, 1: a command, 2: the power-on sequence
	uint8	parked;		// waiting on the CPU
	uint8	variant;	// the chip's revision
};
static_assert(sizeof(State) == 100, "savestate layout");

State	s;
uint16	*dr, *sr, *ram, *trb;
uint64	budget;
bool	(*prog) (void);		// the running command's program, from s.cmd

S9xUPD7725Lane	*lanes;		// upd7725.cpp's, where a chip parked in a lane says so

// A lane: a command's loop of words, one per wait on the CPU, which upd7725.cpp can step through for the
// program while the chip waits. It runs while s.i < count(); word k comes after gap(k) instructions and
// a wait, and put takes it (in) or get gives it (out). gap mustn't depend on what put changes.
struct Lane
{
	int32	(*count) (void);
	uint32	(*gap) (int32 k);
	void	(*put) (int32 k, uint16 v);
	uint16	(*get) (int32 k);
	void	(*step) (void);				// LaneIn or LaneOut, for these
};
uint16	lane_word;

// one turn of the loop from its wait to the next: the access, then the gap, parked again
template <int32 (*Count) (void), uint32 (*Gap) (int32), void (*Put) (int32, uint16)>
void LaneIn (void)
{
	lane_word = *dr;
	*sr |= SR_RQM;
	Put(s.i, lane_word);
	s.i++;
	s.parked = 1;
	s.due = 0;
	const int32	n = Count();
	lanes->span = s.i + 1 < n ? 2 + Gap(s.i + 1) : 0;
}

template <int32 (*Count) (void), uint32 (*Gap) (int32), uint16 (*Get) (int32)>
void LaneOut (void)
{
	*dr = Get(s.i);
	*sr |= SR_RQM;
	s.i++;
	s.parked = 1;
	s.due = 0;
	const int32	n = Count();
	lanes->span = s.i + 1 < n ? 2 + Gap(s.i + 1) : 0;
}

bool	Boot (void);
bool	Idle (void);
bool	(*Program (uint8 cmd)) (void);
void	ChipReset (bool variant);
void	ChipLoaded (void);

#ifdef UPD7725N_LAB
// The test lab's view: every bus event with the instructions since the one before.
void	(*lab_event) (char kind, uint32 n, uint32 val);
uint32	lab_n;
#define NT_EVENT(kind, v)	do { if (lab_event) lab_event(kind, lab_n, v); lab_n = 0; } while (0)
#define NT_SPENT(k)			(lab_n += (uint32) (k))
#else
#define NT_EVENT(kind, v)	do { } while (0)
#define NT_SPENT(k)			do { } while (0)
#endif

// Resume points carry fixed ids, so a savestate outlives edits around them.
#define NT_BEGIN		switch (s.line) { case 0:
#define NT_END			} s.line = 0; return (true);
#define NT_BURN(k)		(s.due += (uint32) (k))
// the access instruction itself, once the clock gets there
#define NT_AT(id)																\
	s.due += 1; s.line = (id); case (id):										\
	if (budget < s.due) { s.due -= (uint32) budget; NT_SPENT(budget); budget = 0; return (false); }	\
	budget -= s.due; NT_SPENT(s.due); s.due = 0
// a JRQM on itself: parks while RQM is up, then runs once more as it falls through
#define NT_WAIT(id)	do {														\
	s.line = (id); case (id):													\
	if (!s.parked)																\
	{																			\
		if (budget < s.due) { s.due -= (uint32) budget; NT_SPENT(budget); budget = 0; return (false); }	\
		budget -= s.due; NT_SPENT(s.due); s.due = 0;							\
	}																			\
	if (*sr & SR_RQM)															\
	{																			\
		if (!s.parked) { s.parked = 1; NT_EVENT('P', 0); }						\
		lanes->span = 0;														\
		budget = 0;																\
		return (false);															\
	}																			\
	s.parked = 0; s.due = 1; } while (0)
// a lane's wait for word s.i, which posts the lane as it parks
#define NT_LWAIT(id, L)	do {													\
	s.line = (id); case (id):													\
	if (!s.parked)																\
	{																			\
		if (budget < s.due) { s.due -= (uint32) budget; NT_SPENT(budget); budget = 0; return (false); }	\
		budget -= s.due; NT_SPENT(s.due); s.due = 0;							\
	}																			\
	if (*sr & SR_RQM)															\
	{																			\
		if (!s.parked) { s.parked = 1; NT_EVENT('P', 0); }						\
		lanes->span = s.i + 1 < (L).count() ? 2 + (L).gap(s.i + 1) : 0;		\
		lanes->step = (L).step;													\
		budget = 0;																\
		return (false);															\
	}																			\
	s.parked = 0; s.due = 1; } while (0)
#define NT_READ(id, dst)	do { NT_AT(id); (dst) = (int16) *dr; *sr |= SR_RQM; NT_EVENT('R', *dr); } while (0)
#define NT_READNF(id, dst)	do { NT_AT(id); (dst) = (int16) *dr; NT_EVENT('N', *dr); } while (0)
#define NT_WRITE(id, v)		do { NT_AT(id); *dr = (uint16) (v); *sr |= SR_RQM; NT_EVENT('W', *dr); } while (0)
#define NT_SR(id, v)		do { NT_AT(id); *sr = (uint16) ((*sr & SR_FIXED) | ((v) & ~SR_FIXED)); NT_EVENT('S', *sr); } while (0)

// A lane's loop: words s.i..count()-1 from the CPU (GETS) or to it (PUTS).
#define NT_GETS(id, L)	do {													\
	for (; s.i < (L).count(); s.i++)											\
	{																			\
		NT_BURN((L).gap(s.i));													\
		NT_LWAIT(id, L); NT_READ((id) + 1, lane_word);							\
		(L).put(s.i, lane_word);												\
	}																			\
	} while (0)
#define NT_PUTS(id, L)	do {													\
	for (; s.i < (L).count(); s.i++)											\
	{																			\
		NT_BURN((L).gap(s.i));													\
		NT_LWAIT(id, L); NT_WRITE((id) + 1, (L).get(s.i));						\
	}																			\
	} while (0)

// the multiplier's high word
inline int16 Mul (int16 a, int16 b)
{
	return ((int16) ((int32) a * b >> 15));
}

void Attach (uint16 *data, uint16 *status, uint16 *mem, uint16 *t, S9xUPD7725Lane *l)
{
	dr = data;
	sr = status;
	ram = mem;
	trb = t;
	lanes = l;
}

void Reset (bool8 variant)
{
	memset(&s, 0, sizeof(s));
	if (lanes)
		lanes->span = 0;
	s.variant = variant ? 1 : 0;
	s.running = 2;
	ChipReset(s.variant != 0);
	prog = Program(0);
}

uint32 Owed (void)
{
	// freed but not resumed, or between programs: it acts on its next instruction
	if (s.parked)
		return ((*sr & SR_RQM) ? 0 : 1);
	return (s.due ? s.due : 1);
}

uint32 Run (uint64 b)
{
	// most syncs find it waiting on the CPU
	if (!(s.parked && (*sr & SR_RQM)))
	{
		budget = b;
		for (;;)
		{
			bool	done;
			switch (s.running)
			{
				case 0:	 done = Idle(); break;
				case 1:	 done = prog(); break;
				default: done = Boot(); break;
			}
			if (!done)
				break;
			if (s.running == 0)
				prog = Program(s.cmd);
			s.running = s.running == 0 ? 1 : 0;
		}
	}
	return (Owed());
}

void IdleAt (bool8 variant)
{
	Reset(variant);
	s.running = 0;
	s.line = NT_IDLE_WAIT;
	s.parked = 1;
}

uint32 StateSize (void)
{
	return (sizeof(s));
}

void StateSave (uint8 *buf)
{
	memcpy(buf, &s, sizeof(s));
}

bool8 StateLoad (const uint8 *buf, uint32 size)
{
	if (size != sizeof(s))
		return (FALSE);
	memcpy(&s, buf, sizeof(s));
	if (lanes)
		lanes->span = 0;
	ChipLoaded();
	prog = Program(s.cmd);
	return (TRUE);
}

#ifdef UPD7725N_LAB
void LabEvents (void (*fn) (char kind, uint32 n, uint32 val))
{
	lab_event = fn;
	lab_n = 0;
}

bool LabIdle (void)
{
	return (s.running == 0);
}
#define NT_CHIP_LAB	, LabEvents, LabIdle
#else
#define NT_CHIP_LAB
#endif

// the chip's S9xUPD7725Native, for the chip file to define
#define NT_CHIP		{ Attach, Reset, Run, Owed, IdleAt, StateSize, StateSave, StateLoad NT_CHIP_LAB }
