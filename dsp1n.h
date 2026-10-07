/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The DSP-1 and DSP-1B without a dump: each firmware command as a native program
// on the uPD77C25's own handshake and clock (upd7725.cpp), matching the chip's
// results and its instruction count between every bus access.

#ifndef _DSP1N_H_
#define _DSP1N_H_

#include "port.h"

// The chip's data register, status register and RAM, which the programs share.
void	S9xDSP1NAttach (uint16 *dr, uint16 *sr, uint16 *ram);
// Power-on; `first` picks the first DSP-1 (Pilotwings) over the DSP-1B.
void	S9xDSP1NReset (bool8 first);
// Runs for `budget` instructions, or until the program waits on the CPU; returns S9xDSP1NOwed().
uint32	S9xDSP1NRun (uint64 budget);
// Instructions it has to run before its next bus access; 0 while it waits on the CPU.
uint32	S9xDSP1NOwed (void);
// At the command wait, its last command done, as a state from the firmware leaves it.
void	S9xDSP1NIdle (bool8 first);

uint32	S9xDSP1NStateSize (void);
void	S9xDSP1NStateSave (uint8 *buf);
bool8	S9xDSP1NStateLoad (const uint8 *buf, uint32 size);

#endif
