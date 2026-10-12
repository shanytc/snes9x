/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <string.h>
#include "snes9x.h"
#include "st018.h"
#include "memmap.h"

#define ST018_HZ	21440000	// the board's oscillator, as bsnes has it

// ARM memory map: program ROM at 0, the bridge at 4000_0000, data ROM at A000_0000, RAM at E000_0000.
// Each bus access and each internal cycle takes one clock (unmeasured: the ROMs are on the die).

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b, M_SYS = 0x1f };
enum { F_N = 0x80000000u, F_Z = 0x40000000u, F_C = 0x20000000u, F_V = 0x10000000u, F_I = 0x80, F_F = 0x40 };

// Saved as-is, so the layout is the same everywhere.
struct State
{
	uint32	r[16];				// the current mode's registers; r[15] is the next instruction's address
	uint32	bank_r8[2][5];		// r8-r12: [0] every mode but FIQ, [1] FIQ
	uint32	bank_sp[6];			// r13 and r14 by bank: usr/sys, fiq, irq, svc, abt, und
	uint32	bank_lr[6];
	uint32	spsr[6];
	uint32	cpsr;
	uint32	timer, timerlatch;
	uint32	last_fetch;			// what an unmapped read returns
	uint32	boot_wait;			// clocks left of the delay after reset
	uint8	cputoarm_ready, cputoarm_data, armtocpu_ready, armtocpu_data;
	uint8	reset, ready, signal, pad[5];
	uint64	line_base;			// master clock at the start of the current scanline
	uint64	synced;				// master clock the chip has been run up to
	uint64	clocks;				// chip clocks run since power-on
	uint64	target;				// chip clocks due by `synced`
	uint64	rem;				// what's left over of `target`, in master clocks * ST018_HZ
};
static_assert(sizeof(State) == 248, "savestate layout");

static State	s;
static uint8	prog[ST018_PROGRAM_SIZE];
static uint8	drom[ST018_DATA_SIZE];
static uint8	ram[0x4000];
static uint32	cur;			// address of the instruction executing
static bool8	loaded = FALSE;
static bool8	active = FALSE;

bool8 S9xST018IsFirmware (const uint8 *image, uint32 size)
{
	if (!image || size != ST018_PROGRAM_SIZE + ST018_DATA_SIZE)
		return (FALSE);

	// The reset vector is a branch (cond AL), as on any ARM.
	const uint32	v = image[0] | (image[1] << 8) | (image[2] << 16) | ((uint32) image[3] << 24);
	return ((v & 0xff000000) == 0xea000000);
}

static inline uint8 Status (void)
{
	return ((uint8) (s.armtocpu_ready | (s.signal << 2) | (s.cputoarm_ready << 3) | (s.ready << 7)));
}

static void PowerArm (void)
{
	memset(s.r, 0, sizeof(s.r));
	memset(s.bank_r8, 0, sizeof(s.bank_r8));
	memset(s.bank_sp, 0, sizeof(s.bank_sp));
	memset(s.bank_lr, 0, sizeof(s.bank_lr));
	memset(s.spsr, 0, sizeof(s.spsr));
	s.cpsr = M_SVC | F_I | F_F;
	s.timer = s.timerlatch = 0;
	s.cputoarm_ready = s.armtocpu_ready = 0;
	s.signal = s.ready = 0;
	s.boot_wait = 65536;		// the reset sequence's delay before the ARM starts
}

bool8 S9xST018Load (const uint8 *image, uint32 size)
{
	loaded = active = FALSE;
	if (!S9xST018IsFirmware(image, size))
		return (FALSE);
	memcpy(prog, image, ST018_PROGRAM_SIZE);
	memcpy(drom, image + ST018_PROGRAM_SIZE, ST018_DATA_SIZE);
	loaded = TRUE;
	S9xST018Reset();
	return (TRUE);
}

void S9xST018Unload (void)
{
	loaded = active = FALSE;
}

bool8 S9xST018Loaded (void)
{
	return (loaded);
}

bool8 S9xST018Active (void)
{
	return (active);
}

void S9xST018Reset (void)
{
	if (!loaded)
		return;
	memset(&s, 0, sizeof(s));
	memset(ram, 0, sizeof(ram));
	PowerArm();
	active = TRUE;
}

void S9xST018Suspend (void)
{
	active = FALSE;
}

// ---- the ARM's bus

static uint32 IoRead (uint32 addr)
{
	switch (addr & 0xe000003f)
	{
		case 0x40000010:
			if (s.cputoarm_ready)
			{
				s.cputoarm_ready = 0;
				return (s.cputoarm_data);
			}
			return (0);
		case 0x40000020:
			return (Status());
	}
	return (0);
}

static void IoWrite (uint32 addr, uint32 v)
{
	v &= 0xff;
	switch (addr & 0xe000003f)
	{
		case 0x40000000: s.armtocpu_ready = 1; s.armtocpu_data = (uint8) v; break;
		case 0x40000010: s.signal = 1; break;
		case 0x40000020: s.timerlatch = (s.timerlatch & 0xffff00) | v; break;
		case 0x40000024: s.timerlatch = (s.timerlatch & 0xff00ff) | (v << 8); break;
		case 0x40000028: s.timerlatch = (s.timerlatch & 0x00ffff) | (v << 16); break;
		case 0x4000002c: s.timer = s.timerlatch; break;
	}
}

static inline const uint8 *Mem (uint32 addr, uint32 &mask)
{
	switch (addr >> 29)
	{
		case 0: mask = ST018_PROGRAM_SIZE - 1; return (prog);
		case 5: mask = ST018_DATA_SIZE - 1; return (drom);
		case 7: mask = sizeof(ram) - 1; return (ram);
	}
	return (NULL);
}

// A word as on the bus: aligned.
static uint32 Read32 (uint32 addr)
{
	uint32		mask;
	const uint8	*m = Mem(addr, mask);
	if (m)
	{
		const uint8	*p = m + (addr & mask & ~3u);
		return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32) p[3] << 24));
	}
	switch (addr >> 29)
	{
		case 2: return (IoRead(addr));
		case 3: return (0x40404001);
	}
	return (s.last_fetch);
}

static uint8 Read8 (uint32 addr)
{
	uint32		mask;
	const uint8	*m = Mem(addr, mask);
	if (m)
		return (m[addr & mask]);
	return ((uint8) Read32(addr));
}

static void Write32 (uint32 addr, uint32 v)
{
	if ((addr >> 29) == 7)
	{
		uint8	*p = ram + (addr & (sizeof(ram) - 1) & ~3u);
		p[0] = (uint8) v; p[1] = (uint8) (v >> 8); p[2] = (uint8) (v >> 16); p[3] = (uint8) (v >> 24);
	}
	else if ((addr >> 29) == 2)
		IoWrite(addr, v);
}

static void Write8 (uint32 addr, uint8 v)
{
	if ((addr >> 29) == 7)
		ram[addr & (sizeof(ram) - 1)] = v;
	else if ((addr >> 29) == 2)
		IoWrite(addr, v);
}

// An unaligned word load reads the aligned word rotated.
static inline uint32 LoadWord (uint32 addr)
{
	const uint32	v = Read32(addr);
	const uint32	rot = (addr & 3) * 8;
	return (rot ? (v >> rot) | (v << (32 - rot)) : v);
}

// ---- modes

static inline int Bank (uint32 mode)
{
	switch (mode & 0x1f)
	{
		case M_FIQ: return (1);
		case M_IRQ: return (2);
		case M_SVC: return (3);
		case M_ABT: return (4);
		case M_UND: return (5);
	}
	return (0);
}

static void SetCPSR (uint32 v)
{
	const int	ob = Bank(s.cpsr), nb = Bank(v);
	if (ob != nb)
	{
		s.bank_sp[ob] = s.r[13];
		s.bank_lr[ob] = s.r[14];
		s.r[13] = s.bank_sp[nb];
		s.r[14] = s.bank_lr[nb];
		const int	of = ob == 1, nf = nb == 1;
		if (of != nf)
		{
			memcpy(s.bank_r8[of], s.r + 8, sizeof(s.bank_r8[0]));
			memcpy(s.r + 8, s.bank_r8[nf], sizeof(s.bank_r8[0]));
		}
	}
	s.cpsr = v;
}

static inline uint32 SPSR (void)
{
	const int	b = Bank(s.cpsr);
	return (b ? s.spsr[b] : s.cpsr);
}

static void Exception (uint32 mode, uint32 vector)
{
	const uint32	old = s.cpsr;
	SetCPSR((old & ~0x1fu) | mode | F_I);
	s.spsr[Bank(mode)] = old;
	s.r[14] = cur + 4;
	s.r[15] = vector;
	s.clocks += 2;
}

// ---- instructions

static inline uint32 Reg (uint32 n)
{
	return ((n == 15) ? cur + 8 : s.r[n]);
}

static inline bool Cond (uint32 c)
{
	const uint32	n = s.cpsr >> 31, z = (s.cpsr >> 30) & 1, cf = (s.cpsr >> 29) & 1, v = (s.cpsr >> 28) & 1;
	switch (c)
	{
		case  0: return (z);
		case  1: return (!z);
		case  2: return (cf);
		case  3: return (!cf);
		case  4: return (n);
		case  5: return (!n);
		case  6: return (v);
		case  7: return (!v);
		case  8: return (cf && !z);
		case  9: return (!cf || z);
		case 10: return (n == v);
		case 11: return (n != v);
		case 12: return (!z && n == v);
		case 13: return (z || n != v);
		case 14: return (true);
	}
	return (false);		// NV
}

// The barrel shifter; `carry` comes in as C and leaves as the shifter's carry out.
static inline uint32 ShiftImm (uint32 v, uint32 type, uint32 amt, uint32 &carry)
{
	switch (type)
	{
		case 0:
			if (amt)
			{
				carry = (v >> (32 - amt)) & 1;
				v <<= amt;
			}
			return (v);
		case 1:
			if (!amt)
				amt = 32;
			carry = (v >> (amt - 1)) & 1;
			return ((amt == 32) ? 0 : v >> amt);
		case 2:
			if (!amt)
				amt = 32;
			carry = (v >> (amt - 1)) & 1;
			return ((uint32) ((int32) v >> ((amt == 32) ? 31 : amt)));
		default:
			if (!amt)
			{
				const uint32	c = v & 1;
				v = (carry << 31) | (v >> 1);	// RRX
				carry = c;
				return (v);
			}
			carry = (v >> (amt - 1)) & 1;
			return ((v >> amt) | (v << (32 - amt)));
	}
}

static inline uint32 ShiftReg (uint32 v, uint32 type, uint32 amt, uint32 &carry)
{
	if (!amt)
		return (v);
	switch (type)
	{
		case 0:
			if (amt < 32)
			{
				carry = (v >> (32 - amt)) & 1;
				return (v << amt);
			}
			carry = (amt == 32) ? (v & 1) : 0;
			return (0);
		case 1:
			if (amt < 32)
			{
				carry = (v >> (amt - 1)) & 1;
				return (v >> amt);
			}
			carry = (amt == 32) ? (v >> 31) : 0;
			return (0);
		case 2:
			if (amt < 32)
			{
				carry = (v >> (amt - 1)) & 1;
				return ((uint32) ((int32) v >> amt));
			}
			carry = v >> 31;
			return ((uint32) ((int32) v >> 31));
		default:
			amt &= 31;
			if (!amt)
			{
				carry = v >> 31;
				return (v);
			}
			carry = (v >> (amt - 1)) & 1;
			return ((v >> amt) | (v << (32 - amt)));
	}
}

static void DataProcessing (uint32 op)
{
	const uint32	opc = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	const bool		setflags = (op >> 20) & 1;
	uint32			carry = (s.cpsr >> 29) & 1, op2, a;

	if (op & (1u << 25))
	{
		const uint32	rot = ((op >> 8) & 15) * 2;
		op2 = op & 0xff;
		if (rot)
		{
			op2 = (op2 >> rot) | (op2 << (32 - rot));
			carry = op2 >> 31;
		}
		a = Reg(rn);
	}
	else if (op & 0x10)
	{
		// shift by a register: one internal cycle, and PC reads 12 ahead
		const uint32	rm = op & 15;
		op2 = ShiftReg((rm == 15) ? cur + 12 : s.r[rm], (op >> 5) & 3, s.r[(op >> 8) & 15] & 0xff, carry);
		a = (rn == 15) ? cur + 12 : s.r[rn];
		s.clocks++;
	}
	else
	{
		op2 = ShiftImm(Reg(op & 15), (op >> 5) & 3, (op >> 7) & 31, carry);
		a = Reg(rn);
	}

	const uint32	cin = (s.cpsr >> 29) & 1;
	uint32			res, v = (s.cpsr >> 28) & 1;
	bool			arith = true;

	switch (opc)
	{
		case  0: case 8:  res = a & op2; arith = false; break;			// AND, TST
		case  1: case 9:  res = a ^ op2; arith = false; break;			// EOR, TEQ
		case  2: case 10: res = a - op2; carry = a >= op2; v = ((a ^ op2) & (a ^ res)) >> 31; break;	// SUB, CMP
		case  3: res = op2 - a; carry = op2 >= a; v = ((op2 ^ a) & (op2 ^ res)) >> 31; break;			// RSB
		case  4: case 11: res = a + op2; carry = res < a; v = (~(a ^ op2) & (a ^ res)) >> 31; break;	// ADD, CMN
		case  5:																						// ADC
		{
			const uint64	t = (uint64) a + op2 + cin;
			res = (uint32) t; carry = (uint32) (t >> 32); v = (~(a ^ op2) & (a ^ res)) >> 31;
			break;
		}
		case  6:																						// SBC
			res = a - op2 - (cin ^ 1); carry = (uint64) a >= (uint64) op2 + (cin ^ 1); v = ((a ^ op2) & (a ^ res)) >> 31;
			break;
		case  7:																						// RSC
			res = op2 - a - (cin ^ 1); carry = (uint64) op2 >= (uint64) a + (cin ^ 1); v = ((op2 ^ a) & (op2 ^ res)) >> 31;
			break;
		case 12: res = a | op2; arith = false; break;					// ORR
		case 13: res = op2; arith = false; break;						// MOV
		case 14: res = a & ~op2; arith = false; break;					// BIC
		default: res = ~op2; arith = false; break;						// MVN
	}

	const bool	writes = opc < 8 || opc > 11;
	if (setflags)
	{
		if (rd == 15 && writes)
			SetCPSR(SPSR());	// a return from an exception
		else
			s.cpsr = (s.cpsr & 0x0fffffff) | (res & F_N) | (res ? 0 : F_Z) | (carry << 29) | ((arith ? v : (s.cpsr >> 28) & 1) << 28);
	}

	if (writes)
	{
		if (rd == 15)
		{
			s.r[15] = res & ~3u;
			s.clocks += 2;
		}
		else
			s.r[rd] = res;
	}
}

// MRS and MSR live in the TST/TEQ/CMP/CMN space with S clear.
static void StatusTransfer (uint32 op)
{
	const bool	spsr = (op >> 22) & 1;

	if (!(op & (1u << 21)))
	{
		s.r[(op >> 12) & 15] = spsr ? SPSR() : s.cpsr;		// MRS
		return;
	}

	uint32	v;
	if (op & (1u << 25))
	{
		const uint32	rot = ((op >> 8) & 15) * 2;
		v = op & 0xff;
		if (rot)
			v = (v >> rot) | (v << (32 - rot));
	}
	else
		v = s.r[op & 15];

	uint32	mask = 0;
	if (op & (1u << 19))
		mask |= 0xff000000;
	if ((op & (1u << 16)) && (s.cpsr & 0x1f) != M_USR)
		mask |= 0x000000ff;

	if (spsr)
	{
		const int	b = Bank(s.cpsr);
		if (b)
			s.spsr[b] = (s.spsr[b] & ~mask) | (v & mask);
	}
	else
		SetCPSR((s.cpsr & ~mask) | (v & mask) | 0x10);
}

static void Multiply (uint32 op)
{
	const uint32	rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = s.r[(op >> 8) & 15], rm = s.r[op & 15];
	uint32			res = rm * rs;

	// one internal cycle per 8 bits of Rs that aren't all sign
	uint32	m = 4;
	if ((rs & 0xffffff00) == 0 || (rs & 0xffffff00) == 0xffffff00) m = 1;
	else if ((rs & 0xffff0000) == 0 || (rs & 0xffff0000) == 0xffff0000) m = 2;
	else if ((rs & 0xff000000) == 0 || (rs & 0xff000000) == 0xff000000) m = 3;
	s.clocks += m;

	if (op & (1u << 21))
	{
		res += s.r[rn];		// MLA
		s.clocks++;
	}
	if (op & (1u << 20))
		s.cpsr = (s.cpsr & 0x3fffffff) | (res & F_N) | (res ? 0 : F_Z);
	s.r[rd] = res;
}

static void Swap (uint32 op)
{
	const uint32	addr = s.r[(op >> 16) & 15], rd = (op >> 12) & 15, v = s.r[op & 15];
	uint32			old;

	if (op & (1u << 22))
	{
		old = Read8(addr);
		Write8(addr, (uint8) v);
	}
	else
	{
		old = LoadWord(addr);
		Write32(addr, v);
	}
	s.r[rd] = old;
	s.clocks += 3;
}

static void SingleTransfer (uint32 op)
{
	const uint32	rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	const bool		pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	uint32			off;

	if (op & (1u << 25))
	{
		uint32	carry = (s.cpsr >> 29) & 1;
		off = ShiftImm(Reg(op & 15), (op >> 5) & 3, (op >> 7) & 31, carry);
	}
	else
		off = op & 0xfff;

	const uint32	base = Reg(rn);
	const uint32	moved = up ? base + off : base - off;
	const uint32	addr = pre ? moved : base;

	if (load)
	{
		const uint32	v = byte ? Read8(addr) : LoadWord(addr);
		if (!pre || wb)
			s.r[rn] = moved;
		if (rd == 15)
		{
			s.r[15] = v & ~3u;
			s.clocks += 2;
		}
		else
			s.r[rd] = v;
		s.clocks += 2;
	}
	else
	{
		const uint32	v = (rd == 15) ? cur + 12 : s.r[rd];
		if (byte)
			Write8(addr, (uint8) v);
		else
			Write32(addr, v);
		if (!pre || wb)
			s.r[rn] = moved;
		s.clocks += 1;
	}
}

static void BlockTransfer (uint32 op)
{
	const uint32	rn = (op >> 16) & 15, list = op & 0xffff;
	const bool		pre = (op >> 24) & 1, up = (op >> 23) & 1, psr = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	uint32			n = 0;

	for (uint32 i = 0; i < 16; i++)
		n += (list >> i) & 1;

	const uint32	base = s.r[rn];
	const uint32	span = n ? n * 4 : 0x40;
	const uint32	final = up ? base + span : base - span;
	uint32			addr = up ? (pre ? base + 4 : base) : (pre ? base - span : base - span + 4);

	// S without PC loaded: the user mode's registers
	const bool		user = psr && !(load && (list & 0x8000));
	const uint32	saved = s.cpsr;
	if (user)
		SetCPSR((s.cpsr & ~0x1fu) | M_USR);

	if (load)
	{
		if (wb)
			s.r[rn] = final;
		for (uint32 i = 0; i < 16; i++)
		{
			if (!((list >> i) & 1))
				continue;
			const uint32	v = Read32(addr);
			addr += 4;
			if (i == 15)
			{
				s.r[15] = v & ~3u;
				s.clocks += 2;
			}
			else
				s.r[i] = v;
		}
		s.clocks += n + 1;
		if (user)
			SetCPSR(saved);
		else if (psr && (list & 0x8000))
			SetCPSR(SPSR());
	}
	else
	{
		bool	first = true;
		for (uint32 i = 0; i < 16; i++)
		{
			if (!((list >> i) & 1))
				continue;
			// a written-back base stores its old value only as the first register
			const uint32	v = (i == 15) ? cur + 12 : (i == rn && wb && !first) ? final : s.r[i];
			Write32(addr, v);
			addr += 4;
			first = false;
		}
		if (user)
			SetCPSR(saved);
		if (wb)
			s.r[rn] = final;
		s.clocks += n;
	}
}

static void Step (void)
{
	cur = s.r[15];
	uint32	op;
	if ((cur >> 29) == 0)
	{
		const uint8	*p = prog + (cur & (ST018_PROGRAM_SIZE - 1) & ~3u);
		op = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32) p[3] << 24);
	}
	else
		op = Read32(cur);
	s.last_fetch = op;
	s.r[15] = cur + 4;
	s.clocks++;

	if (!Cond(op >> 28))
		return;

	switch ((op >> 25) & 7)
	{
		case 0:
			if ((op & 0x0fc000f0) == 0x00000090)
				Multiply(op);
			else if ((op & 0x0fb00ff0) == 0x01000090)
				Swap(op);
			else if ((op & 0x90) == 0x90)
				Exception(M_UND, 0x04);			// ARMv4's halfword and long multiply space
			else if ((op & 0x01900000) == 0x01000000)
				StatusTransfer(op);
			else
				DataProcessing(op);
			break;
		case 1:
			if ((op & 0x01900000) == 0x01000000)
				StatusTransfer(op);
			else
				DataProcessing(op);
			break;
		case 2:
			SingleTransfer(op);
			break;
		case 3:
			if (op & 0x10)
				Exception(M_UND, 0x04);
			else
				SingleTransfer(op);
			break;
		case 4:
			BlockTransfer(op);
			break;
		case 5:
			if (op & (1u << 24))
				s.r[14] = cur + 4;				// BL
			s.r[15] = cur + 8 + (uint32) (((int32) (op << 8)) >> 6);
			s.clocks += 2;
			break;
		case 6:
			Exception(M_UND, 0x04);				// no coprocessor answers
			break;
		default:
			if (op & (1u << 24))
				Exception(M_SVC, 0x08);			// SWI
			else
				Exception(M_UND, 0x04);
			break;
	}
}

// Up to `target`: held in reset, the reset delay, then instructions.
static void Run (void)
{
	while (s.clocks < s.target)
	{
		if (s.reset)
		{
			s.clocks = s.target;
			break;
		}
		if (s.boot_wait)
		{
			const uint64	n = s.target - s.clocks;
			if (n < s.boot_wait)
			{
				s.boot_wait -= (uint32) n;
				s.clocks = s.target;
				break;
			}
			s.clocks += s.boot_wait;
			s.boot_wait = 0;
			s.ready = 1;
			continue;
		}
		Step();
	}
}

static void SyncTo (int32 cycles)
{
	const uint64	t = s.line_base + (uint64) (int64) cycles;
	if (t <= s.synced)
		return;

	const uint64	master = Settings.PAL ? 21281370 : 21477273;
	const uint64	acc = (t - s.synced) * ST018_HZ + s.rem;
	s.synced = t;
	s.target += acc / master;
	s.rem = acc % master;
	Run();
}

// The ARM never parks, so it runs to each line's end: a long gap between accesses
// (the game leaves it alone for its first ten seconds) isn't one burst of catch-up.
void S9xST018EndScanline (void)
{
	SyncTo(Timings.H_Max);
	s.line_base += Timings.H_Max;
}

// $3800 the ARM's byte, $3802 clears its signal, $3804 the status; A0 doesn't decode.
uint8 S9xST018Read (uint32 address, int32 speed)
{
	if ((address & 0xff00) != 0x3800)
		return (OpenBus);

	const uint32	a = address & 0xff06;
	if (speed < 0)
		return ((a == 0x3800) ? (s.armtocpu_ready ? s.armtocpu_data : 0) : (a == 0x3804) ? Status() : 0);
	SyncTo(CPU.Cycles + speed - 1);

	switch (a)
	{
		case 0x3800:
			if (s.armtocpu_ready)
			{
				s.armtocpu_ready = 0;
				return (s.armtocpu_data);
			}
			return (0);
		case 0x3802:
			s.signal = 0;
			return (0);
		case 0x3804:
			return (Status());
	}
	return (0);
}

// $3802 a byte for the ARM, $3804 bit 0 holds it in reset (power-on on the rising edge).
void S9xST018Write (uint32 address, uint8 byte, int32 speed)
{
	if (speed < 0 || (address & 0xff00) != 0x3800)
		return;
	SyncTo(CPU.Cycles + speed);

	switch (address & 0xff06)
	{
		case 0x3802:
			s.cputoarm_ready = 1;
			s.cputoarm_data = byte;
			break;
		case 0x3804:
			byte &= 1;
			if (!s.reset && byte)
				PowerArm();
			s.reset = byte;
			break;
	}
}

uint32 S9xST018StateSize (void)
{
	return (sizeof(s) + sizeof(ram));
}

void S9xST018StateSave (uint8 *buf)
{
	memcpy(buf, &s, sizeof(s));
	memcpy(buf + sizeof(s), ram, sizeof(ram));
}

bool8 S9xST018StateLoad (const uint8 *buf, uint32 size)
{
	if (!loaded || size != sizeof(s) + sizeof(ram))
		return (FALSE);
	memcpy(&s, buf, sizeof(s));
	memcpy(ram, buf + sizeof(s), sizeof(ram));
	active = TRUE;
	return (TRUE);
}
