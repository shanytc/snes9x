/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_CPU_H_
#define _SGB_GB_CPU_H_

#include <cstdint>

namespace SGB {

struct Memory;

// Sharp LR35902 / SM83 — 8-bit CPU used by DMG/CGB/SGB.
// A Z80-derived ISA but missing the alt register bank, IX/IY, I/O ports, and
// several opcodes; adds SWAP, RETI, LDH, and the $CB bit-manipulation page.
struct CpuRegs
{
	// Register-pair access via union — host must be little-endian.
	union { uint16_t af; struct { uint8_t f, a; }; };
	union { uint16_t bc; struct { uint8_t c, b; }; };
	union { uint16_t de; struct { uint8_t e, d; }; };
	union { uint16_t hl; struct { uint8_t l, h; }; };
	uint16_t sp;
	uint16_t pc;
};

enum : uint8_t
{
	FLAG_Z = 0x80,
	FLAG_N = 0x40,
	FLAG_H = 0x20,
	FLAG_C = 0x10
};

enum : uint8_t
{
	IRQ_VBLANK  = 0x01,
	IRQ_LCDSTAT = 0x02,
	IRQ_TIMER   = 0x04,
	IRQ_SERIAL  = 0x08,
	IRQ_JOYPAD  = 0x10,
	IRQ_ALL     = 0x1F
};

struct CpuState
{
	CpuRegs  r;
	bool     ime;          // master interrupt enable
	bool     ime_pending;  // EI takes effect after the next instruction
	bool     halted;
	bool     stopped;
	bool     halt_bug;     // HALT-with-IME-off-and-pending-IRQ oddity
	int64_t  t_cycles;     // monotonic T-cycle counter
	uint32_t illegal_ops;  // count of the 11 undefined opcodes executed
};

uint32_t IrqServicedCount(uint8_t vector);
void     IrqServicedReset();

// Per-instruction PC trace ring — captures pc, opcode and key registers for
// each Step(). Freezes automatically once an RST 38 loop is detected (PC at
// $0038/$0039 for many consecutive instructions), so the entries leading up
// to a crash are preserved instead of being overwritten by the loop itself.
struct PcTraceEntry
{
	uint16_t pc;        // PC at fetch
	uint16_t sp;        // SP at fetch — diverges fast when stack gets hosed
	uint16_t af;        // AF for context
	uint8_t  opcode;    // opcode byte at PC
	uint8_t  ime;       // 0/1
};
static constexpr uint32_t kPcTraceCap = 64;

uint32_t PcTraceCount();
bool     PcTraceFrozen();
bool     PcTraceGet(uint32_t i, PcTraceEntry &out);
void     PcTraceReset();

// Debug trace — called before each instruction dispatch.
// Fired in user code (hot path), so keep hooks cheap.
using TraceHook = void (*)(uint16_t pc, uint8_t opcode, const CpuState &state);

// Upper bound on T-cycles a single Cpu::Step() can consume. The longest
// observed paths are CALL cc,nn taken (24 T-cycles) and IRQ dispatch
// (20 T-cycles). Used by the per-dot CPU/PPU interleaving gate in
// RunCycles so the GB CPU never overshoots PPU on a sub-opcode boundary.
static constexpr int kMaxOpcodeTCycles = 24;

class Cpu
{
public:
	void Reset();
	void Step(Memory &mem);           // execute one instruction, may be multi-cycle
	int  ServiceInterrupts(Memory &mem); // returns T-cycles consumed (0 or 20)

	CpuState &State() { return state_; }
	const CpuState &State() const { return state_; }

	// P1a installs a process-global trace hook. nullptr disables.
	static void SetTraceHook(TraceHook hook);

private:
	CpuState state_{};
};

} // namespace SGB

#endif
