/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native DSP-n chips: each firmware command as a program on the uPD77C25's own
// handshake and clock (upd7725.cpp), matching the chip's results and its
// instruction count between every bus access, with no dump.

#ifndef _UPD7725N_H_
#define _UPD7725N_H_

#include "port.h"

// A native chip parked in a lane (a loop of words it waits on the CPU for) posts its next turn here.
struct S9xUPD7725Lane
{
	uint32	span;					// instructions from resuming to the next wait (0: not in one; ASK: span_of's)
	bool	(*step) (void);			// takes the turn, parked at that wait; false: the program goes elsewhere
	uint32	(*effect) (void);		// once freed: instructions to the first thing the CPU could see, either way
	uint32	(*span_of) (void);		// for ASK, at the handshake that frees it: the span, from the word just in
};
#define UPD7725_LANE_ASK	0xffffffffu

// One native chip, as upd7725.cpp drives it. `variant` picks a revision (the first DSP-1 over the DSP-1B).
struct S9xUPD7725Native
{
	// the registers the programs share with the firmware's: DR, SR, RAM, and TR and TRB, which a command
	// can leave to the next (the DSP-3's last symbol, the DSP-2's transparent colour); and where it posts its lane
	void	(*attach) (uint16 *dr, uint16 *sr, uint16 *ram, uint16 *tr, uint16 *trb, S9xUPD7725Lane *lane);
	void	(*reset) (bool8 variant);
	// runs for `budget` instructions or until it waits on the CPU; returns owed()
	uint32	(*run) (uint64 budget);
	// instructions it has to run before its next bus access; 0 while it waits on the CPU
	uint32	(*owed) (void);
	// at its command wait, its last command done, as a state from the firmware leaves it
	void	(*idle) (bool8 variant);
	uint32	(*state_size) (void);
	void	(*state_save) (uint8 *buf);
	bool8	(*state_load) (const uint8 *buf, uint32 size);
#ifdef UPD7725N_LAB
	void	(*lab_events) (void (*fn) (char kind, uint32 n, uint32 val));
	bool	(*lab_idle) (void);
#endif
};

extern const S9xUPD7725Native	S9xDSP1Native;
extern const S9xUPD7725Native	S9xDSP2Native;
extern const S9xUPD7725Native	S9xDSP3Native;

#endif
