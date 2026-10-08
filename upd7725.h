/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// NEC uPD77C25, the DSP-n cartridge chip, run from a dump of its firmware.
// It runs on its own 7.6 MHz clock and catches up to the CPU on each access,
// so a game that skips the status poll races it the way it does on hardware.

#ifndef _UPD7725_H_
#define _UPD7725_H_

#include "port.h"

#define UPD7725_FIRMWARE_SIZE	8192	// 2048 24-bit program words, then 1024 16-bit data words

// Whether an image is laid out like a firmware dump.
bool8	S9xUPD7725IsFirmware (const uint8 *image, uint32 size);

// Which DSP-n the chip is.
enum
{
	UPD7725_DSP1 = 1,	// the first DSP-1, as in Pilotwings
	UPD7725_DSP1B,
	UPD7725_DSP2,
	UPD7725_DSP3,
	UPD7725_DSP4
};

// Starts running `image`; FALSE (and off) if it isn't a firmware dump.
bool8	S9xUPD7725Load (const uint8 *image, uint32 size, int chip);
// No dump: the chip runs natively instead (upd7725n.h); FALSE for one that has no native version.
bool8	S9xUPD7725LoadNative (int chip);
void	S9xUPD7725Unload (void);
bool8	S9xUPD7725Loaded (void);
// Whether the chip, rather than the HLE, is answering the CPU right now.
bool8	S9xUPD7725Active (void);
// Back to power-on, and back in charge if a state had handed off to the HLE.
void	S9xUPD7725Reset (void);
// A state saved without the firmware: the HLE state it carries takes over.
void	S9xUPD7725Suspend (void);
void	S9xUPD7725EndScanline (void);

// `sr` picks the status register over the data register; `speed` is the
// bus cycle's length in master clocks; `address`, the CPU's, if it's known.
uint8	S9xUPD7725Read (bool8 sr, int32 speed, int32 address = -1);
void	S9xUPD7725Write (uint8 byte, bool8 sr, int32 speed);

uint32	S9xUPD7725StateSize (void);
void	S9xUPD7725StateSave (uint8 *buf);
bool8	S9xUPD7725StateLoad (const uint8 *buf, uint32 size);	// resumes the chip

#endif
