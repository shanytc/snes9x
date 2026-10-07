/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <string.h>
#include "snes9x.h"
#include "upd7725.h"
#include "upd7725n.h"

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
static void		Decode (void);
static Regs		r;
static bool8	loaded = FALSE;
static bool8	active = FALSE;
static int		chip = 0;		// UPD7725_DSP1...
static const S9xUPD7725Native	*native = NULL;	// the native chip, or NULL for the firmware
static bool8	native_variant = FALSE;
static uint64	native_next = 0;	// master clock of the native chip's next bus event (0: unknown)
static S9xUPD7725Lane	lane = { 0, NULL };	// the native chip's, while parked in a lane
static uint64	lane_free = 0;		// freed in a lane: when (0: not); its clock still stands where it parked
static uint32	lane_lo[64], lane_hi[64];	// master clocks within which a span's instructions may, and surely do, all run

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

bool8 S9xUPD7725Load (const uint8 *image, uint32 size, int which)
{
	loaded = active = FALSE;
	native = NULL;
	if (!S9xUPD7725IsFirmware(image, size))
		return (FALSE);
	chip = which;

	for (int i = 0; i < 2048; i++)
	{
		prog[i] = ProgWord(image, i);
		uint32	op = prog[i], brch = (op >> 13) & 0x1ff;
		park[i] = ((op >> 22) == 2 && ((op >> 2) & 0x7ff) == (uint32) i) ? (brch == 0x0bc ? 1 : brch == 0x0be ? 2 : 0) : 0;
	}
	for (int i = 0; i < 1024; i++)
		drom[i] = image[6144 + i * 2] | (image[6144 + i * 2 + 1] << 8);
	Decode();

	loaded = TRUE;
	S9xUPD7725Reset();
	return (TRUE);
}

bool8 S9xUPD7725LoadNative (int which)
{
	loaded = active = FALSE;
	native = NULL;
	if (which == UPD7725_DSP1 || which == UPD7725_DSP1B)
		native = &S9xDSP1Native;
	else if (which == UPD7725_DSP2)
		native = &S9xDSP2Native;
	else
		return (FALSE);
	chip = which;
	native_variant = which == UPD7725_DSP1;
	loaded = TRUE;
	native->attach(&r.dr, &r.sr, r.ram, &r.trb, &lane);
	S9xUPD7725Reset();
	return (TRUE);
}

void S9xUPD7725Unload (void)
{
	loaded = active = FALSE;
	native = NULL;
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
	lane_free = 0;
	// n instructions take a span of master clocks that has n or n - 1 of them, by the clock's phase
	const uint64	m = Settings.PAL ? 21281370 : 21477273;
	for (uint32 n = 0; n < 64; n++)
	{
		lane_lo[n] = (uint32) ((n ? (n - 1) * m : 0) + DSP_HZ - 1) / DSP_HZ;
		lane_hi[n] = (uint32) ((n * m + DSP_HZ - 1) / DSP_HZ);
	}
	if (native)
		native->reset(native_variant);
	native_next = 0;
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

// Each program word decoded once at load, so the run loop reads its fields
// instead of extracting them, and DP/RP update without branches.
enum { D_OP, D_RT, D_JP, D_LD, D_PARK };
struct Dec
{
	uint8	kind, src, dst, alu;
	uint8	psel, accb, dpinc, dpkeep;
	uint8	dpxor, rpdec, jtest, jwant;	// jtest: 0 flag, 1 DP low, 2 RQM, 3 always, 4 call, 5 JMPSO
	uint8	jmask, jflagb;
	uint16	imm;						// LD's immediate, or the jump target
};
static Dec	dec[2048];

static void Decode (void)
{
	static const uint8	flag_bits[6] = { F_C, F_Z, F_OV0, F_OV1, F_S0, F_S1 };

	for (int i = 0; i < 2048; i++)
	{
		const uint32	op = prog[i];
		Dec				d;
		memset(&d, 0, sizeof(d));
		d.dpkeep = 0x0f;

		switch (op >> 22)
		{
			case 0:
			case 1:
			{
				d.kind = (op >> 22) ? D_RT : D_OP;
				d.src = (op >> 4) & 15;
				d.dst = op & 15;
				d.alu = (op >> 16) & 15;
				d.psel = (op >> 20) & 3;
				d.accb = (op & 0x8000) != 0;
				// A move into DP or RP takes precedence over that pointer's modifier.
				if (d.dst != 4)
				{
					switch ((op >> 13) & 3)
					{
						case 1: d.dpinc = 1; break;
						case 2: d.dpinc = 0x0f; break;
						case 3: d.dpkeep = 0; break;
					}
					d.dpxor = ((op >> 9) & 15) << 4;
				}
				d.rpdec = (op & 0x100) && d.dst != 5;
				break;
			}

			case 2:
			{
				const uint32	brch = (op >> 13) & 0x1ff;
				d.kind = D_JP;
				d.imm = (op >> 2) & 0x7ff;
				if (brch >= 0x080 && brch <= 0x0af && !(brch & 1))
				{
					const uint32	sel = (brch - 0x080) >> 2;
					d.jtest = 0;
					d.jflagb = sel & 1;
					d.jmask = flag_bits[sel >> 1];
					d.jwant = (brch & 2) != 0;
				}
				else switch (brch)
				{
					case 0x000: d.jtest = 5; break;
					case 0x0b0: d.jtest = 1; d.jmask = 0x0f; d.jwant = 0; d.jflagb = 0; break;	// low nibble == 0
					case 0x0b1: d.jtest = 1; d.jmask = 0x0f; d.jwant = 0; d.jflagb = 1; break;	// != 0
					case 0x0b2: d.jtest = 1; d.jmask = 0x0f; d.jwant = 0x0f; d.jflagb = 0; break;	// == F
					case 0x0b3: d.jtest = 1; d.jmask = 0x0f; d.jwant = 0x0f; d.jflagb = 1; break;	// != F
					case 0x0b4:
					case 0x0b8:
					case 0x100: d.jtest = 3; break;
					case 0x0bc: d.jtest = 2; d.jwant = 0; break;
					case 0x0be: d.jtest = 2; d.jwant = 1; break;
					case 0x140: d.jtest = 4; break;
					default:	d.jtest = 6; break;	// never taken
				}
				if (park[i])
					d.kind = D_PARK;
				break;
			}

			default:
				d.kind = D_LD;
				d.dst = op & 15;
				d.imm = (op >> 6) & 0xffff;
				break;
		}
		dec[i] = d;
	}
}

// The run loop with the registers in locals, written back at the end. The
// multiplier only changes M and N when K or L does, so it runs on those writes.
static void RunFast (void)
{
	uint64	left = r.target - r.executed;
	uint32	pc = r.pc, rp = r.rp, dp = r.dp, sp = r.sp;
	uint16	k = r.k, l = r.l, m = r.m, n = r.n;
	uint16	a = r.a, b = r.b, tr = r.tr, trb = r.trb;
	uint16	dr = r.dr, sr = r.sr, si = r.si, so = r.so;
	uint8	flaga = r.flaga, flagb = r.flagb;

#define FAST_MUL	{ int32 _p = (int16) k * (int16) l; m = (uint16) (_p >> 15); n = (uint16) (_p << 1); }
#define FAST_MOVE(idb, dst)															\
	switch (dst)																	\
	{																				\
		case  0: break;																\
		case  1: a = idb; break;													\
		case  2: b = idb; break;													\
		case  3: tr = idb; break;													\
		case  4: dp = idb & 0xff; break;											\
		case  5: rp = idb & 0x3ff; break;											\
		case  6: dr = idb; sr |= SR_RQM; break;										\
		case  7: sr = (sr & SR_FIXED) | (idb & ~SR_FIXED); break;					\
		case  8:																	\
		case  9: so = idb; break;													\
		case 10: k = idb; FAST_MUL; break;											\
		case 11: k = idb; l = drom[rp]; FAST_MUL; break;							\
		case 12: l = idb; k = r.ram[dp | 0x40]; FAST_MUL; break;					\
		case 13: l = idb; FAST_MUL; break;											\
		case 14: trb = idb; break;													\
		default: r.ram[dp] = idb; break;											\
	}

	while (left)
	{
		const Dec	&d = dec[pc];
		pc = (pc + 1) & 0x7ff;
		left--;

		switch (d.kind)
		{
			case D_OP:
			case D_RT:
			{
				uint16	idb;
				switch (d.src)
				{
					case  0: idb = trb; break;
					case  1: idb = a; break;
					case  2: idb = b; break;
					case  3: idb = tr; break;
					case  4: idb = (uint16) dp; break;
					case  5: idb = (uint16) rp; break;
					case  6: idb = drom[rp]; break;
					case  7: idb = 0x8000 - ((flaga & F_S1) ? 1 : 0); break;
					case  8: sr |= SR_RQM; idb = dr; break;
					case  9: idb = dr; break;
					case 10: idb = sr; break;
					case 11:
					case 12: idb = si; break;
					case 13: idb = k; break;
					case 14: idb = l; break;
					default: idb = r.ram[dp]; break;
				}

				if (d.alu)
				{
					uint16	p;
					switch (d.psel)
					{
						case 0:  p = r.ram[dp]; break;
						case 1:  p = idb; break;
						case 2:  p = m; break;
						default: p = n; break;
					}
					if (d.accb)
						Alu(d.alu, b, flagb, p, (flaga & F_C) != 0);
					else
						Alu(d.alu, a, flaga, p, (flagb & F_C) != 0);
				}

				FAST_MOVE(idb, d.dst)
				dp = ((dp & 0xf0) | ((dp + d.dpinc) & d.dpkeep)) ^ d.dpxor;
				rp = (rp - d.rpdec) & 0x3ff;

				if (d.kind == D_RT)
				{
					sp = (sp - 1) & 3;
					pc = r.stack[sp];
				}
				break;
			}

			case D_PARK:	// a one-instruction loop on RQM can't end until the CPU touches DR
				if ((sr & SR_RQM) ? d.jwant : !d.jwant)
				{
					pc = (pc - 1) & 0x7ff;
					left = 0;
				}
				break;		// else the jump falls through

			case D_JP:
			{
				bool	take;
				switch (d.jtest)
				{
					case 0:  take = (((d.jflagb ? flagb : flaga) & d.jmask) != 0) == (d.jwant != 0); break;
					case 1:  take = ((dp & 0x0f) == d.jwant) != (d.jflagb != 0); break;
					case 2:  take = ((sr & SR_RQM) != 0) == (d.jwant != 0); break;
					case 3:  take = true; break;
					case 4:
						r.stack[sp] = (uint16) pc;
						sp = (sp + 1) & 3;
						take = true;
						break;
					case 5:  pc = so & 0x7ff; take = false; break;	// JMPSO
					default: take = false; break;
				}
				if (take)
					pc = d.imm;
				break;
			}

			default:	// LD: immediate to a destination
			{
				const uint16	idb = d.imm;
				FAST_MOVE(idb, d.dst)
				break;
			}
		}
	}
#undef FAST_MUL
#undef FAST_MOVE

	r.executed = r.target - left;
	r.pc = (uint16) pc; r.rp = (uint16) rp; r.dp = (uint16) dp; r.sp = (uint16) sp;
	r.k = k; r.l = l; r.m = m; r.n = n;
	r.a = a; r.b = b; r.tr = tr; r.trb = trb;
	r.dr = dr; r.sr = sr; r.si = si; r.so = so;
	r.flaga = flaga; r.flagb = flagb;
}

// The DSP's clock up to master clock `t`: the instructions due by then.
static alwaysinline void Advance (uint64 t)
{
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
}

// Freed at master clock `at`, has a chip in a lane run its span by `t`? The span decides it from the gap
// unless the gap is at the edge, where the clock's phase does.
static bool LaneTakenAtEdge (uint64 at, uint64 t)
{
	const uint64	a = (at - r.synced) * DSP_HZ + r.rem, b = (t - r.synced) * DSP_HZ + r.rem;
	return ((Settings.PAL ? b / 21281370 - a / 21281370 : b / 21477273 - a / 21477273) >= lane.span);
}

static alwaysinline bool LaneTaken (uint64 at, uint64 t)
{
	const uint64	d = t - at;
	const uint32	n = lane.span;
	if (n < 64)
	{
		if (d >= lane_hi[n])
			return (true);
		if (d < lane_lo[n])
			return (false);
	}
	return (LaneTakenAtEdge(at, t));
}

static void SyncFrom (uint64 t);

// Most syncs end here: before the native chip's next bus event, or with its lane's word taken.
static alwaysinline void SyncTo (int32 cycles)
{
	uint64	t = r.line_base + (uint64) (int64) cycles;
	if (t <= r.synced)
		return;
	// The native chip can't change what the CPU sees before its next bus event.
	if (t < native_next)
		return;
	// freed in a lane and past its next wait: it has taken its word and parked there, untouched since
	if (lane_free)
	{
		if (LaneTaken(lane_free, t))
		{
			lane_free = 0;
			lane.step();
			native_next = ~(uint64) 0;
			return;
		}
	}
	SyncFrom(t);
}

static void SyncFrom (uint64 t)
{
	if (lane_free)
	{
		// it resumes from where it was freed
		Advance(lane_free);
		r.executed = r.target;
		lane_free = 0;
	}

	Advance(t);

	if (native)
	{

		// overclocked, it runs ahead to its next wait on the CPU, as the firmware does below
		const bool		oc = ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8;
		const uint64	owed = (oc || r.executed < r.target) ? native->run(oc ? (uint64) 1 << 40 : r.target - r.executed) : native->owed();
		r.executed = r.target;

		// the first clock its owed instructions have all run by
		if (!owed)
			native_next = ~(uint64) 0;
		else
		{
			const uint64	m = Settings.PAL ? 21281370 : 21477273;
			native_next = r.synced + (owed * m - r.rem + DSP_HZ - 1) / DSP_HZ;
		}
		return;
	}

	// An overclocked CPU outruns the delays games count on, so the DSP keeps up instead.
	if (ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8)
	{
		for (int i = 0; i < 100000 && !Parked(); i++)
			Step();
		r.executed = r.target;
		return;
	}

	if (r.executed >= r.target)
		return;
	// Most syncs find the DSP parked; settle those before loading anything.
	if (Parked())
	{
		r.executed = r.target;
		return;
	}
	RunFast();
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

// The handshake at `t`. A native chip waiting on it resumes there, so its clock settles up to `t`
// first; a running one only looks at RQM at its next wait.
static alwaysinline void NativeHandshake (int32 t)
{
	// parked on a handshake that frees it: only its clock moves, to where it resumes
	if (native_next == ~(uint64) 0 && (r.sr & (SR_DRC | SR_DRS)))
	{
		const uint64	at = r.line_base + (uint64) (int64) t;
		Handshake();
		native_next = 0;
		if (at > r.synced)
		{
			// in a lane, its clock waits to see if the next sync finds its word taken
			if (lane.span && ONE_CYCLE == 6 && SLOW_ONE_CYCLE == 8)
				lane_free = at;
			else
			{
				Advance(at);
				r.executed = r.target;
			}
		}
		return;
	}
	SyncTo(t);
	Handshake();
	// freed (or parked on the way here and freed): it acts on its next instruction
	if (native_next == ~(uint64) 0 && !(r.sr & SR_RQM))
		native_next = 0;
}

uint8 S9xUPD7725Read (bool8 sr, int32 speed)
{
	int32	end = CPU.Cycles + speed;

	SyncTo(end - 1);
	if (sr)
		return (r.sr >> 8);

	uint8	byte = (!(r.sr & SR_DRC) && (r.sr & SR_DRS)) ? r.dr >> 8 : (uint8) r.dr;

	NativeHandshake(end + 2);
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
	NativeHandshake(end + 2);
}

// The firmware at its command wait, with the registers its wait loop sets, for a state from the native chip.
static void FirmwareAtWait (void)
{
	r.sp = 0;
	if (chip == UPD7725_DSP2)
	{
		r.pc = 0x003;
		r.rp = 0x3ff;
		r.a = 0;
	}
	else
	{
		r.pc = 0x004;
		r.b = 0x00c0;
	}
}

// The native chip saves its program's place after the registers it shares.
uint32 S9xUPD7725StateSize (void)
{
	return (sizeof(r) + (native ? native->state_size() : 0));
}

void S9xUPD7725StateSave (uint8 *buf)
{
	// a lane's pending resumption, settled as the handshake would have
	if (lane_free)
	{
		Advance(lane_free);
		r.executed = r.target;
		lane_free = 0;
	}
	memcpy(buf, &r, sizeof(r));
	if (native)
		native->state_save(buf + sizeof(r));
}

bool8 S9xUPD7725StateLoad (const uint8 *buf, uint32 size)
{
	lane_free = 0;
	if (!loaded || size < sizeof(r))
		return (FALSE);

	const uint32	extra = size - (uint32) sizeof(r);
	if (native ? extra != native->state_size() : extra != 0)
	{
		// From the other kind of chip, firmware or native: only RAM and the bus carry over,
		// so it picks up at its command wait (where a game nearly always leaves it).
		if (native ? extra != 0 : extra != S9xDSP1Native.state_size())
			return (FALSE);
		memcpy(&r, buf, sizeof(r));
		if (native)
			native->idle(native_variant);
		else
			FirmwareAtWait();
		S9xMessage(S9X_WARNING, S9X_FREEZE_FILE_INFO, "This state's DSP chip ran from its firmware, or without it; carrying on from its command wait.");
		native_next = 0;
		active = TRUE;
		return (TRUE);
	}

	memcpy(&r, buf, sizeof(r));
	if (native)
		native->state_load(buf + sizeof(r), extra);
	native_next = 0;
	active = TRUE;
	return (TRUE);
}
