/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <string.h>
#include "snes9x.h"
#include "upd7725.h"

#define DSP_HZ		7600000		// the DSP-n carts' oscillator; one instruction per clock

enum
{
	F_OV0 = 0x01, F_OV1 = 0x02, F_Z = 0x04, F_C = 0x08, F_S0 = 0x10, F_S1 = 0x20
};

enum
{
	SR_RQM = 0x8000, SR_DRS = 0x1000, SR_DRC = 0x0400,
	SR_FIXED = 0x907c	// RQM, DRS and the unused bits: the DSP's own SR writes skip them
};

// Saved as-is, so the padding is explicit and the layout the same everywhere.
struct Regs
{
	uint16	ram[256];
	uint16	stack[4];
	uint16	pc, rp, dp, sp;
	uint16	k, l, m, n;
	uint16	a, b, tr, trb;
	uint16	dr, sr, si, so;
	uint8	flaga, flagb;
	uint8	pad[6];
	uint64	line_base;		// master clock at the start of the current scanline
	uint64	synced;			// master clock the DSP has been run up to
	uint64	executed;		// instructions run since reset
	uint64	target;			// instructions due by `synced`
	uint64	rem;			// what's left over of `target`, in master clocks * DSP_HZ
};
static_assert(sizeof(Regs) == 600, "savestate layout");

static uint32	prog[2048];
static uint16	drom[1024];
static uint8	park[2048];		// 1: a JNRQM to itself, 2: a JRQM to itself
static Regs		r;
static bool8	loaded = FALSE;
static bool8	active = FALSE;

static inline uint32 ProgWord (const uint8 *image, int i)
{
	return (image[i * 3] | (image[i * 3 + 1] << 8) | (image[i * 3 + 2] << 16));
}

bool8 S9xUPD7725IsFirmware (const uint8 *image, uint32 size)
{
	if (!image || size != UPD7725_FIRMWARE_SIZE)
		return (FALSE);

	// Jump words keep their two low bits clear; a byte-swapped or foreign dump won't.
	int	jumps = 0;
	for (int i = 0; i < 2048; i++)
	{
		uint32	op = ProgWord(image, i);
		if ((op >> 22) == 2)
		{
			if (op & 3)
				return (FALSE);
			jumps++;
		}
	}

	return (jumps > 0);
}

bool8 S9xUPD7725Load (const uint8 *image, uint32 size)
{
	loaded = active = FALSE;
	if (!S9xUPD7725IsFirmware(image, size))
		return (FALSE);

	for (int i = 0; i < 2048; i++)
	{
		prog[i] = ProgWord(image, i);
		uint32	op = prog[i], brch = (op >> 13) & 0x1ff;
		park[i] = ((op >> 22) == 2 && ((op >> 2) & 0x7ff) == (uint32) i) ? (brch == 0x0bc ? 1 : brch == 0x0be ? 2 : 0) : 0;
	}
	for (int i = 0; i < 1024; i++)
		drom[i] = image[6144 + i * 2] | (image[6144 + i * 2 + 1] << 8);

	loaded = TRUE;
	S9xUPD7725Reset();
	return (TRUE);
}

void S9xUPD7725Unload (void)
{
	loaded = active = FALSE;
}

bool8 S9xUPD7725Loaded (void)
{
	return (loaded);
}

bool8 S9xUPD7725Active (void)
{
	return (active);
}

void S9xUPD7725Reset (void)
{
	if (!loaded)
		return;
	memset(&r, 0, sizeof(r));
	r.rp = 0x3ff;
	active = TRUE;
}

void S9xUPD7725Suspend (void)
{
	active = FALSE;
}

static alwaysinline uint16 Source (uint32 src)
{
	switch (src)
	{
		case  0: return (r.trb);
		case  1: return (r.a);
		case  2: return (r.b);
		case  3: return (r.tr);
		case  4: return (r.dp);
		case  5: return (r.rp);
		case  6: return (drom[r.rp]);
		case  7: return (0x8000 - ((r.flaga & F_S1) ? 1 : 0));	// SGN: the saturation value
		case  8: r.sr |= SR_RQM; return (r.dr);
		case  9: return (r.dr);			// DRNF: read without requesting more
		case 10: return (r.sr);
		case 11:
		case 12: return (r.si);
		case 13: return (r.k);
		case 14: return (r.l);
		default: return (r.ram[r.dp]);
	}
}

static alwaysinline void Move (uint16 idb, uint32 dst)
{
	switch (dst)
	{
		case  0: break;
		case  1: r.a = idb; break;
		case  2: r.b = idb; break;
		case  3: r.tr = idb; break;
		case  4: r.dp = idb & 0xff; break;
		case  5: r.rp = idb & 0x3ff; break;
		case  6: r.dr = idb; r.sr |= SR_RQM; break;
		case  7: r.sr = (r.sr & SR_FIXED) | (idb & ~SR_FIXED); break;
		case  8:
		case  9: r.so = idb; break;
		case 10: r.k = idb; break;
		case 11: r.k = idb; r.l = drom[r.rp]; break;		// KLR
		case 12: r.l = idb; r.k = r.ram[r.dp | 0x40]; break;	// KLM
		case 13: r.l = idb; break;
		case 14: r.trb = idb; break;
		default: r.ram[r.dp] = idb; break;
	}
}

// S0, Z and S1 without branches: ALU results are too varied to predict. S1
// follows S0 until an overflow is outstanding (OV1), then holds its sign.
static alwaysinline uint32 SignFlags (uint16 v, uint8 flag)
{
	uint32	s0 = v >> 15;
	uint32	ov1 = (flag >> 1) & 1;
	uint32	s1 = s0 ^ ((s0 ^ (flag >> 5)) & ov1 & 1);
	return ((s0 << 4) | ((uint32) (v == 0) << 2) | (s1 << 5));
}

// ADD/SUB and friends: carry or borrow, OV0, and OV1, where a second overflow
// cancels the first if it lands on the other side.
static alwaysinline uint8 ArithFlags (uint16 q, uint16 p, uint32 res, uint8 flag, bool sub)
{
	uint16	v = (uint16) res;
	uint32	f = SignFlags(v, flag);
	uint32	s0 = (f >> 4) & 1, s1 = (f >> 5) & 1;
	uint32	ov0 = (sub ? ((q ^ v) & (q ^ p)) : ((q ^ v) & (p ^ v))) >> 15;
	uint32	ov1_in = (flag >> 1) & 1;
	uint32	t = 1 ^ (ov1_in & (s1 ^ s0));
	uint32	ov1 = ov1_in ^ (ov0 & (t ^ ov1_in));
	return ((uint8) (f | ((uint32) (res > 0xffff) << 3) | ov0 | (ov1 << 1)));
}

// ADC/SBB and SHL1 take their carry in from the other accumulator's flags,
// which is what chains ACCA and ACCB into one 32-bit value.
static alwaysinline void Alu (uint32 op, uint16 &acc, uint8 &flag, uint16 p, bool cin)
{
	uint16	q = acc;
	uint32	res;

	switch (op)
	{
		case  1: acc = q | p; flag = (uint8) SignFlags(acc, flag); return;
		case  2: acc = q & p; flag = (uint8) SignFlags(acc, flag); return;
		case  3: acc = q ^ p; flag = (uint8) SignFlags(acc, flag); return;
		case  4: res = q - p; flag = ArithFlags(q, p, res, flag, true); break;
		case  5: res = q + p; flag = ArithFlags(q, p, res, flag, false); break;
		case  6: res = q - p - cin; flag = ArithFlags(q, p, res, flag, true); break;
		case  7: res = q + p + cin; flag = ArithFlags(q, p, res, flag, false); break;
		case  8: res = q - 1; flag = ArithFlags(q, 1, res, flag, true); break;
		case  9: res = q + 1; flag = ArithFlags(q, 1, res, flag, false); break;
		case 10: acc = (uint16) ~q; flag = (uint8) SignFlags(acc, flag); return;
		case 11: acc = (q >> 1) | (q & 0x8000); flag = (uint8) (SignFlags(acc, flag) | ((q & 1) << 3)); return;
		case 12: acc = (uint16) ((q << 1) | cin); flag = (uint8) (SignFlags(acc, flag) | ((q >> 15) << 3)); return;
		case 13: acc = (uint16) ((q << 2) | 3); flag = (uint8) SignFlags(acc, flag); return;
		case 14: acc = (uint16) ((q << 4) | 15); flag = (uint8) SignFlags(acc, flag); return;
		default: acc = (uint16) ((q << 8) | (q >> 8)); flag = (uint8) SignFlags(acc, flag); return;
	}

	acc = (uint16) res;
}

static alwaysinline void ExecOP (uint32 op)
{
	uint16	idb = Source((op >> 4) & 15);
	uint32	alu = (op >> 16) & 15;

	if (alu)
	{
		uint16	p;

		switch ((op >> 20) & 3)
		{
			case 0:  p = r.ram[r.dp]; break;
			case 1:  p = idb; break;
			case 2:  p = r.m; break;
			default: p = r.n; break;
		}

		if (op & 0x8000)
			Alu(alu, r.b, r.flagb, p, (r.flaga & F_C) != 0);
		else
			Alu(alu, r.a, r.flaga, p, (r.flagb & F_C) != 0);
	}

	uint32	dst = op & 15;
	Move(idb, dst);

	// A move into DP or RP takes precedence over that pointer's modifier.
	if (dst != 4)
	{
		switch ((op >> 13) & 3)
		{
			case 1: r.dp = (r.dp & 0xf0) | ((r.dp + 1) & 0x0f); break;
			case 2: r.dp = (r.dp & 0xf0) | ((r.dp - 1) & 0x0f); break;
			case 3: r.dp &= 0xf0; break;
		}
		r.dp ^= ((op >> 9) & 15) << 4;
	}

	if ((op & 0x100) && dst != 5)
		r.rp = (r.rp - 1) & 0x3ff;
}

static alwaysinline void ExecJP (uint32 op)
{
	static const uint8	flag_bits[6] = { F_C, F_Z, F_OV0, F_OV1, F_S0, F_S1 };
	uint32	brch = (op >> 13) & 0x1ff;
	uint16	na = (op >> 2) & 0x7ff;
	bool	take = false;

	if (brch >= 0x080 && brch <= 0x0af && !(brch & 1))
	{
		// pairs of JNx/Jx: flag C, Z, OV0, OV1, S0, S1, each for ACCA then ACCB
		uint32	sel = (brch - 0x080) >> 2;
		uint8	f = (sel & 1) ? r.flagb : r.flaga;
		bool	set = (f & flag_bits[sel >> 1]) != 0;
		take = (brch & 2) ? set : !set;
	}
	else switch (brch)
	{
		case 0x000: r.pc = r.so & 0x7ff; return;		// JMPSO
		case 0x0b0: take = (r.dp & 0x0f) == 0x00; break;
		case 0x0b1: take = (r.dp & 0x0f) != 0x00; break;
		case 0x0b2: take = (r.dp & 0x0f) == 0x0f; break;
		case 0x0b3: take = (r.dp & 0x0f) != 0x0f; break;
		case 0x0b4:										// JNSIAK, JNSOAK: the serial
		case 0x0b8: take = true; break;					// port is never wired up
		case 0x0bc: take = !(r.sr & SR_RQM); break;
		case 0x0be: take = (r.sr & SR_RQM) != 0; break;
		case 0x100: take = true; break;
		case 0x140:
			r.stack[r.sp] = r.pc;
			r.sp = (r.sp + 1) & 3;
			take = true;
			break;
	}

	if (take)
		r.pc = na;
}

static alwaysinline void Step (void)
{
	uint32	op = prog[r.pc];
	r.pc = (r.pc + 1) & 0x7ff;

	switch (op >> 22)
	{
		case 0:
			ExecOP(op);
			break;
		case 1:		// RT: an OP, then return
			ExecOP(op);
			r.sp = (r.sp - 1) & 3;
			r.pc = r.stack[r.sp];
			break;
		case 2:
			ExecJP(op);
			break;
		default:	// LD: immediate to a destination
			Move((op >> 6) & 0xffff, op & 15);
			break;
	}

	// The multiplier runs every cycle on whatever K and L hold.
	int32	prod = (int16) r.k * (int16) r.l;
	r.m = (uint16) (prod >> 15);
	r.n = (uint16) (prod << 1);
}

// A one-instruction loop on RQM can't end until the CPU touches DR.
static alwaysinline bool Parked (void)
{
	uint8	k = park[r.pc];
	return (k && ((k == 1) ? !(r.sr & SR_RQM) : (r.sr & SR_RQM) != 0));
}

static void SyncTo (int32 cycles)
{
	uint64	t = r.line_base + (uint64) (int64) cycles;
	if (t <= r.synced)
		return;

	// Constant divisors, so the compiler multiplies instead of dividing.
	uint64	acc = (t - r.synced) * DSP_HZ + r.rem;
	r.synced = t;
	if (Settings.PAL)
	{
		r.target += acc / 21281370;
		r.rem = acc % 21281370;
	}
	else
	{
		r.target += acc / 21477273;
		r.rem = acc % 21477273;
	}

	// An overclocked CPU outruns the delays games count on, so the DSP keeps up instead.
	if (ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8)
	{
		for (int i = 0; i < 100000 && !Parked(); i++)
			Step();
		r.executed = r.target;
		return;
	}

	while (r.executed < r.target)
	{
		if (Parked())
		{
			r.executed = r.target;
			break;
		}
		Step();
		r.executed++;
	}
}

void S9xUPD7725EndScanline (void)
{
	SyncTo(CPU.Cycles);
	r.line_base += Timings.H_Max;
}

// Bus timing, as on the cart (MiSTer's model, which matches it): the CPU's
// byte beats DSP writes up to the end of its strobe, a read latches a clock
// before that, and RQM/DRS reach the DSP two clocks after it.
static void Handshake (void)
{
	if (r.sr & SR_DRC)
		r.sr &= ~SR_RQM;
	else if (!(r.sr & SR_DRS))
		r.sr |= SR_DRS;
	else
		r.sr &= ~(SR_DRS | SR_RQM);
}

uint8 S9xUPD7725Read (bool8 sr, int32 speed)
{
	int32	end = CPU.Cycles + speed;

	SyncTo(end - 1);
	if (sr)
		return (r.sr >> 8);

	uint8	byte = (!(r.sr & SR_DRC) && (r.sr & SR_DRS)) ? r.dr >> 8 : (uint8) r.dr;

	SyncTo(end + 2);
	Handshake();
	return (byte);
}

void S9xUPD7725Write (uint8 byte, bool8 sr, int32 speed)
{
	int32	end = CPU.Cycles + speed;

	SyncTo(end);
	if (sr)
		return;

	if (!(r.sr & SR_DRC) && (r.sr & SR_DRS))
		r.dr = (byte << 8) | (r.dr & 0x00ff);
	else
		r.dr = (r.dr & 0xff00) | byte;

	// A DSP write before the DSP sees this one lands on top of it.
	SyncTo(end + 2);
	Handshake();
}

uint32 S9xUPD7725StateSize (void)
{
	return (sizeof(r));
}

void S9xUPD7725StateSave (uint8 *buf)
{
	memcpy(buf, &r, sizeof(r));
}

bool8 S9xUPD7725StateLoad (const uint8 *buf, uint32 size)
{
	if (!loaded || size != sizeof(r))
		return (FALSE);
	memcpy(&r, buf, sizeof(r));
	active = TRUE;
	return (TRUE);
}
