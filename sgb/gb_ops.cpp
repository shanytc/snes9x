/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// SM83 non-CB opcode dispatcher, machine-cycle accurate. The caller has
// already fetched the opcode (one ticked M-cycle); every further bus access
// here goes through BusRead/BusWrite/Fetch8, each of which advances the
// world one M-cycle BEFORE the access, and internal (no-bus) cycles are
// modeled with explicit TickM calls. Summed per opcode this reproduces the
// exact Pan Docs T-cycle counts — but, more importantly, each individual
// read/write now lands on its true machine cycle, which is what the
// mooneye-gb acceptance timing tests measure.
//
// Rows are grouped 16 opcodes at a time to match the standard DMG
// reference table. Undefined opcodes ($D3 $DB $DD $E3 $E4 $EB $EC $ED
// $F4 $FC $FD) lock up the real CPU; we stop the CPU and tick an
// illegal-op counter so upper layers can flag it.

#include "gb_ops.h"
#include "gb_ppu.h"
#include "gb_timer.h"

namespace SGB {

void Dispatch(CpuState &s, Memory &mem, uint8_t op)
{
	switch (op)
	{
	// ===== 0x00-0x0F =====
	case 0x00: break;                                                                          // NOP
	case 0x01: s.r.bc = Fetch16(s, mem); break;                                                // LD BC,nn
	case 0x02: BusWrite(s, mem, s.r.bc, s.r.a); break;                                         // LD (BC),A
	case 0x03: TickM(s, mem); MemOamBugIncDec(mem, s.r.bc); s.r.bc++; break;                                                 // INC BC
	case 0x04: s.r.b = AluInc(s, s.r.b); break;                                                // INC B
	case 0x05: s.r.b = AluDec(s, s.r.b); break;                                                // DEC B
	case 0x06: s.r.b = Fetch8(s, mem); break;                                                  // LD B,n
	case 0x07: AluRlca(s); break;                                                              // RLCA
	case 0x08: { uint16_t a = Fetch16(s, mem);
	             BusWrite(s, mem, a, static_cast<uint8_t>(s.r.sp & 0xFF));
	             BusWrite(s, mem, static_cast<uint16_t>(a + 1), static_cast<uint8_t>(s.r.sp >> 8)); } break;  // LD (nn),SP
	case 0x09: s.r.hl = AluAdd16(s, s.r.hl, s.r.bc); TickM(s, mem); break;                     // ADD HL,BC
	case 0x0A: s.r.a = BusRead(s, mem, s.r.bc); break;                                         // LD A,(BC)
	case 0x0B: TickM(s, mem); MemOamBugIncDec(mem, s.r.bc); s.r.bc--; break;                                                 // DEC BC
	case 0x0C: s.r.c = AluInc(s, s.r.c); break;                                                // INC C
	case 0x0D: s.r.c = AluDec(s, s.r.c); break;                                                // DEC C
	case 0x0E: s.r.c = Fetch8(s, mem); break;                                                  // LD C,n
	case 0x0F: AluRrca(s); break;                                                              // RRCA

	// ===== 0x10-0x1F =====
	case 0x10:                                                                                 // STOP / CGB speed switch
		s.r.pc++;   // skip the pad byte without a bus cycle
		if (mem.key1_armed)
		{
			mem.double_speed = !mem.double_speed;
			mem.key1_armed   = false;
			{
				// The CPU/PPU clock-divider phase re-aligns at the switch
				// (daid speed_switch_timing_stat samples land on it).
				static int dsa = -1;
				if (dsa < 0) { const char *e = getenv("ACID_DSA"); dsa = e ? atoi(e) : -2; }
				if (dsa >= 0) mem.ds_tick_rem = static_cast<uint8_t>(dsa);
			}
			// The switch stalls the CPU for roughly a frame while the PPU
			// keeps scanning (daid speed_switch_timing_ly sees LY advance
			// ~143 lines), then DIV restarts from zero.
			{
				static int stl = -1;
				if (stl < 0) { const char *e = getenv("ACID_STL"); stl = e ? atoi(e) : 32770; }
				for (int i = 0; i < stl; ++i)
					TickM(s, mem);
			}
			if (mem.timer) TimerResetDiv(*mem.timer, mem);
			// The display clock resumes a few dots out of step with the
			// CPU's first post-switch cycle (daid speed_switch_timing_stat).
			{
				static int ssd = -99;
				if (ssd < -90) { const char *e = getenv("ACID_SSD"); ssd = e ? atoi(e) : 0; }
				if (ssd > 0 && mem.ppu) PpuStep(*mem.ppu, mem, ssd);
			}
		}
		else
		{
			if (mem.timer) TimerResetDiv(*mem.timer, mem);   // STOP resets DIV
			s.stopped = true;
			if (mem.ppu && (mem.ppu->lcdc & 0x80))
				PpuOnCpuStop(*mem.ppu, mem.cgb_hw);
		}
		break;
	case 0x11: s.r.de = Fetch16(s, mem); break;                                                // LD DE,nn
	case 0x12: BusWrite(s, mem, s.r.de, s.r.a); break;                                         // LD (DE),A
	case 0x13: TickM(s, mem); MemOamBugIncDec(mem, s.r.de); s.r.de++; break;                                                 // INC DE
	case 0x14: s.r.d = AluInc(s, s.r.d); break;                                                // INC D
	case 0x15: s.r.d = AluDec(s, s.r.d); break;                                                // DEC D
	case 0x16: s.r.d = Fetch8(s, mem); break;                                                  // LD D,n
	case 0x17: AluRla(s); break;                                                               // RLA
	case 0x18: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             s.r.pc = static_cast<uint16_t>(s.r.pc + e); TickM(s, mem); } break;           // JR e
	case 0x19: s.r.hl = AluAdd16(s, s.r.hl, s.r.de); TickM(s, mem); break;                     // ADD HL,DE
	case 0x1A: s.r.a = BusRead(s, mem, s.r.de); break;                                         // LD A,(DE)
	case 0x1B: TickM(s, mem); MemOamBugIncDec(mem, s.r.de); s.r.de--; break;                                                 // DEC DE
	case 0x1C: s.r.e = AluInc(s, s.r.e); break;                                                // INC E
	case 0x1D: s.r.e = AluDec(s, s.r.e); break;                                                // DEC E
	case 0x1E: s.r.e = Fetch8(s, mem); break;                                                  // LD E,n
	case 0x1F: AluRra(s); break;                                                               // RRA

	// ===== 0x20-0x2F =====
	case 0x20: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             if (!FlagZ(s)) { s.r.pc = static_cast<uint16_t>(s.r.pc + e); TickM(s, mem); } } break;  // JR NZ,e
	case 0x21: s.r.hl = Fetch16(s, mem); break;                                                // LD HL,nn
	case 0x22: BusWrite(s, mem, s.r.hl, s.r.a); s.r.hl++; break;                               // LD (HL+),A
	case 0x23: TickM(s, mem); MemOamBugIncDec(mem, s.r.hl); s.r.hl++; break;                                                 // INC HL
	case 0x24: s.r.h = AluInc(s, s.r.h); break;                                                // INC H
	case 0x25: s.r.h = AluDec(s, s.r.h); break;                                                // DEC H
	case 0x26: s.r.h = Fetch8(s, mem); break;                                                  // LD H,n
	case 0x27: AluDaa(s); break;                                                               // DAA
	case 0x28: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             if (FlagZ(s))  { s.r.pc = static_cast<uint16_t>(s.r.pc + e); TickM(s, mem); } } break;  // JR Z,e
	case 0x29: s.r.hl = AluAdd16(s, s.r.hl, s.r.hl); TickM(s, mem); break;                     // ADD HL,HL
	case 0x2A: s.r.a = BusRead(s, mem, s.r.hl); s.r.hl++; break;                               // LD A,(HL+)
	case 0x2B: TickM(s, mem); MemOamBugIncDec(mem, s.r.hl); s.r.hl--; break;                                                 // DEC HL
	case 0x2C: s.r.l = AluInc(s, s.r.l); break;                                                // INC L
	case 0x2D: s.r.l = AluDec(s, s.r.l); break;                                                // DEC L
	case 0x2E: s.r.l = Fetch8(s, mem); break;                                                  // LD L,n
	case 0x2F: AluCpl(s); break;                                                               // CPL

	// ===== 0x30-0x3F =====
	case 0x30: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             if (!FlagC(s)) { s.r.pc = static_cast<uint16_t>(s.r.pc + e); TickM(s, mem); } } break;  // JR NC,e
	case 0x31: s.r.sp = Fetch16(s, mem); break;                                                // LD SP,nn
	case 0x32: BusWrite(s, mem, s.r.hl, s.r.a); s.r.hl--; break;                               // LD (HL-),A
	case 0x33: TickM(s, mem); MemOamBugIncDec(mem, s.r.sp); s.r.sp++; break;                                                 // INC SP
	case 0x34: { uint8_t v = BusRead(s, mem, s.r.hl); BusWrite(s, mem, s.r.hl, AluInc(s, v)); } break;  // INC (HL)
	case 0x35: { uint8_t v = BusRead(s, mem, s.r.hl); BusWrite(s, mem, s.r.hl, AluDec(s, v)); } break;  // DEC (HL)
	case 0x36: { uint8_t v = Fetch8(s, mem); BusWrite(s, mem, s.r.hl, v); } break;             // LD (HL),n
	case 0x37: AluScf(s); break;                                                               // SCF
	case 0x38: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             if (FlagC(s))  { s.r.pc = static_cast<uint16_t>(s.r.pc + e); TickM(s, mem); } } break;  // JR C,e
	case 0x39: s.r.hl = AluAdd16(s, s.r.hl, s.r.sp); TickM(s, mem); break;                     // ADD HL,SP
	case 0x3A: s.r.a = BusRead(s, mem, s.r.hl); s.r.hl--; break;                               // LD A,(HL-)
	case 0x3B: TickM(s, mem); MemOamBugIncDec(mem, s.r.sp); s.r.sp--; break;                                                 // DEC SP
	case 0x3C: s.r.a = AluInc(s, s.r.a); break;                                                // INC A
	case 0x3D: s.r.a = AluDec(s, s.r.a); break;                                                // DEC A
	case 0x3E: s.r.a = Fetch8(s, mem); break;                                                  // LD A,n
	case 0x3F: AluCcf(s); break;                                                               // CCF

	// ===== 0x40-0x4F : LD B,r / LD C,r =====
	case 0x40: break;                                                                          // LD B,B (no-op)
	case 0x41: s.r.b = s.r.c; break;                                                           // LD B,C
	case 0x42: s.r.b = s.r.d; break;                                                           // LD B,D
	case 0x43: s.r.b = s.r.e; break;                                                           // LD B,E
	case 0x44: s.r.b = s.r.h; break;                                                           // LD B,H
	case 0x45: s.r.b = s.r.l; break;                                                           // LD B,L
	case 0x46: s.r.b = BusRead(s, mem, s.r.hl); break;                                         // LD B,(HL)
	case 0x47: s.r.b = s.r.a; break;                                                           // LD B,A
	case 0x48: s.r.c = s.r.b; break;                                                           // LD C,B
	case 0x49: break;                                                                          // LD C,C
	case 0x4A: s.r.c = s.r.d; break;                                                           // LD C,D
	case 0x4B: s.r.c = s.r.e; break;                                                           // LD C,E
	case 0x4C: s.r.c = s.r.h; break;                                                           // LD C,H
	case 0x4D: s.r.c = s.r.l; break;                                                           // LD C,L
	case 0x4E: s.r.c = BusRead(s, mem, s.r.hl); break;                                         // LD C,(HL)
	case 0x4F: s.r.c = s.r.a; break;                                                           // LD C,A

	// ===== 0x50-0x5F : LD D,r / LD E,r =====
	case 0x50: s.r.d = s.r.b; break;                                                           // LD D,B
	case 0x51: s.r.d = s.r.c; break;                                                           // LD D,C
	case 0x52: break;                                                                          // LD D,D
	case 0x53: s.r.d = s.r.e; break;                                                           // LD D,E
	case 0x54: s.r.d = s.r.h; break;                                                           // LD D,H
	case 0x55: s.r.d = s.r.l; break;                                                           // LD D,L
	case 0x56: s.r.d = BusRead(s, mem, s.r.hl); break;                                         // LD D,(HL)
	case 0x57: s.r.d = s.r.a; break;                                                           // LD D,A
	case 0x58: s.r.e = s.r.b; break;                                                           // LD E,B
	case 0x59: s.r.e = s.r.c; break;                                                           // LD E,C
	case 0x5A: s.r.e = s.r.d; break;                                                           // LD E,D
	case 0x5B: break;                                                                          // LD E,E
	case 0x5C: s.r.e = s.r.h; break;                                                           // LD E,H
	case 0x5D: s.r.e = s.r.l; break;                                                           // LD E,L
	case 0x5E: s.r.e = BusRead(s, mem, s.r.hl); break;                                         // LD E,(HL)
	case 0x5F: s.r.e = s.r.a; break;                                                           // LD E,A

	// ===== 0x60-0x6F : LD H,r / LD L,r =====
	case 0x60: s.r.h = s.r.b; break;                                                           // LD H,B
	case 0x61: s.r.h = s.r.c; break;                                                           // LD H,C
	case 0x62: s.r.h = s.r.d; break;                                                           // LD H,D
	case 0x63: s.r.h = s.r.e; break;                                                           // LD H,E
	case 0x64: break;                                                                          // LD H,H
	case 0x65: s.r.h = s.r.l; break;                                                           // LD H,L
	case 0x66: s.r.h = BusRead(s, mem, s.r.hl); break;                                         // LD H,(HL)
	case 0x67: s.r.h = s.r.a; break;                                                           // LD H,A
	case 0x68: s.r.l = s.r.b; break;                                                           // LD L,B
	case 0x69: s.r.l = s.r.c; break;                                                           // LD L,C
	case 0x6A: s.r.l = s.r.d; break;                                                           // LD L,D
	case 0x6B: s.r.l = s.r.e; break;                                                           // LD L,E
	case 0x6C: s.r.l = s.r.h; break;                                                           // LD L,H
	case 0x6D: break;                                                                          // LD L,L
	case 0x6E: s.r.l = BusRead(s, mem, s.r.hl); break;                                         // LD L,(HL)
	case 0x6F: s.r.l = s.r.a; break;                                                           // LD L,A

	// ===== 0x70-0x7F : LD (HL),r / HALT / LD A,r =====
	case 0x70: BusWrite(s, mem, s.r.hl, s.r.b); break;                                         // LD (HL),B
	case 0x71: BusWrite(s, mem, s.r.hl, s.r.c); break;                                         // LD (HL),C
	case 0x72: BusWrite(s, mem, s.r.hl, s.r.d); break;                                         // LD (HL),D
	case 0x73: BusWrite(s, mem, s.r.hl, s.r.e); break;                                         // LD (HL),E
	case 0x74: BusWrite(s, mem, s.r.hl, s.r.h); break;                                         // LD (HL),H
	case 0x75: BusWrite(s, mem, s.r.hl, s.r.l); break;                                         // LD (HL),L
	case 0x76:                                                                                 // HALT
		// HALT bug: entered with IME=0 (and no EI pending) while an IRQ is
		// already pending, the CPU fails to halt and re-reads the next byte.
		if (!s.ime && !s.ime_pending && (mem.ie & mem.if_ & IRQ_ALL))
			s.halt_bug = true;
		else
			s.halted = true;
		break;
	case 0x77: BusWrite(s, mem, s.r.hl, s.r.a); break;                                         // LD (HL),A
	case 0x78: s.r.a = s.r.b; break;                                                           // LD A,B
	case 0x79: s.r.a = s.r.c; break;                                                           // LD A,C
	case 0x7A: s.r.a = s.r.d; break;                                                           // LD A,D
	case 0x7B: s.r.a = s.r.e; break;                                                           // LD A,E
	case 0x7C: s.r.a = s.r.h; break;                                                           // LD A,H
	case 0x7D: s.r.a = s.r.l; break;                                                           // LD A,L
	case 0x7E: s.r.a = BusRead(s, mem, s.r.hl); break;                                         // LD A,(HL)
	case 0x7F: break;                                                                          // LD A,A

	// ===== 0x80-0x8F : ADD/ADC =====
	case 0x80: s.r.a = AluAdd(s, s.r.a, s.r.b); break;                                         // ADD A,B
	case 0x81: s.r.a = AluAdd(s, s.r.a, s.r.c); break;                                         // ADD A,C
	case 0x82: s.r.a = AluAdd(s, s.r.a, s.r.d); break;                                         // ADD A,D
	case 0x83: s.r.a = AluAdd(s, s.r.a, s.r.e); break;                                         // ADD A,E
	case 0x84: s.r.a = AluAdd(s, s.r.a, s.r.h); break;                                         // ADD A,H
	case 0x85: s.r.a = AluAdd(s, s.r.a, s.r.l); break;                                         // ADD A,L
	case 0x86: s.r.a = AluAdd(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // ADD A,(HL)
	case 0x87: s.r.a = AluAdd(s, s.r.a, s.r.a); break;                                         // ADD A,A
	case 0x88: s.r.a = AluAdc(s, s.r.a, s.r.b); break;                                         // ADC A,B
	case 0x89: s.r.a = AluAdc(s, s.r.a, s.r.c); break;                                         // ADC A,C
	case 0x8A: s.r.a = AluAdc(s, s.r.a, s.r.d); break;                                         // ADC A,D
	case 0x8B: s.r.a = AluAdc(s, s.r.a, s.r.e); break;                                         // ADC A,E
	case 0x8C: s.r.a = AluAdc(s, s.r.a, s.r.h); break;                                         // ADC A,H
	case 0x8D: s.r.a = AluAdc(s, s.r.a, s.r.l); break;                                         // ADC A,L
	case 0x8E: s.r.a = AluAdc(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // ADC A,(HL)
	case 0x8F: s.r.a = AluAdc(s, s.r.a, s.r.a); break;                                         // ADC A,A

	// ===== 0x90-0x9F : SUB/SBC =====
	case 0x90: s.r.a = AluSub(s, s.r.a, s.r.b); break;                                         // SUB B
	case 0x91: s.r.a = AluSub(s, s.r.a, s.r.c); break;                                         // SUB C
	case 0x92: s.r.a = AluSub(s, s.r.a, s.r.d); break;                                         // SUB D
	case 0x93: s.r.a = AluSub(s, s.r.a, s.r.e); break;                                         // SUB E
	case 0x94: s.r.a = AluSub(s, s.r.a, s.r.h); break;                                         // SUB H
	case 0x95: s.r.a = AluSub(s, s.r.a, s.r.l); break;                                         // SUB L
	case 0x96: s.r.a = AluSub(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // SUB (HL)
	case 0x97: s.r.a = AluSub(s, s.r.a, s.r.a); break;                                         // SUB A
	case 0x98: s.r.a = AluSbc(s, s.r.a, s.r.b); break;                                         // SBC A,B
	case 0x99: s.r.a = AluSbc(s, s.r.a, s.r.c); break;                                         // SBC A,C
	case 0x9A: s.r.a = AluSbc(s, s.r.a, s.r.d); break;                                         // SBC A,D
	case 0x9B: s.r.a = AluSbc(s, s.r.a, s.r.e); break;                                         // SBC A,E
	case 0x9C: s.r.a = AluSbc(s, s.r.a, s.r.h); break;                                         // SBC A,H
	case 0x9D: s.r.a = AluSbc(s, s.r.a, s.r.l); break;                                         // SBC A,L
	case 0x9E: s.r.a = AluSbc(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // SBC A,(HL)
	case 0x9F: s.r.a = AluSbc(s, s.r.a, s.r.a); break;                                         // SBC A,A

	// ===== 0xA0-0xAF : AND/XOR =====
	case 0xA0: s.r.a = AluAnd(s, s.r.a, s.r.b); break;                                         // AND B
	case 0xA1: s.r.a = AluAnd(s, s.r.a, s.r.c); break;                                         // AND C
	case 0xA2: s.r.a = AluAnd(s, s.r.a, s.r.d); break;                                         // AND D
	case 0xA3: s.r.a = AluAnd(s, s.r.a, s.r.e); break;                                         // AND E
	case 0xA4: s.r.a = AluAnd(s, s.r.a, s.r.h); break;                                         // AND H
	case 0xA5: s.r.a = AluAnd(s, s.r.a, s.r.l); break;                                         // AND L
	case 0xA6: s.r.a = AluAnd(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // AND (HL)
	case 0xA7: s.r.a = AluAnd(s, s.r.a, s.r.a); break;                                         // AND A
	case 0xA8: s.r.a = AluXor(s, s.r.a, s.r.b); break;                                         // XOR B
	case 0xA9: s.r.a = AluXor(s, s.r.a, s.r.c); break;                                         // XOR C
	case 0xAA: s.r.a = AluXor(s, s.r.a, s.r.d); break;                                         // XOR D
	case 0xAB: s.r.a = AluXor(s, s.r.a, s.r.e); break;                                         // XOR E
	case 0xAC: s.r.a = AluXor(s, s.r.a, s.r.h); break;                                         // XOR H
	case 0xAD: s.r.a = AluXor(s, s.r.a, s.r.l); break;                                         // XOR L
	case 0xAE: s.r.a = AluXor(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                       // XOR (HL)
	case 0xAF: s.r.a = AluXor(s, s.r.a, s.r.a); break;                                         // XOR A

	// ===== 0xB0-0xBF : OR/CP =====
	case 0xB0: s.r.a = AluOr(s, s.r.a, s.r.b); break;                                          // OR B
	case 0xB1: s.r.a = AluOr(s, s.r.a, s.r.c); break;                                          // OR C
	case 0xB2: s.r.a = AluOr(s, s.r.a, s.r.d); break;                                          // OR D
	case 0xB3: s.r.a = AluOr(s, s.r.a, s.r.e); break;                                          // OR E
	case 0xB4: s.r.a = AluOr(s, s.r.a, s.r.h); break;                                          // OR H
	case 0xB5: s.r.a = AluOr(s, s.r.a, s.r.l); break;                                          // OR L
	case 0xB6: s.r.a = AluOr(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                        // OR (HL)
	case 0xB7: s.r.a = AluOr(s, s.r.a, s.r.a); break;                                          // OR A
	case 0xB8: AluCp(s, s.r.a, s.r.b); break;                                                  // CP B
	case 0xB9: AluCp(s, s.r.a, s.r.c); break;                                                  // CP C
	case 0xBA: AluCp(s, s.r.a, s.r.d); break;                                                  // CP D
	case 0xBB: AluCp(s, s.r.a, s.r.e); break;                                                  // CP E
	case 0xBC: AluCp(s, s.r.a, s.r.h); break;                                                  // CP H
	case 0xBD: AluCp(s, s.r.a, s.r.l); break;                                                  // CP L
	case 0xBE: AluCp(s, s.r.a, BusRead(s, mem, s.r.hl)); break;                                // CP (HL)
	case 0xBF: AluCp(s, s.r.a, s.r.a); break;                                                  // CP A

	// ===== 0xC0-0xCF =====
	case 0xC0: TickM(s, mem); if (!FlagZ(s)) { s.r.pc = Pop16(s, mem); TickM(s, mem); } break; // RET NZ
	case 0xC1: s.r.bc = Pop16(s, mem); break;                                                  // POP BC
	case 0xC2: { uint16_t a = Fetch16(s, mem); if (!FlagZ(s)) { s.r.pc = a; TickM(s, mem); } } break;  // JP NZ,nn
	case 0xC3: { uint16_t a = Fetch16(s, mem); s.r.pc = a; TickM(s, mem); } break;             // JP nn
	case 0xC4: { uint16_t a = Fetch16(s, mem); if (!FlagZ(s)) { Push16(s, mem, s.r.pc); s.r.pc = a; } } break;  // CALL NZ,nn
	case 0xC5: Push16(s, mem, s.r.bc); break;                                                  // PUSH BC
	case 0xC6: s.r.a = AluAdd(s, s.r.a, Fetch8(s, mem)); break;                                // ADD A,n
	case 0xC7: Push16(s, mem, s.r.pc); s.r.pc = 0x0000; break;                                 // RST 00h
	case 0xC8: TickM(s, mem); if (FlagZ(s))  { s.r.pc = Pop16(s, mem); TickM(s, mem); } break; // RET Z
	case 0xC9: s.r.pc = Pop16(s, mem); TickM(s, mem); break;                                   // RET
	case 0xCA: { uint16_t a = Fetch16(s, mem); if (FlagZ(s))  { s.r.pc = a; TickM(s, mem); } } break;  // JP Z,nn
	case 0xCB: { uint8_t cb = Fetch8(s, mem); DispatchCB(s, mem, cb); } break;                 // CB prefix
	case 0xCC: { uint16_t a = Fetch16(s, mem); if (FlagZ(s))  { Push16(s, mem, s.r.pc); s.r.pc = a; } } break;  // CALL Z,nn
	case 0xCD: { uint16_t a = Fetch16(s, mem); Push16(s, mem, s.r.pc); s.r.pc = a; } break;    // CALL nn
	case 0xCE: s.r.a = AluAdc(s, s.r.a, Fetch8(s, mem)); break;                                // ADC A,n
	case 0xCF: Push16(s, mem, s.r.pc); s.r.pc = 0x0008; break;                                 // RST 08h

	// ===== 0xD0-0xDF =====
	case 0xD0: TickM(s, mem); if (!FlagC(s)) { s.r.pc = Pop16(s, mem); TickM(s, mem); } break; // RET NC
	case 0xD1: s.r.de = Pop16(s, mem); break;                                                  // POP DE
	case 0xD2: { uint16_t a = Fetch16(s, mem); if (!FlagC(s)) { s.r.pc = a; TickM(s, mem); } } break;  // JP NC,nn
	case 0xD3: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xD4: { uint16_t a = Fetch16(s, mem); if (!FlagC(s)) { Push16(s, mem, s.r.pc); s.r.pc = a; } } break;  // CALL NC,nn
	case 0xD5: Push16(s, mem, s.r.de); break;                                                  // PUSH DE
	case 0xD6: s.r.a = AluSub(s, s.r.a, Fetch8(s, mem)); break;                                // SUB A,n
	case 0xD7: Push16(s, mem, s.r.pc); s.r.pc = 0x0010; break;                                 // RST 10h
	case 0xD8: TickM(s, mem); if (FlagC(s))  { s.r.pc = Pop16(s, mem); TickM(s, mem); } break; // RET C
	case 0xD9: s.r.pc = Pop16(s, mem); s.ime = true; s.ime_pending = false; TickM(s, mem); break;  // RETI
	case 0xDA: { uint16_t a = Fetch16(s, mem); if (FlagC(s))  { s.r.pc = a; TickM(s, mem); } } break;  // JP C,nn
	case 0xDB: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xDC: { uint16_t a = Fetch16(s, mem); if (FlagC(s))  { Push16(s, mem, s.r.pc); s.r.pc = a; } } break;  // CALL C,nn
	case 0xDD: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xDE: s.r.a = AluSbc(s, s.r.a, Fetch8(s, mem)); break;                                // SBC A,n
	case 0xDF: Push16(s, mem, s.r.pc); s.r.pc = 0x0018; break;                                 // RST 18h

	// ===== 0xE0-0xEF =====
	case 0xE0: { uint16_t a = static_cast<uint16_t>(0xFF00 + Fetch8(s, mem)); BusWrite(s, mem, a, s.r.a); } break;  // LDH (n),A
	case 0xE1: s.r.hl = Pop16(s, mem); break;                                                  // POP HL
	case 0xE2: BusWrite(s, mem, static_cast<uint16_t>(0xFF00 + s.r.c), s.r.a); break;          // LD (C),A
	case 0xE3: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xE4: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xE5: Push16(s, mem, s.r.hl); break;                                                  // PUSH HL
	case 0xE6: s.r.a = AluAnd(s, s.r.a, Fetch8(s, mem)); break;                                // AND A,n
	case 0xE7: Push16(s, mem, s.r.pc); s.r.pc = 0x0020; break;                                 // RST 20h
	case 0xE8: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             s.r.sp = AluAddSpE(s, s.r.sp, e); TickM(s, mem); TickM(s, mem); } break;      // ADD SP,e
	case 0xE9: s.r.pc = s.r.hl; break;                                                         // JP HL  (a.k.a JP (HL))
	case 0xEA: { uint16_t a = Fetch16(s, mem); BusWrite(s, mem, a, s.r.a); } break;            // LD (nn),A
	case 0xEB: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xEC: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xED: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xEE: s.r.a = AluXor(s, s.r.a, Fetch8(s, mem)); break;                                // XOR A,n
	case 0xEF: Push16(s, mem, s.r.pc); s.r.pc = 0x0028; break;                                 // RST 28h

	// ===== 0xF0-0xFF =====
	case 0xF0: { uint16_t a = static_cast<uint16_t>(0xFF00 + Fetch8(s, mem)); s.r.a = BusRead(s, mem, a); } break;  // LDH A,(n)
	case 0xF1: s.r.af = static_cast<uint16_t>(Pop16(s, mem) & 0xFFF0); break;                  // POP AF — F low nibble always 0
	case 0xF2: s.r.a = BusRead(s, mem, static_cast<uint16_t>(0xFF00 + s.r.c)); break;          // LD A,(C)
	case 0xF3: s.ime = false; s.ime_pending = false; break;                                    // DI
	case 0xF4: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xF5: Push16(s, mem, s.r.af); break;                                                  // PUSH AF
	case 0xF6: s.r.a = AluOr(s, s.r.a, Fetch8(s, mem)); break;                                 // OR A,n
	case 0xF7: Push16(s, mem, s.r.pc); s.r.pc = 0x0030; break;                                 // RST 30h
	case 0xF8: { int8_t e = static_cast<int8_t>(Fetch8(s, mem));
	             s.r.hl = AluAddSpE(s, s.r.sp, e); TickM(s, mem); } break;                     // LD HL,SP+e
	case 0xF9: s.r.sp = s.r.hl; TickM(s, mem); break;                                          // LD SP,HL
	case 0xFA: { uint16_t a = Fetch16(s, mem); s.r.a = BusRead(s, mem, a); } break;            // LD A,(nn)
	case 0xFB: s.ime_pending = true; break;                                                    // EI
	case 0xFC: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xFD: ++s.illegal_ops; s.stopped = true; break;                                       // *** undefined ***
	case 0xFE: AluCp(s, s.r.a, Fetch8(s, mem)); break;                                         // CP A,n
	case 0xFF: Push16(s, mem, s.r.pc); s.r.pc = 0x0038; break;                                 // RST 38h
	}
}

} // namespace SGB
