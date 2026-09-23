/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Zilog Z80 CPU core for the Nintendo Super System supervisor board (nss.cpp).
// Plain Z84C00 at 4MHz: no MMU, no on-chip peripherals, and every
// undocumented encoding executes the way an NMOS/CMOS Z80 does rather than
// trapping — that is what separates it from the Z180 core in hd64180.cpp,
// which the SFC-Box needs precisely because it traps.
// See docs/nss.md for the board-level picture.

#ifndef _Z80_H_
#define _Z80_H_

#include "port.h"

struct SZ80Callbacks
{
	uint8	(*MemRead) (uint16 addr);
	void	(*MemWrite) (uint16 addr, uint8 byte);
	uint8	(*IORead) (uint16 port);
	void	(*IOWrite) (uint16 port, uint8 byte);
	// Byte the peripheral puts on the bus during an IM0/IM2 interrupt ack.
	// NULL reads as FFh (RST 38h in IM0).
	uint8	(*IntAck) (void);
};

struct SZ80
{
	// Register file
	uint8	A, F, B, C, D, E, H, L;
	uint8	A2, F2, B2, C2, D2, E2, H2, L2;	// shadow set
	uint16	IX, IY, SP, PC;
	uint16	WZ;				// MEMPTR: the undocumented internal pointer
	uint8	I, R;
	uint8	IFF1, IFF2, IM;
	uint8	Halted;
	uint8	EIPending;		// EI defers interrupt acceptance by one instruction

	// Interrupt lines (1 = asserted). INT is level-triggered, NMI edge.
	uint8	INTLine;
	uint8	NMIPending;

	int32	Cycles;			// consumed within the current Execute() slice
	uint32	TotalCycles;	// lifetime, for board debug traces
};

extern struct SZ80				Z80;
extern struct SZ80Callbacks		Z80CB;

void	Z80_Reset (void);
// Run at least `cycles` T-states (finishes the instruction in progress);
// returns the number actually consumed.
int32	Z80_Execute (int32 cycles);
void	Z80_SetINT (bool8 asserted);
void	Z80_PulseNMI (void);

#endif
