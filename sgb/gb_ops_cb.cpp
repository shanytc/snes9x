/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// CB-prefixed opcodes. 256 values, decoded by bit pattern rather than a
// 256-case switch, because the CB table is very regular:
//
//   bits 7:6  = operation class: 00=shift/rotate, 01=BIT, 10=RES, 11=SET
//   bits 5:3  = sub-op (shift family: RLC/RRC/RL/RR/SLA/SRA/SWAP/SRL)
//               or bit number (for BIT/RES/SET, n = 0..7)
//   bits 2:0  = operand register: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A
//
// Cycle counts (Pan Docs):
//   op on register:            8 T-cycles
//   BIT n,(HL):               12
//   RES/SET/shift on (HL):    16
//
// Caller (gb_ops.cpp, case 0xCB) has already fetched the prefix and
// the CB byte itself. We add the full T-cycle cost here.

#include "gb_ops.h"

namespace SGB {

namespace {

// Register-file index access — decoded from op's low 3 bits.
// Index 6 = (HL), handled by the caller; this helper never receives 6.
inline uint8_t GetReg(CpuState &s, uint8_t idx)
{
	switch (idx)
	{
		case 0: return s.r.b;
		case 1: return s.r.c;
		case 2: return s.r.d;
		case 3: return s.r.e;
		case 4: return s.r.h;
		case 5: return s.r.l;
		case 7: return s.r.a;
		default: return 0;  // unreachable
	}
}

inline void SetReg(CpuState &s, uint8_t idx, uint8_t v)
{
	switch (idx)
	{
		case 0: s.r.b = v; break;
		case 1: s.r.c = v; break;
		case 2: s.r.d = v; break;
		case 3: s.r.e = v; break;
		case 4: s.r.h = v; break;
		case 5: s.r.l = v; break;
		case 7: s.r.a = v; break;
		default: break;  // unreachable
	}
}

inline uint8_t RunShiftRotate(CpuState &s, uint8_t sub_op, uint8_t v)
{
	switch (sub_op)
	{
		case 0: return AluRlc (s, v);
		case 1: return AluRrc (s, v);
		case 2: return AluRl  (s, v);
		case 3: return AluRr  (s, v);
		case 4: return AluSla (s, v);
		case 5: return AluSra (s, v);
		case 6: return AluSwap(s, v);
		case 7: return AluSrl (s, v);
		default: return v;  // unreachable
	}
}

} // anonymous

void DispatchCB(CpuState &s, Memory &mem, uint8_t op)
{
	const uint8_t reg_idx = op & 0x07;
	const bool    is_hl   = (reg_idx == 6);
	const uint8_t klass   = (op >> 6) & 0x03;     // 00/01/10/11
	const uint8_t sub_op  = (op >> 3) & 0x07;     // shift kind or bit number

	// Read operand.
	uint8_t v = is_hl ? MemRead(mem, s.r.hl) : GetReg(s, reg_idx);

	switch (klass)
	{
	case 0:  // shift / rotate / swap
	{
		uint8_t r = RunShiftRotate(s, sub_op, v);
		if (is_hl)
		{
			MemWrite(mem, s.r.hl, r);
			s.t_cycles += 16;
		}
		else
		{
			SetReg(s, reg_idx, r);
			s.t_cycles += 8;
		}
		break;
	}

	case 1:  // BIT n,r / BIT n,(HL) — read-only
		AluBit(s, sub_op, v);
		s.t_cycles += is_hl ? 12 : 8;
		break;

	case 2:  // RES n,r / RES n,(HL)
	{
		uint8_t r = AluRes(sub_op, v);
		if (is_hl)
		{
			MemWrite(mem, s.r.hl, r);
			s.t_cycles += 16;
		}
		else
		{
			SetReg(s, reg_idx, r);
			s.t_cycles += 8;
		}
		break;
	}

	case 3:  // SET n,r / SET n,(HL)
	{
		uint8_t r = AluSet(sub_op, v);
		if (is_hl)
		{
			MemWrite(mem, s.r.hl, r);
			s.t_cycles += 16;
		}
		else
		{
			SetReg(s, reg_idx, r);
			s.t_cycles += 8;
		}
		break;
	}
	}
}

} // namespace SGB
