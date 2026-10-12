/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native Seta uPD96050 chips: each firmware command as a program on the chip's own
// clock (upd96050.cpp), matching its results and the instruction count of every
// access to the RAM it shares with the CPU, with no dump.

#ifndef _UPD96050N_H_
#define _UPD96050N_H_

#include "port.h"

struct S9xUPD96050Native
{
	// DR and SR, and the shared RAM (2048 words, low byte first)
	void	(*attach) (uint16 *dr, uint16 *sr, uint8 *ram);
	void	(*reset) (void);
	// at its command wait, as a state from the firmware leaves it
	void	(*idle) (void);
	// runs `budget` instructions: a native chip always spends all of them, waiting or not. Returns how
	// many more it runs before its next counted access (DR, SR or the RAM), or 0 if only the CPU's next
	// DR access moves it on.
	uint64	(*run) (uint64 budget);
	uint32	(*state_size) (void);
	void	(*state_save) (uint8 *buf);
	bool8	(*state_load) (const uint8 *buf, uint32 size);
#ifdef UPD96050_LAB
	// every RAM access with the chip's instruction count, as the firmware's lab hook reports them
	void	(*lab_events) (void (*fn) (char kind, uint64 at, uint32 word, uint16 v));
#endif
};

extern const S9xUPD96050Native	S9xST010Native;
extern const S9xUPD96050Native	S9xST011Native;

#endif
