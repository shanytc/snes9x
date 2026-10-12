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

#endif
