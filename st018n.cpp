/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native ST018 (Hayazashi Nidan Morita Shougi 2): the ARM's program translated to C, clock
// for clock as st018.cpp runs it (st018n_code.h, from s9x-harness/chipbench/st018/armc.py),
// with its ROMs built in. The ARM's RAM is its own, so it runs ahead of the CPU between
// bridge accesses and stops before any the CPU could see that the clock hasn't reached.
// Its idle loop, polling the status register, repeats exactly: whole turns of it are skipped.

#include <string.h>
#include "snes9x.h"
#include "st018.h"

namespace {

#include "st018n_rom.h"

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b };
enum { F_N = 0x80000000u, F_Z = 0x40000000u, F_C = 0x20000000u, F_V = 0x10000000u, F_I = 0x80 };

// The idle loop, as found: a status poll that came round to the same state.
struct Idle
{
	S9xST018Arm		arm;
	S9xST018Bus		bus;
	uint64			period;
	uint8			idle, at_poll, pad[6];
};

S9xST018Arm		a;
Idle			idle;
S9xST018Bus		*bus = NULL;
uint8			*ram = NULL;
uint64			target, limit;
bool			ramdirty;			// a RAM byte changed since the idle snapshot
uint32			sync_addr, sync_load;	// the bridge access the run stopped before
#ifdef ST018_LAB
}
uint64	S9xST018NativeFallbacks = 0;	// instructions the translation didn't have (the lab's alarm)
uint64	S9xST018NativeStrays = 0;		// reads of unmapped addresses (the lab's alarm: the program never makes them)
namespace {
#define LAB_COUNT(x)	(x++)
#else
#define LAB_COUNT(x)	do { } while (0)
#endif

#define FN	(a.cpsr >> 31)
#define FZ	((a.cpsr >> 30) & 1)
#define FC	((a.cpsr >> 29) & 1)
#define FV	((a.cpsr >> 28) & 1)

const uint32	kRunAhead = 1 << 22;	// clocks it may run past the target between bridge accesses

// ---- the bridge, as st018.cpp's IoRead / IoWrite

inline uint8 Status (void)
{
	return ((uint8) (bus->armtocpu_ready | (bus->signal << 2) | (bus->cputoarm_ready << 3) | (bus->ready << 7)));
}

uint32 IoRead (uint32 addr)
{
	switch (addr & 0xe000003f)
	{
		case 0x40000010:
			if (bus->cputoarm_ready)
			{
				bus->cputoarm_ready = 0;
				return (bus->cputoarm_data);
			}
			return (0);
		case 0x40000020:
			return (Status());
	}
	return (0);
}

void IoWrite (uint32 addr, uint32 v)
{
	v &= 0xff;
	switch (addr & 0xe000003f)
	{
		case 0x40000000: bus->armtocpu_ready = 1; bus->armtocpu_data = (uint8) v; break;
		case 0x40000010: bus->signal = 1; break;
		case 0x40000020: bus->timerlatch = (bus->timerlatch & 0xffff00) | v; break;
		case 0x40000024: bus->timerlatch = (bus->timerlatch & 0xff00ff) | (v << 8); break;
		case 0x40000028: bus->timerlatch = (bus->timerlatch & 0x00ffff) | (v << 16); break;
		case 0x4000002c: bus->timer = bus->timerlatch; break;
	}
}

// A bridge access the CPU can see: the ARM's byte or signal out, the CPU's byte or the status in. The run
// stops before it if the clock hasn't reached it.
inline bool BridgeAt (uint32 addr, uint32 load)
{
	if ((addr >> 29) != 2)
		return (false);
	const uint32	reg = addr & 0x3f;
	if (load ? (reg != 0x10 && reg != 0x20) : (reg != 0x00 && reg != 0x10))
		return (false);
	sync_addr = addr;
	sync_load = load;
	return (true);
}

// ---- the ARM's bus, as st018.cpp's. `op`: the instruction's own word, which an unmapped read returns (the
// interpreter's last fetch)

inline uint32 Read32 (uint32 addr, uint32 op)
{
	switch (addr >> 29)
	{
		case 7: { const uint8 *p = ram + (addr & 0x3ffc); return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32) p[3] << 24)); }
		case 0: return (kProg[(addr & (ST018_PROGRAM_SIZE - 1)) >> 2]);
		case 5: { const uint8 *p = kData + (addr & (ST018_DATA_SIZE - 4)); return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32) p[3] << 24)); }
		case 2: return (IoRead(addr));
		case 3: return (0x40404001);
	}
	LAB_COUNT(S9xST018NativeStrays);
	return (op);
}

inline uint8 Load8 (uint32 addr, uint32 op)
{
	switch (addr >> 29)
	{
		case 7: return (ram[addr & 0x3fff]);
		case 0: return ((uint8) (kProg[(addr & (ST018_PROGRAM_SIZE - 1)) >> 2] >> ((addr & 3) * 8)));
		case 5: return (kData[addr & (ST018_DATA_SIZE - 1)]);
	}
	return ((uint8) Read32(addr, op));
}

inline uint32 LoadWord (uint32 addr, uint32 op)
{
	const uint32	v = Read32(addr, op);
	const uint32	rot = (addr & 3) * 8;
	return (rot ? (v >> rot) | (v << (32 - rot)) : v);
}

inline void Store32 (uint32 addr, uint32 v)
{
	if ((addr >> 29) == 7)
	{
		uint8	*p = ram + (addr & 0x3ffc);
		const uint32	old = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32) p[3] << 24);
		if (old != v)
		{
			ramdirty = true;
			p[0] = (uint8) v; p[1] = (uint8) (v >> 8); p[2] = (uint8) (v >> 16); p[3] = (uint8) (v >> 24);
		}
	}
	else if ((addr >> 29) == 2)
		IoWrite(addr, v);
}

inline void Store8 (uint32 addr, uint8 v)
{
	if ((addr >> 29) == 7)
	{
		uint8	&b = ram[addr & 0x3fff];
		if (b != v)
		{
			ramdirty = true;
			b = v;
		}
	}
	else if ((addr >> 29) == 2)
		IoWrite(addr, v);
}

// ---- modes and the ALU, as st018.cpp's

inline int Bank (uint32 mode)
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

void SetCPSR (uint32 v)
{
	const int	ob = Bank(a.cpsr), nb = Bank(v);
	if (ob != nb)
	{
		a.bank_sp[ob] = a.r[13];
		a.bank_lr[ob] = a.r[14];
		a.r[13] = a.bank_sp[nb];
		a.r[14] = a.bank_lr[nb];
		const int	of = ob == 1, nf = nb == 1;
		if (of != nf)
		{
			memcpy(a.bank_r8[of], a.r + 8, sizeof(a.bank_r8[0]));
			memcpy(a.r + 8, a.bank_r8[nf], sizeof(a.bank_r8[0]));
		}
	}
	a.cpsr = v;
}

inline uint32 SPSR (void)
{
	const int	b = Bank(a.cpsr);
	return (b ? a.spsr[b] : a.cpsr);
}

void Exception (uint32 mode, uint32 vector, uint32 cur)
{
	const uint32	old = a.cpsr;
	SetCPSR((old & ~0x1fu) | mode | F_I);
	a.spsr[Bank(mode)] = old;
	a.r[14] = cur + 4;
	a.r[15] = vector;
	a.clocks += 2;
}

inline uint32 ShiftImm (uint32 v, uint32 type, uint32 amt, uint32 &carry)
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
				v = (carry << 31) | (v >> 1);
				carry = c;
				return (v);
			}
			carry = (v >> (amt - 1)) & 1;
			return ((v >> amt) | (v << (32 - amt)));
	}
}

inline uint32 ShiftReg (uint32 v, uint32 type, uint32 amt, uint32 &carry)
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

// Data processing: the result, and the flags if S. `carry` is the shifter's carry out.
template <int OPC, bool S>
inline uint32 Alu (uint32 x, uint32 op2, uint32 carry)
{
	const uint32	cin = FC;
	uint32			res, v = FV;
	bool			arith = true;
	switch (OPC)
	{
		case  0: case 8:  res = x & op2; arith = false; break;
		case  1: case 9:  res = x ^ op2; arith = false; break;
		case  2: case 10: res = x - op2; carry = x >= op2; v = ((x ^ op2) & (x ^ res)) >> 31; break;
		case  3: res = op2 - x; carry = op2 >= x; v = ((op2 ^ x) & (op2 ^ res)) >> 31; break;
		case  4: case 11: res = x + op2; carry = res < x; v = (~(x ^ op2) & (x ^ res)) >> 31; break;
		case  5:
		{
			const uint64	t = (uint64) x + op2 + cin;
			res = (uint32) t; carry = (uint32) (t >> 32); v = (~(x ^ op2) & (x ^ res)) >> 31;
			break;
		}
		case  6: res = x - op2 - (cin ^ 1); carry = (uint64) x >= (uint64) op2 + (cin ^ 1); v = ((x ^ op2) & (x ^ res)) >> 31; break;
		case  7: res = op2 - x - (cin ^ 1); carry = (uint64) op2 >= (uint64) x + (cin ^ 1); v = ((op2 ^ x) & (op2 ^ res)) >> 31; break;
		case 12: res = x | op2; arith = false; break;
		case 13: res = op2; arith = false; break;
		case 14: res = x & ~op2; arith = false; break;
		default: res = ~op2; arith = false; break;
	}
	if (S)
		a.cpsr = (a.cpsr & 0x0fffffff) | (res & F_N) | (res ? 0 : F_Z) | (carry << 29) | ((arith ? v : FV) << 28);
	return (res);
}

void StatusTransfer (uint32 op)
{
	const bool	spsr = (op >> 22) & 1;
	if (!(op & (1u << 21)))
	{
		a.r[(op >> 12) & 15] = spsr ? SPSR() : a.cpsr;
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
		v = a.r[op & 15];
	uint32	mask = 0;
	if (op & (1u << 19))
		mask |= 0xff000000;
	if ((op & (1u << 16)) && (a.cpsr & 0x1f) != M_USR)
		mask |= 0x000000ff;
	if (spsr)
	{
		const int	b = Bank(a.cpsr);
		if (b)
			a.spsr[b] = (a.spsr[b] & ~mask) | (v & mask);
	}
	else
		SetCPSR((a.cpsr & ~mask) | (v & mask) | 0x10);
}

inline void Multiply (uint32 op)
{
	const uint32	rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = a.r[(op >> 8) & 15], rm = a.r[op & 15];
	uint32			res = rm * rs;
	uint32			m = 4;
	if ((rs & 0xffffff00) == 0 || (rs & 0xffffff00) == 0xffffff00) m = 1;
	else if ((rs & 0xffff0000) == 0 || (rs & 0xffff0000) == 0xffff0000) m = 2;
	else if ((rs & 0xff000000) == 0 || (rs & 0xff000000) == 0xff000000) m = 3;
	a.clocks += m;
	if (op & (1u << 21))
	{
		res += a.r[rn];
		a.clocks++;
	}
	if (op & (1u << 20))
		a.cpsr = (a.cpsr & 0x3fffffff) | (res & F_N) | (res ? 0 : F_Z);
	a.r[rd] = res;
}

void Swap (uint32 op)
{
	const uint32	addr = a.r[(op >> 16) & 15], rd = (op >> 12) & 15, v = a.r[op & 15];
	uint32			old;
	if (op & (1u << 22))
	{
		old = Load8(addr, op);
		Store8(addr, (uint8) v);
	}
	else
	{
		old = LoadWord(addr, op);
		Store32(addr, v);
	}
	a.r[rd] = old;
	a.clocks += 3;
}

void BlockTransfer (uint32 op, uint32 cur)
{
	const uint32	rn = (op >> 16) & 15, list = op & 0xffff;
	const bool		pre = (op >> 24) & 1, up = (op >> 23) & 1, psr = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	uint32			n = 0;
	for (uint32 i = 0; i < 16; i++)
		n += (list >> i) & 1;
	const uint32	base = a.r[rn];
	const uint32	span = n ? n * 4 : 0x40;
	const uint32	final = up ? base + span : base - span;
	uint32			addr = up ? (pre ? base + 4 : base) : (pre ? base - span : base - span + 4);
	const bool		user = psr && !(load && (list & 0x8000));
	const uint32	saved = a.cpsr;
	if (user)
		SetCPSR((a.cpsr & ~0x1fu) | M_USR);
	if (load)
	{
		if (wb)
			a.r[rn] = final;
		for (uint32 i = 0; i < 16; i++)
		{
			if (!((list >> i) & 1))
				continue;
			const uint32	v = Read32(addr, op);
			addr += 4;
			if (i == 15)
			{
				a.r[15] = v & ~3u;
				a.clocks += 2;
			}
			else
				a.r[i] = v;
		}
		a.clocks += n + 1;
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
			const uint32	v = (i == 15) ? cur + 12 : (i == rn && wb && !first) ? final : a.r[i];
			Store32(addr, v);
			addr += 4;
			first = false;
		}
		if (user)
			SetCPSR(saved);
		if (wb)
			a.r[rn] = final;
		a.clocks += n;
	}
}

#if defined(ST018_LAB) && defined(ST018N_CODE)
#include ST018N_CODE
#else
#include "st018n_code.h"
#endif

// ---- st018.cpp's interpreter, for a word the translation doesn't have (outside the command layer, so never at
// the bridge)

inline bool Cond (uint32 c)
{
	switch (c)
	{
		case  0: return (FZ);
		case  1: return (!FZ);
		case  2: return (FC);
		case  3: return (!FC);
		case  4: return (FN);
		case  5: return (!FN);
		case  6: return (FV);
		case  7: return (!FV);
		case  8: return (FC && !FZ);
		case  9: return (!FC || FZ);
		case 10: return (FN == FV);
		case 11: return (FN != FV);
		case 12: return (!FZ && FN == FV);
		case 13: return (FZ || FN != FV);
		case 14: return (true);
	}
	return (false);
}

void DataProcessing (uint32 op, uint32 cur)
{
	const uint32	opc = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	const bool		setflags = (op >> 20) & 1;
	uint32			carry = FC, op2, x;
	if (op & (1u << 25))
	{
		const uint32	rot = ((op >> 8) & 15) * 2;
		op2 = op & 0xff;
		if (rot)
		{
			op2 = (op2 >> rot) | (op2 << (32 - rot));
			carry = op2 >> 31;
		}
		x = (rn == 15) ? cur + 8 : a.r[rn];
	}
	else if (op & 0x10)
	{
		const uint32	rm = op & 15;
		op2 = ShiftReg((rm == 15) ? cur + 12 : a.r[rm], (op >> 5) & 3, a.r[(op >> 8) & 15] & 0xff, carry);
		x = (rn == 15) ? cur + 12 : a.r[rn];
		a.clocks++;
	}
	else
	{
		const uint32	rm = op & 15;
		op2 = ShiftImm((rm == 15) ? cur + 8 : a.r[rm], (op >> 5) & 3, (op >> 7) & 31, carry);
		x = (rn == 15) ? cur + 8 : a.r[rn];
	}
	const bool	writes = opc < 8 || opc > 11;
	uint32		res;
	const bool	flags = setflags && !(rd == 15 && writes);
	switch (opc)
	{
		case  0: res = flags ? Alu<0, true>(x, op2, carry) : Alu<0, false>(x, op2, carry); break;
		case  1: res = flags ? Alu<1, true>(x, op2, carry) : Alu<1, false>(x, op2, carry); break;
		case  2: res = flags ? Alu<2, true>(x, op2, carry) : Alu<2, false>(x, op2, carry); break;
		case  3: res = flags ? Alu<3, true>(x, op2, carry) : Alu<3, false>(x, op2, carry); break;
		case  4: res = flags ? Alu<4, true>(x, op2, carry) : Alu<4, false>(x, op2, carry); break;
		case  5: res = flags ? Alu<5, true>(x, op2, carry) : Alu<5, false>(x, op2, carry); break;
		case  6: res = flags ? Alu<6, true>(x, op2, carry) : Alu<6, false>(x, op2, carry); break;
		case  7: res = flags ? Alu<7, true>(x, op2, carry) : Alu<7, false>(x, op2, carry); break;
		case  8: res = flags ? Alu<8, true>(x, op2, carry) : Alu<8, false>(x, op2, carry); break;
		case  9: res = flags ? Alu<9, true>(x, op2, carry) : Alu<9, false>(x, op2, carry); break;
		case 10: res = flags ? Alu<10, true>(x, op2, carry) : Alu<10, false>(x, op2, carry); break;
		case 11: res = flags ? Alu<11, true>(x, op2, carry) : Alu<11, false>(x, op2, carry); break;
		case 12: res = flags ? Alu<12, true>(x, op2, carry) : Alu<12, false>(x, op2, carry); break;
		case 13: res = flags ? Alu<13, true>(x, op2, carry) : Alu<13, false>(x, op2, carry); break;
		case 14: res = flags ? Alu<14, true>(x, op2, carry) : Alu<14, false>(x, op2, carry); break;
		default: res = flags ? Alu<15, true>(x, op2, carry) : Alu<15, false>(x, op2, carry); break;
	}
	if (setflags && rd == 15 && writes)
		SetCPSR(SPSR());
	if (writes)
	{
		if (rd == 15)
		{
			a.r[15] = res & ~3u;
			a.clocks += 2;
		}
		else
			a.r[rd] = res;
	}
}

void SingleTransfer (uint32 op, uint32 cur)
{
	const uint32	rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	const bool		pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	uint32			off;
	if (op & (1u << 25))
	{
		uint32	carry = FC;
		const uint32	rm = op & 15;
		off = ShiftImm((rm == 15) ? cur + 8 : a.r[rm], (op >> 5) & 3, (op >> 7) & 31, carry);
	}
	else
		off = op & 0xfff;
	const uint32	base = (rn == 15) ? cur + 8 : a.r[rn];
	const uint32	moved = up ? base + off : base - off;
	const uint32	addr = pre ? moved : base;
	if (load)
	{
		const uint32	v = byte ? Load8(addr, op) : LoadWord(addr, op);
		if (!pre || wb)
			a.r[rn] = moved;
		if (rd == 15)
		{
			a.r[15] = v & ~3u;
			a.clocks += 2;
		}
		else
			a.r[rd] = v;
		a.clocks += 2;
	}
	else
	{
		const uint32	v = (rd == 15) ? cur + 12 : a.r[rd];
		if (byte)
			Store8(addr, (uint8) v);
		else
			Store32(addr, v);
		if (!pre || wb)
			a.r[rn] = moved;
		a.clocks += 1;
	}
}

void Interpret (void)
{
	const uint32	cur = a.r[15];
	const uint32	op = ((cur >> 29) == 0) ? kProg[(cur & (ST018_PROGRAM_SIZE - 1)) >> 2] : Read32(cur, a.last_fetch);
	a.last_fetch = op;
	a.r[15] = cur + 4;
	a.clocks++;
	LAB_COUNT(S9xST018NativeFallbacks);
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
				Exception(M_UND, 0x04, cur);
			else if ((op & 0x01900000) == 0x01000000)
				StatusTransfer(op);
			else
				DataProcessing(op, cur);
			break;
		case 1:
			if ((op & 0x01900000) == 0x01000000)
				StatusTransfer(op);
			else
				DataProcessing(op, cur);
			break;
		case 2:
			SingleTransfer(op, cur);
			break;
		case 3:
			if (op & 0x10)
				Exception(M_UND, 0x04, cur);
			else
				SingleTransfer(op, cur);
			break;
		case 4:
			BlockTransfer(op, cur);
			break;
		case 5:
			if (op & (1u << 24))
				a.r[14] = cur + 4;
			a.r[15] = cur + 8 + (uint32) (((int32) (op << 8)) >> 6);
			a.clocks += 2;
			break;
		case 6:
			Exception(M_UND, 0x04, cur);
			break;
		default:
			Exception((op & (1u << 24)) ? M_SVC : M_UND, (op & (1u << 24)) ? 0x08 : 0x04, cur);
			break;
	}
}

// Runs from PC to the target (or past it, to the next bridge access). TRUE: stopped before a bridge access.
bool Execute (void)
{
	for (;;)
	{
		const uint32	pc = a.r[15];
		int				r = 2;
		if (pc < ST018_PROGRAM_SIZE && (kTranslated[pc >> 7] >> ((pc >> 2) & 31) & 1))
			r = kChunks[pc >> 12](pc);
		if (r == 1)
			return (true);
		if (r == 2)
			Interpret();
		if (a.clocks >= limit)
			return (false);
	}
}

inline bool SameArm (const S9xST018Arm &x, const S9xST018Arm &y)
{
	return (!memcmp(x.r, y.r, sizeof(x.r)) && !memcmp(x.bank_r8, y.bank_r8, sizeof(x.bank_r8)) &&
			!memcmp(x.bank_sp, y.bank_sp, sizeof(x.bank_sp)) && !memcmp(x.bank_lr, y.bank_lr, sizeof(x.bank_lr)) &&
			!memcmp(x.spsr, y.spsr, sizeof(x.spsr)) && x.cpsr == y.cpsr);
}

inline bool SameBus (const S9xST018Bus &x, const S9xST018Bus &y)
{
	return (!memcmp(&x, &y, sizeof(x)));
}

// Runs to `to`; the clock of its next CPU-visible access, as S9xST018NativeRun returns it.
uint64 RunTo (uint64 to)
{
	target = to;
	limit = ((a.clocks > to) ? a.clocks : to) + kRunAhead;
	if (Execute())
	{
		idle.at_poll = sync_load && (sync_addr & 0x3f) == 0x20;
		return (a.clocks);
	}
	idle.at_poll = 0;
	return (a.clocks);
}

}

void S9xST018NativeAttach (S9xST018Bus *b, uint8 *r)
{
	bus = b;
	ram = r;
}

void S9xST018NativeSetArm (const S9xST018Arm *arm)
{
	a = *arm;
	memset(&idle, 0, sizeof(idle));
	ramdirty = true;
}

void S9xST018NativeGetArm (S9xST018Arm *arm)
{
	*arm = a;
}

uint64 S9xST018NativeRun (uint64 to)
{
	// held in reset, the clock just goes by; the reset sequence's delay, then the ready flag
	if (bus->reset)
	{
		if (a.clocks < to)
			a.clocks = to;
		return (~(uint64) 0);
	}
	if (a.boot_wait)
	{
		if (to <= a.clocks)
			return (a.clocks + a.boot_wait);
		const uint64	n = to - a.clocks;
		if (n < a.boot_wait)
		{
			a.boot_wait -= (uint32) n;
			a.clocks = to;
			return (a.clocks + a.boot_wait);
		}
		a.clocks += a.boot_wait;
		a.boot_wait = 0;
		bus->ready = 1;
	}

	// at its idle poll: whole turns go by as long as nothing the ARM sees has changed
	if (idle.idle)
	{
		if (SameBus(idle.bus, *bus))
		{
			if (a.clocks < to)
				a.clocks += (to - a.clocks + idle.period - 1) / idle.period * idle.period;
			return (~(uint64) 0);
		}
		idle.idle = 0;
	}
	if (a.clocks >= to)
		return (a.clocks);

	// stopped at a status poll: one turn, to see whether it's the idle loop
	if (idle.at_poll)
	{
		idle.arm = a;
		idle.bus = *bus;
		ramdirty = false;
		const uint32	pc = a.r[15];
		const uint64	start = a.clocks;
		RunTo(start + 1);
		if (idle.at_poll && a.r[15] == pc && !ramdirty && SameArm(a, idle.arm) && SameBus(*bus, idle.bus))
		{
			idle.period = a.clocks - start;
			idle.idle = 1;
			if (a.clocks < to)
				a.clocks += (to - a.clocks + idle.period - 1) / idle.period * idle.period;
			return (~(uint64) 0);
		}
		if (a.clocks >= to)
			return (a.clocks);
	}
	return (RunTo(to));
}

#ifdef ST018_LAB
// One instruction, with no target to stop at: the single-step build's chunks return after each.
void S9xST018NativeLabStep (void)
{
	target = limit = ~(uint64) 0;
	const uint32	pc = a.r[15];
	int				r = 2;
	if (pc < ST018_PROGRAM_SIZE && (kTranslated[pc >> 7] >> ((pc >> 2) & 31) & 1))
		r = kChunks[pc >> 12](pc);
	if (r == 2)
		Interpret();
}
#endif
