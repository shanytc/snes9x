/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cpu.h"
#include "gb_memory.h"

namespace SGB {

namespace {

TraceHook g_trace_hook = nullptr;

inline uint8_t Fetch8(CpuState &s, Memory &mem)
{
	uint8_t b = MemRead(mem, s.r.pc);
	s.r.pc++;
	return b;
}

} // anonymous

void Cpu::SetTraceHook(TraceHook hook)
{
	g_trace_hook = hook;
}

void Cpu::Reset()
{
	state_ = {};
	// DMG post-boot register state (Pan Docs: Power Up Sequence).
	// SGB1/SGB2 have slightly different A/BC/HL values; P7 will re-apply
	// them when RunMode indicates SGB after Reset().
	state_.r.af = 0x01B0;
	state_.r.bc = 0x0013;
	state_.r.de = 0x00D8;
	state_.r.hl = 0x014D;
	state_.r.sp = 0xFFFE;
	state_.r.pc = 0x0100;
	state_.ime = false;
	unknown_opcodes_ = 0;
}

void Cpu::Step(Memory &mem)
{
	// EI has a one-instruction delay: the IME bit becomes true *after* the
	// instruction that follows EI completes. We latch the pending state up
	// front so an EI instruction setting it this cycle doesn't accidentally
	// promote itself.
	const bool promote_ime_after = state_.ime_pending;

	// While halted, tick 4 T-cycles per poll and wait for any pending IRQ
	// to lift us back out. IRQ dispatch itself happens in ServiceInterrupts.
	if (state_.halted)
	{
		const uint8_t pending = mem.ie & mem.if_ & IRQ_ALL;
		if (pending)
			state_.halted = false;
		else
		{
			state_.t_cycles += 4;
			return;
		}
	}

	const uint16_t pc_at_fetch = state_.r.pc;
	const uint8_t  op          = Fetch8(state_, mem);

	// HALT bug: if HALT executes with IME=0 but an IRQ is pending, the CPU
	// does NOT halt and the byte immediately after HALT gets read twice.
	// We reverse the PC increment here so the next dispatch refetches it.
	if (state_.halt_bug)
	{
		state_.r.pc--;
		state_.halt_bug = false;
	}

	if (g_trace_hook) g_trace_hook(pc_at_fetch, op, state_);

	switch (op)
	{
		case 0x00:  // NOP
			state_.t_cycles += 4;
			break;

		case 0x76:  // HALT
			state_.t_cycles += 4;
			if (!state_.ime && (mem.ie & mem.if_ & IRQ_ALL))
			{
				// IME off + IRQ pending = HALT bug, no actual halt.
				state_.halt_bug = true;
			}
			else
			{
				state_.halted = true;
			}
			break;

		default:
			// P1b/c fill in the full opcode table. Until then, treat
			// unknowns as NOPs so the dispatcher still advances and we
			// can run the harness without hard-crashing. Count them so
			// P1b can triage what's still missing.
			++unknown_opcodes_;
			state_.t_cycles += 4;
			break;
	}

	if (promote_ime_after)
	{
		state_.ime         = true;
		state_.ime_pending = false;
	}
}

int Cpu::ServiceInterrupts(Memory & /*mem*/)
{
	// P1c implements the full 20-T-cycle dispatch (push PC, jump to vector).
	return 0;
}

} // namespace SGB
