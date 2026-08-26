/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// DIV/TIMA timer — GB hardware clocks TIMA from a falling edge of a
// specific DIV bit, not from a standalone counter. This file emulates
// that exactly so Blargg's timer tests (and the edge cases they exist
// to catch) pass.
//
//   TAC bits 1:0  selected bit   TIMA rate
//     00            bit 9          4096 Hz  (period 1024 T-cycles)
//     01            bit 3        262144 Hz  (period   16)
//     10            bit 5         65536 Hz  (period   64)
//     11            bit 7         16384 Hz  (period  256)
//
// When TIMA overflows (0xFF → 0x00) the reload from TMA is delayed by
// 4 T-cycles. During that window TIMA reads as 0. Writing TIMA during
// the delay cancels the reload and the IRQ.

#include "gb_timer.h"
#include "gb_memory.h"
#include "gb_cpu.h"
#include "gb_apu.h"

namespace SGB {

namespace {

// Which bit of the 16-bit DIV counter's falling edge clocks TIMA.
constexpr uint8_t TIMA_BIT[4] = { 9, 3, 5, 7 };

inline bool DivBit(uint16_t div, uint8_t bit)
{
	return ((div >> bit) & 1) != 0;
}

// Fired when a falling-edge is detected on the selected DIV bit.
inline void ClockTima(Timer &t)
{
	++t.tima;
	if (t.tima == 0)
	{
		t.tima_overflow_pending = true;
		t.reload_delay = 4;
	}
}

} // anonymous

void TimerReset(Timer &t)
{
	t.div_counter            = 0;
	t.tima                   = 0;
	t.tma                    = 0;
	t.tac                    = 0;
	t.tima_overflow_pending  = false;
	t.reload_delay           = 0;
	t.reload_just            = 0;
}

void TimerStep(Timer &t, Memory &mem, int32_t tcycles)
{
	for (int32_t i = 0; i < tcycles; ++i)
	{
		if (t.reload_just > 0) --t.reload_just;

		// Pending overflow: tick down, then reload and raise IRQ.
		if (t.tima_overflow_pending)
		{
			if (--t.reload_delay <= 0)
			{
				t.tima                  = t.tma;
				t.tima_overflow_pending = false;
				t.reload_just           = 4;
				mem.if_ = static_cast<uint8_t>(mem.if_ | IRQ_TIMER);
			}
		}

		const uint16_t prev_div = t.div_counter;
		++t.div_counter;

		if (t.tac & 0x04)
		{
			const uint8_t bit = TIMA_BIT[t.tac & 0x03];
			if (DivBit(prev_div, bit) && !DivBit(t.div_counter, bit))
				ClockTima(t);
		}

		// Internal-clock serial: one bit per falling edge of DIV bit 8
		// (8192 Hz); disconnected MISO shifts in 1s. The byte lands and
		// the IRQ fires when the 8th bit clocks (mooneye boot_sclk_align).
		if (mem.serial_guard > 0)
			--mem.serial_guard;
		else if (mem.serial_bits > 0 &&
		    DivBit(static_cast<uint16_t>(prev_div + 4), 8) &&
		    !DivBit(static_cast<uint16_t>(t.div_counter + 4), 8))
		{
			mem.serial_data = static_cast<uint8_t>((mem.serial_data << 1) | 1);
			if (--mem.serial_bits == 0)
			{
				mem.if_            = static_cast<uint8_t>(mem.if_ | IRQ_SERIAL);
				mem.serial_control = static_cast<uint8_t>(mem.serial_control & 0x7F);
			}
		}

		// DIV-APU: the primary event fires on the falling edge of DIV
		// bit 12 (13 in double speed), the secondary on the rising edge.
		if (mem.apu)
		{
			const uint8_t abit = mem.double_speed ? 13 : 12;
			const bool was = DivBit(prev_div, abit);
			const bool now = DivBit(t.div_counter, abit);
			if (was && !now)      ApuDivEvent(*mem.apu, mem.double_speed);
			else if (!was && now) ApuDivSecondaryEvent(*mem.apu);
		}
	}
}

// DIV reset (FF04 write / STOP): both the TIMA mux and the DIV-APU see
// any resulting falling edge.
void TimerResetDiv(Timer &t, Memory &mem)
{
	const uint16_t old = t.div_counter;
	t.div_counter = 0;
	if ((t.tac & 0x04) && DivBit(old, TIMA_BIT[t.tac & 0x03]))
		ClockTima(t);
	if (mem.apu)
	{
		const uint8_t abit = mem.double_speed ? 13 : 12;
		if (DivBit(old, abit))
			ApuDivEvent(*mem.apu, mem.double_speed);
	}
}

uint8_t TimerRead(const Timer &t, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF04:
			return static_cast<uint8_t>(t.div_counter >> 8);
		case 0xFF05: return t.tima;
		case 0xFF06: return t.tma;
		case 0xFF07: return static_cast<uint8_t>(t.tac | 0xF8);
	}
	return 0xFF;
}

void TimerWrite(Timer &t, Memory &mem, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF04:
			// DIV reset — spurious TIMA tick + DIV-APU edge handled in
			// the shared helper.
			TimerResetDiv(t, mem);
			return;

		case 0xFF05:
			// Writing TIMA during the 4-cycle reload delay cancels the
			// pending reload + IRQ. A write landing on the reload cycle
			// itself is ignored — TMA wins.
			if (t.reload_just > 0) return;
			if (t.tima_overflow_pending) t.tima_overflow_pending = false;
			t.tima = value;
			return;

		case 0xFF06:
			t.tma = value;
			// TMA written on the reload cycle propagates into TIMA too.
			if (t.reload_just > 0) t.tima = value;
			return;

		case 0xFF07:
		{
			// Changing TAC can cause a spurious TIMA tick when the
			// multiplexer sees a 1 → 0 transition on the effective
			// "enabled && selected-bit" signal. We model that here.
			const bool old_bit    = (t.tac & 0x04) && DivBit(t.div_counter, TIMA_BIT[t.tac & 0x03]);
			t.tac                 = static_cast<uint8_t>(value & 0x07);
			const bool new_bit    = (t.tac & 0x04) && DivBit(t.div_counter, TIMA_BIT[t.tac & 0x03]);
			if (old_bit && !new_bit) ClockTima(t);
			return;
		}
	}
}

} // namespace SGB
