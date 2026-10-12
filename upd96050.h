/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// NEC uPD96050, the Seta ST010/ST011 cartridge chip, run from a dump of its firmware.
// It runs on the cart's own oscillator and catches up to the CPU on each access.
// Its 2K-word data RAM is the cart's battery RAM, Memory.SRAM, low byte first.

#ifndef _UPD96050_H_
#define _UPD96050_H_

#include "port.h"

#define UPD96050_FIRMWARE_SIZE	0xd000	// 16384 24-bit program words, then 2048 16-bit data words

// Which Seta chip it is.
enum
{
	UPD96050_ST010 = 1,		// F1 ROC II, Exhaust Heat II: 11 MHz
	UPD96050_ST011			// Hayazashi Nidan Morita Shougi: 15 MHz
};

// Whether an image is laid out like a firmware dump.
bool8	S9xUPD96050IsFirmware (const uint8 *image, uint32 size);

// Starts running `image`; FALSE (and off) if it isn't a firmware dump.
bool8	S9xUPD96050Load (const uint8 *image, uint32 size, int chip);
// No dump: the chip runs natively (upd96050n.h); FALSE for one that has no native version.
bool8	S9xUPD96050LoadNative (int chip);
void	S9xUPD96050Unload (void);
// Whether the loaded chip is the native one rather than a firmware dump.
bool8	S9xUPD96050IsNative (void);
bool8	S9xUPD96050Loaded (void);
// Whether the chip, rather than the HLE, is answering the CPU right now.
bool8	S9xUPD96050Active (void);
int		S9xUPD96050Chip (void);
// Back to power-on, and back in charge if a state had handed off to the HLE.
void	S9xUPD96050Reset (void);
// A state saved without the firmware: the HLE carries on.
void	S9xUPD96050Suspend (void);
void	S9xUPD96050EndScanline (void);

// $60-$67:0000-3FFF are DR (A0 = 0) and SR (A0 = 1), $68-$6F:0000-7FFF the data RAM.
// `speed` is the bus cycle's length in master clocks; below 0, a peek with no side effects.
uint8	S9xUPD96050Read (uint32 address, int32 speed);
void	S9xUPD96050Write (uint32 address, uint8 byte, int32 speed);

uint32	S9xUPD96050StateSize (void);
void	S9xUPD96050StateSave (uint8 *buf);
bool8	S9xUPD96050StateLoad (const uint8 *buf, uint32 size);	// resumes the chip

#endif
