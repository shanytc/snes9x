/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_OPS_H_
#define _SGB_GB_OPS_H_

#include <cstdint>

#include "gb_cpu.h"
#include "gb_memory.h"

namespace SGB {

// Dispatch a single non-CB opcode. Caller has already fetched `op` and
// advanced PC past it. Adds the exact T-cycle count for the instruction
// to state.t_cycles. The CB prefix (0xCB) is handled here too — it pulls
// one more byte from memory and dispatches into the CB table.
void Dispatch(CpuState &s, Memory &mem, uint8_t op);

// CB-prefixed opcodes live in gb_ops_cb.cpp (P1c).
void DispatchCB(CpuState &s, Memory &mem, uint8_t op);

// Fetch helpers — post-increment PC.
inline uint8_t Fetch8(CpuState &s, Memory &mem)
{
	uint8_t b = MemRead(mem, s.r.pc);
	s.r.pc++;
	return b;
}

inline uint16_t Fetch16(CpuState &s, Memory &mem)
{
	uint16_t lo = Fetch8(s, mem);
	uint16_t hi = Fetch8(s, mem);
	return static_cast<uint16_t>(lo | (hi << 8));
}

inline void Push16(CpuState &s, Memory &mem, uint16_t v)
{
	s.r.sp -= 2;
	MemWrite16(mem, s.r.sp, v);
}

inline uint16_t Pop16(CpuState &s, Memory &mem)
{
	uint16_t v = MemRead16(mem, s.r.sp);
	s.r.sp += 2;
	return v;
}

// Flag helpers. GB F layout: Z N H C 0 0 0 0.
inline bool FlagZ(const CpuState &s) { return (s.r.f & FLAG_Z) != 0; }
inline bool FlagN(const CpuState &s) { return (s.r.f & FLAG_N) != 0; }
inline bool FlagH(const CpuState &s) { return (s.r.f & FLAG_H) != 0; }
inline bool FlagC(const CpuState &s) { return (s.r.f & FLAG_C) != 0; }

inline void SetFlags(CpuState &s, bool z, bool n, bool h, bool c)
{
	s.r.f = static_cast<uint8_t>(
		(z ? FLAG_Z : 0) |
		(n ? FLAG_N : 0) |
		(h ? FLAG_H : 0) |
		(c ? FLAG_C : 0));
}

// ------------------------------------------------------------------
// 8-bit ALU
// ------------------------------------------------------------------

inline uint8_t AluAdd(CpuState &s, uint8_t a, uint8_t b)
{
	uint16_t sum = static_cast<uint16_t>(a) + b;
	uint8_t  r   = static_cast<uint8_t>(sum);
	SetFlags(s, r == 0, false, ((a & 0xF) + (b & 0xF)) > 0xF, sum > 0xFF);
	return r;
}

inline uint8_t AluAdc(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t  c   = FlagC(s) ? 1 : 0;
	uint16_t sum = static_cast<uint16_t>(a) + b + c;
	uint8_t  r   = static_cast<uint8_t>(sum);
	SetFlags(s, r == 0, false, ((a & 0xF) + (b & 0xF) + c) > 0xF, sum > 0xFF);
	return r;
}

inline uint8_t AluSub(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t r = static_cast<uint8_t>(a - b);
	SetFlags(s, r == 0, true, (a & 0xF) < (b & 0xF), a < b);
	return r;
}

inline uint8_t AluSbc(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t c    = FlagC(s) ? 1 : 0;
	int     diff = static_cast<int>(a) - b - c;
	uint8_t r    = static_cast<uint8_t>(diff & 0xFF);
	SetFlags(s, r == 0, true,
	         (static_cast<int>(a & 0xF) - (b & 0xF) - c) < 0,
	         diff < 0);
	return r;
}

inline uint8_t AluAnd(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t r = a & b;
	SetFlags(s, r == 0, false, true, false);
	return r;
}

inline uint8_t AluOr(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t r = a | b;
	SetFlags(s, r == 0, false, false, false);
	return r;
}

inline uint8_t AluXor(CpuState &s, uint8_t a, uint8_t b)
{
	uint8_t r = a ^ b;
	SetFlags(s, r == 0, false, false, false);
	return r;
}

inline void AluCp(CpuState &s, uint8_t a, uint8_t b)
{
	// Same math as SUB, result discarded.
	SetFlags(s, a == b, true, (a & 0xF) < (b & 0xF), a < b);
}

inline uint8_t AluInc(CpuState &s, uint8_t v)
{
	uint8_t r = static_cast<uint8_t>(v + 1);
	// C preserved, Z=(r==0), N=0, H=((v & 0xF) == 0xF).
	s.r.f = static_cast<uint8_t>(
		(s.r.f & FLAG_C) |
		(r == 0       ? FLAG_Z : 0) |
		((v & 0xF) == 0xF ? FLAG_H : 0));
	return r;
}

inline uint8_t AluDec(CpuState &s, uint8_t v)
{
	uint8_t r = static_cast<uint8_t>(v - 1);
	// C preserved, Z=(r==0), N=1, H=((v & 0xF) == 0).
	s.r.f = static_cast<uint8_t>(
		(s.r.f & FLAG_C) |
		(r == 0        ? FLAG_Z : 0) |
		FLAG_N |
		((v & 0xF) == 0 ? FLAG_H : 0));
	return r;
}

// ------------------------------------------------------------------
// 16-bit ALU
// ------------------------------------------------------------------

inline uint16_t AluAdd16(CpuState &s, uint16_t a, uint16_t b)
{
	uint32_t sum = static_cast<uint32_t>(a) + b;
	// Z preserved. N=0. H from bit-11 carry. C from bit-15 carry.
	s.r.f = static_cast<uint8_t>(
		(s.r.f & FLAG_Z) |
		(((a & 0xFFF) + (b & 0xFFF)) > 0xFFF ? FLAG_H : 0) |
		(sum > 0xFFFF ? FLAG_C : 0));
	return static_cast<uint16_t>(sum);
}

inline uint16_t AluAddSpE(CpuState &s, uint16_t sp, int8_t e)
{
	uint16_t eu = static_cast<uint16_t>(static_cast<uint8_t>(e));
	uint16_t r  = static_cast<uint16_t>(sp + static_cast<int16_t>(e));
	// Z=0, N=0, H from low-nibble carry of (sp & 0xFF)+(e as uint8),
	// C from byte carry. Signs don't matter — carry logic uses unsigned.
	SetFlags(s, false, false,
	         ((sp & 0xF)  + (eu & 0xF))  > 0xF,
	         ((sp & 0xFF) + (eu & 0xFF)) > 0xFF);
	return r;
}

// ------------------------------------------------------------------
// Rotate / misc on A
// ------------------------------------------------------------------

inline void AluRlca(CpuState &s)
{
	uint8_t c = static_cast<uint8_t>((s.r.a >> 7) & 1);
	s.r.a = static_cast<uint8_t>((s.r.a << 1) | c);
	SetFlags(s, false, false, false, c != 0);
}

inline void AluRrca(CpuState &s)
{
	uint8_t c = static_cast<uint8_t>(s.r.a & 1);
	s.r.a = static_cast<uint8_t>((s.r.a >> 1) | (c << 7));
	SetFlags(s, false, false, false, c != 0);
}

inline void AluRla(CpuState &s)
{
	uint8_t in  = FlagC(s) ? 1 : 0;
	uint8_t out = static_cast<uint8_t>((s.r.a >> 7) & 1);
	s.r.a = static_cast<uint8_t>((s.r.a << 1) | in);
	SetFlags(s, false, false, false, out != 0);
}

inline void AluRra(CpuState &s)
{
	uint8_t in  = FlagC(s) ? 1 : 0;
	uint8_t out = static_cast<uint8_t>(s.r.a & 1);
	s.r.a = static_cast<uint8_t>((s.r.a >> 1) | (in << 7));
	SetFlags(s, false, false, false, out != 0);
}

inline void AluDaa(CpuState &s)
{
	// Decimal-adjust after BCD ADD/SUB. Depends on N and previous H, C.
	uint16_t a = s.r.a;
	uint8_t  correction = 0;
	bool     carry = false;

	if (FlagN(s))
	{
		if (FlagH(s)) correction |= 0x06;
		if (FlagC(s)) { correction |= 0x60; carry = true; }
		a = static_cast<uint16_t>((a - correction) & 0xFF);
	}
	else
	{
		if (FlagH(s) || (a & 0xF) > 9)      correction |= 0x06;
		if (FlagC(s) || a > 0x99)           { correction |= 0x60; carry = true; }
		a = static_cast<uint16_t>((a + correction) & 0xFF);
	}

	s.r.a = static_cast<uint8_t>(a);
	s.r.f = static_cast<uint8_t>(
		(s.r.f & FLAG_N) |
		(a == 0 ? FLAG_Z : 0) |
		(carry  ? FLAG_C : 0));
}

inline void AluCpl(CpuState &s)
{
	s.r.a = static_cast<uint8_t>(~s.r.a);
	s.r.f = static_cast<uint8_t>(s.r.f | FLAG_N | FLAG_H);  // Z,C preserved
}

inline void AluScf(CpuState &s)
{
	s.r.f = static_cast<uint8_t>((s.r.f & FLAG_Z) | FLAG_C);
}

inline void AluCcf(CpuState &s)
{
	// Toggle C, clear N and H, preserve Z.
	s.r.f = static_cast<uint8_t>((s.r.f & (FLAG_Z | FLAG_C)) ^ FLAG_C);
}

} // namespace SGB

#endif
