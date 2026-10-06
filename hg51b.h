/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Hitachi HG51B169, Capcom's Cx4 cartridge chip, on its own 20 MHz clock.
// It runs code the cart's ROM holds; its one dump is a 3 KB data ROM of math
// tables. It catches up to the CPU on each access and at scanline end.

#ifndef _HG51B_H_
#define _HG51B_H_

#include "port.h"

#define HG51B_DATAROM_SIZE	3072	// 1024 24-bit words, low byte first

// Whether an image is laid out like the data ROM dump.
bool8	S9xHG51BIsDataROM (const uint8 *image, uint32 size);

// Starts running with `image`; FALSE (and off) if it isn't the data ROM.
bool8	S9xHG51BLoad (const uint8 *image, uint32 size);
void	S9xHG51BUnload (void);
bool8	S9xHG51BLoaded (void);
// Whether the chip, rather than the HLE, is answering the CPU right now.
bool8	S9xHG51BActive (void);
// Back to power-on, and back in charge if a state had handed off to the HLE.
void	S9xHG51BReset (void);
// A state saved without the data ROM: the HLE state it carries takes over.
void	S9xHG51BSuspend (void);
void	S9xHG51BEndScanline (void);

// $6000-$7FFF in banks $00-$3F/$80-$BF. `speed` is the bus cycle's length;
// < 0 is a cheat or debugger peek, which doesn't advance the chip.
uint8	S9xHG51BRead (uint16 address, int32 speed);
void	S9xHG51BWrite (uint8 byte, uint16 address, int32 speed);

uint32	S9xHG51BStateSize (void);
void	S9xHG51BStateSave (uint8 *buf);
bool8	S9xHG51BStateLoad (const uint8 *buf, uint32 size);	// resumes the chip

#endif
