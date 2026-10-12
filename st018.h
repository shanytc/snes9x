/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Seta ST018: an ARMv3 (ARM60) core with its own ROM and RAM, run from a dump of
// its firmware, and the byte-wide bridge it talks to the SNES through at
// $00-$3F/$80-$BF:3800-38FF. It catches up to the CPU on each access.

#ifndef _ST018_H_
#define _ST018_H_

#include "port.h"

#define ST018_PROGRAM_SIZE	0x20000		// the ARM's program ROM, then
#define ST018_DATA_SIZE		0x8000		// its data ROM, in the dump

// Whether an image is laid out like a firmware dump.
bool8	S9xST018IsFirmware (const uint8 *image, uint32 size);

// Starts running `image`; FALSE (and off) if it isn't a firmware dump.
bool8	S9xST018Load (const uint8 *image, uint32 size);
void	S9xST018Unload (void);
bool8	S9xST018Loaded (void);
// Whether the chip, rather than the HLE, is answering the CPU right now.
bool8	S9xST018Active (void);
// Power-on, and back in charge if a state had handed off to the HLE.
void	S9xST018Reset (void);
// A state saved without the firmware: the HLE carries on.
void	S9xST018Suspend (void);
void	S9xST018EndScanline (void);

// `speed` is the bus cycle's length in master clocks; below 0, a peek with no side effects.
uint8	S9xST018Read (uint32 address, int32 speed);
void	S9xST018Write (uint32 address, uint8 byte, int32 speed);

uint32	S9xST018StateSize (void);
void	S9xST018StateSave (uint8 *buf);
bool8	S9xST018StateLoad (const uint8 *buf, uint32 size);	// resumes the chip

// The native chip (st018n.cpp): the program translated to C, the ROMs built in.
bool8	S9xST018LoadNative (void);
bool8	S9xST018IsNative (void);

// What the bridge holds, shared by the CPU's side and the ARM's.
struct S9xST018Bus
{
	uint8	cputoarm_ready, cputoarm_data, armtocpu_ready, armtocpu_data;
	uint8	reset, ready, signal, pad;
	uint32	timer, timerlatch;
};

// The ARM's registers and clock, as the firmware core saves them.
struct S9xST018Arm
{
	uint32	r[16];				// r[15]: the next instruction's address
	uint32	bank_r8[2][5], bank_sp[6], bank_lr[6], spsr[6];
	uint32	cpsr, last_fetch, boot_wait, pad;
	uint64	clocks;
};

void	S9xST018NativeAttach (S9xST018Bus *bus, uint8 *ram);
void	S9xST018NativeSetArm (const S9xST018Arm *arm);
void	S9xST018NativeGetArm (S9xST018Arm *arm);
// Runs the ARM so every bridge access before clock `target` has happened. Returns the clock of its next
// access the CPU can see (it may have run ahead to it), or ~0 if only the CPU's next bridge access moves it.
uint64	S9xST018NativeRun (uint64 target);

#endif
