/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cpu.h"
#include "gb_memory.h"

namespace SGB {

void Cpu::Reset()
{
	state_ = {};
	// Post-boot state after the real GB boot ROM on DMG.
	// (SGB BIOS sets the same values — verified against Pan Docs.)
	state_.r.af = 0x01B0;
	state_.r.bc = 0x0013;
	state_.r.de = 0x00D8;
	state_.r.hl = 0x014D;
	state_.r.sp = 0xFFFE;
	state_.r.pc = 0x0100;
	state_.ime = false;
}

void Cpu::Step(Memory & /*mem*/)
{
	// P1a wires up the dispatcher; P1b/c fill in opcodes.
	state_.t_cycles += 4;
}

int Cpu::ServiceInterrupts(Memory & /*mem*/)
{
	// P1c implements IRQ dispatch.
	return 0;
}

} // namespace SGB
