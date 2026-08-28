/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ops.h"
#include "gb_ppu.h"

namespace SGB {

namespace {
TraceHook g_trace_hook = nullptr;
}

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
	state_.ime  = false;
}

void Cpu::Step(Memory &mem)
{
	// Halt wake-up: any pending IRQ (even with IME=0) clears the halt flag.
	// When IME is also set, ServiceInterrupts will dispatch to the vector
	// below; when IME=0 we just resume execution from PC normally.
	if (state_.halted)
	{
		const uint8_t pending = mem.ie & mem.if_ & IRQ_ALL;
		if (pending)
			state_.halted = false;
		else
		{
			TickM(state_, mem);
			return;
		}
	}

	// STOP wakes only on a joypad IRQ being raised (Pan Docs). Unlike
	// HALT, it does NOT wake on VBlank/Timer/Serial/LCDSTAT, and a
	// stopped CPU doesn't service regular interrupts either.
	if (state_.stopped)
	{
		if (mem.if_ & IRQ_JOYPAD)
		{
			state_.stopped = false;
			if (mem.ppu) PpuOnCpuStopEnd(*mem.ppu);
		}
		else
		{
			TickM(state_, mem);
			return;
		}
	}

	// An IRQ takes precedence over the next instruction when IME is set.
	if (ServiceInterrupts(mem) > 0)
		return;

	// EI has a one-instruction delay: the IME bit becomes true *after* the
	// instruction that follows EI completes. Latch the pending state before
	// the instruction runs so EI doesn't accidentally promote itself.
	const bool promote_ime_after = state_.ime_pending;

	const uint16_t pc_at_fetch = state_.r.pc;
	const uint8_t  op          = Fetch8(state_, mem);

	// HALT bug: if HALT executed with IME=0 and an IRQ was pending, the
	// byte immediately after HALT gets read twice. Rewind PC so the next
	// dispatch refetches it.
	if (state_.halt_bug)
	{
		state_.r.pc--;
		state_.halt_bug = false;
	}

	if (g_trace_hook) g_trace_hook(pc_at_fetch, op, state_);

	Dispatch(state_, mem, op);

	if (promote_ime_after)
	{
		// DI in the EI-delay slot cancels the pending enable (rapid_di_ei).
		if (op != 0xF3)
			state_.ime = true;
		state_.ime_pending = false;
	}
}

int Cpu::ServiceInterrupts(Memory &mem)
{
	if (!state_.ime)
		return 0;

	const uint8_t pending = mem.ie & mem.if_ & IRQ_ALL;
	if (!pending)
		return 0;

	state_.ime    = false;
	state_.halted = false;

	// Dispatch is 5 M-cycles = 20 T-cycles: two internal delay cycles, the
	// PCH push, the PCL push, then the vector-load cycle. The vector (and
	// which IF bit clears) is decided from IE & IF sampled AFTER the PCH
	// push — a push that lands on IE ($FFFF) can redirect the dispatch or
	// cancel it entirely, in which case the CPU jumps to $0000
	// (mooneye interrupts/ie_push).
	TickM(state_, mem);
	TickM(state_, mem);

	state_.r.sp--;
	BusWrite(state_, mem, state_.r.sp, static_cast<uint8_t>(state_.r.pc >> 8));

	uint16_t vector = 0x0000;
	const uint8_t pend2 = mem.ie & mem.if_ & IRQ_ALL;
	if (pend2)
	{
		// Priority order matches bit order: VBlank(0) > LCDSTAT(1) >
		// Timer(2) > Serial(3) > Joypad(4). Lowest set bit wins.
		uint8_t bit = 0;
		while (!(pend2 & (1u << bit)))
			++bit;
		mem.if_ = static_cast<uint8_t>(mem.if_ & ~(1u << bit));
		vector  = static_cast<uint16_t>(0x40 + bit * 8);
	}

	state_.r.sp--;
	BusWrite(state_, mem, state_.r.sp, static_cast<uint8_t>(state_.r.pc & 0xFF));

	TickM(state_, mem);
	state_.r.pc = vector;
	return 20;
}

} // namespace SGB
