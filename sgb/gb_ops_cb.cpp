/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// CB-prefixed opcodes — rotate/shift + bit manipulation on any register
// or (HL). P1c fills in the full 256-case table; this stub only ticks
// cycles so code that touches any CB instruction doesn't stall the clock.
//
// Cycle counts (from Pan Docs):
//   CB + op on register:          8 T-cycles (2 M-cycles incl. prefix)
//   CB + BIT n,(HL):             12 T-cycles (3 M-cycles)
//   CB + RES/SET/shift on (HL):  16 T-cycles (4 M-cycles)
// The stub uses the minimum (8). Correct timing lands in P1c.

#include "gb_ops.h"

namespace SGB {

void DispatchCB(CpuState &s, Memory & /*mem*/, uint8_t /*op*/)
{
	s.t_cycles += 8;
}

} // namespace SGB
