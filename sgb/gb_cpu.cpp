/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ops.h"

namespace SGB {

namespace {
TraceHook g_trace_hook = nullptr;
uint32_t  g_irq_serviced[5] = {0, 0, 0, 0, 0};

// PC trace ring — see PcTraceEntry in gb_cpu.h. Frozen on RST 38 loop
// detection so post-crash $0038/$0039 spinning doesn't overwrite the
// pre-crash trail leading into the crash.
PcTraceEntry g_pc_trace[kPcTraceCap]{};
uint32_t     g_pc_trace_head     = 0;
uint32_t     g_pc_trace_count    = 0;
bool         g_pc_trace_frozen   = false;
uint32_t     g_rst38_consecutive = 0;

inline void PcTracePush(uint16_t pc, uint8_t opcode, const CpuState &s)
{
	if (g_pc_trace_frozen) return;

	PcTraceEntry &e = g_pc_trace[g_pc_trace_head];
	e.pc     = pc;
	e.sp     = s.r.sp;
	e.af     = s.r.af;
	e.opcode = opcode;
	e.ime    = s.ime ? 1 : 0;
	g_pc_trace_head = (g_pc_trace_head + 1) % kPcTraceCap;
	if (g_pc_trace_count < kPcTraceCap) ++g_pc_trace_count;

	// Freeze the trace as soon as we see 2 consecutive instructions in
	// $0038/$0039 — the RST 38 trap loop. Threshold 2 (not 1) protects
	// against a single legitimate RST 38h call; threshold low enough that
	// the 64-entry ring still has ~62 pre-crash instructions when freeze
	// triggers, instead of being fully overwritten by the loop itself.
	if (pc == 0x0038 || pc == 0x0039)
	{
		if (++g_rst38_consecutive >= 2)
			g_pc_trace_frozen = true;
	}
	else
	{
		g_rst38_consecutive = 0;
	}
}
}

uint32_t PcTraceCount()  { return g_pc_trace_count; }
bool     PcTraceFrozen() { return g_pc_trace_frozen; }

bool PcTraceGet(uint32_t i, PcTraceEntry &out)
{
	if (i >= g_pc_trace_count) return false;
	const uint32_t start = (g_pc_trace_count < kPcTraceCap)
		? 0 : g_pc_trace_head;
	out = g_pc_trace[(start + i) % kPcTraceCap];
	return true;
}

void PcTraceReset()
{
	g_pc_trace_head     = 0;
	g_pc_trace_count    = 0;
	g_pc_trace_frozen   = false;
	g_rst38_consecutive = 0;
}

uint32_t IrqServicedCount(uint8_t vector)
{
	return (vector < 5) ? g_irq_serviced[vector] : 0;
}

void IrqServicedReset()
{
	for (int i = 0; i < 5; ++i) g_irq_serviced[i] = 0;
	PcTraceReset();
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
			state_.t_cycles += 4;
			return;
		}
	}

	// STOP wakes only on a joypad IRQ being raised (Pan Docs). Unlike
	// HALT, it does NOT wake on VBlank/Timer/Serial/LCDSTAT, and a
	// stopped CPU doesn't service regular interrupts either.
	if (state_.stopped)
	{
		if (mem.if_ & IRQ_JOYPAD)
			state_.stopped = false;
		else
		{
			state_.t_cycles += 4;
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

	PcTracePush(pc_at_fetch, op, state_);

	if (g_trace_hook) g_trace_hook(pc_at_fetch, op, state_);

	Dispatch(state_, mem, op);

	if (promote_ime_after)
	{
		state_.ime         = true;
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

	// Priority order matches bit order: VBlank(0) > LCDSTAT(1) > Timer(2)
	// > Serial(3) > Joypad(4). Service the lowest set bit first.
	uint8_t bit = 0;
	while (!(pending & (1u << bit)))
		++bit;
	const uint16_t vector = static_cast<uint16_t>(0x40 + bit * 8);

	mem.if_       = static_cast<uint8_t>(mem.if_ & ~(1u << bit));
	state_.ime    = false;
	state_.halted = false;
	if (bit < 5) ++g_irq_serviced[bit];

	// Push PC, then jump to vector.
	Push16(state_, mem, state_.r.pc);
	state_.r.pc = vector;

	// Dispatch is 5 M-cycles = 20 T-cycles per Pan Docs.
	state_.t_cycles += 20;
	return 20;
}

} // namespace SGB
