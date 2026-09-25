/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Carts whose game runs on an RP2040 (Bitmap Bureau's Xeno Crisis). The SNES
// side is a thin client: it polls $3000 for commands and executes 65816
// code the RP2040 streams through that same byte, one byte per bus read.
// The RP2040's flash image is a separate file, <rom name>_rp2040.bin, beside
// the ROM or packed in the same zip.

#ifndef _RP2040CART_H_
#define _RP2040CART_H_

#include "port.h"

bool8  S9xRP2040CartDetect (const uint8 *rom, uint32 size);
const char *S9xRP2040CartTitle (void);		// window title, e.g. "Xeno Crisis"
void   S9xRP2040CartSetArchive (const char *archive_path);	// the ROM came out of this archive ("" = none)
bool8  S9xRP2040CartActivate (const char *rom_path);	// loads the firmware
void   S9xRP2040CartDeactivate (void);
void   S9xRP2040CartPowerOn (void);
void   S9xRP2040CartSoftReset (void);
void   S9xRP2040CartEndScanline (void);

uint8  S9xRP2040CartRead (uint32 address);
void   S9xRP2040CartWrite (uint8 byte, uint32 address);
void   S9xRP2040CartDMAPrefetch (void);		// $420B written: fetch the next opcode first

// Debug: called for every access to the window (write = true for writes).
extern void (*S9xRP2040CartTraceHook) (bool write, uint32 address, uint8 byte);
namespace RP2040 { class Chip; }
RP2040::Chip *S9xRP2040CartChip (void);

// The game saves to its own flash; the .srm holds the 4K sectors that
// differ from the firmware file.
bool8  S9xRP2040CartLoadFlash (const char *srm_path);
bool8  S9xRP2040CartSaveFlash (const char *srm_path);

// The same .srm as a fixed-size, zero-padded buffer, for frontends that
// own the save file (libretro SAVE_RAM). Sync applies a frontend-loaded image.
uint8  *S9xRP2040CartSaveImage (void);
size_t S9xRP2040CartSaveImageSize (void);
void   S9xRP2040CartSyncSaveImage (void);

size_t S9xRP2040CartStateSize (void);
void   S9xRP2040CartStateSave (uint8 *buf);
bool8  S9xRP2040CartStateLoad (const uint8 *buf, size_t size);

#endif
