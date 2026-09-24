/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Disc: the unreleased Sony/Nintendo SNES CD-ROM unit. The BIOS
// cartridge holds a 128K LoROM BIOS, 256K of work DRAM at 80h-87h:8000h and
// 8K of battery SRAM at 90h:8000h; the drive side is a mechacon MCU talked
// to one nibble at a time through $21E1 and a Sony CXD1800 decoder behind
// $21E2/$21E3, both raising the SNES /IRQ line. See docs/superdisc.md.

#ifndef _SUPERDISC_H_
#define _SUPERDISC_H_

#include "port.h"

#define SDISC_BIOS_SIZE		0x20000
#define SDISC_DRAM_SIZE		0x40000
#define SDISC_SRAM_SIZE		0x2000
#define SDISC_BUFFER_SIZE	0x8000		// CXD1800 sector buffer (32K SRAM)

// Drive states, as status digit 2 reports them.
enum
{
	SDISC_NO_DISC	= 0x00,
	SDISC_STOP		= 0x01,
	SDISC_PLAY		= 0x02,
	SDISC_PAUSE		= 0x03,
	SDISC_FAST_REV	= 0x04,
	SDISC_FAST_FWD	= 0x05,
	SDISC_SLOW_REV	= 0x06,
	SDISC_SLOW_FWD	= 0x07,
	SDISC_SEEK		= 0x0A,
	SDISC_READ_TOC	= 0x0B,
	SDISC_TRAY_OPEN	= 0x0C
};

// True for the SDBR v0.95 image (or any BIOS with its tell-tale header: an
// FFh-filled title block and native NMI/IRQ vectors at 1FF8h/1FFCh).
bool8 S9xSuperDiscIsBIOS (const uint8 *rom, uint32 size);

// A CD image File -> Load Game should boot through the Super Disc BIOS.
bool8 S9xSuperDiscIsDiscImage (const char *path);

void  S9xSuperDiscActivate (void);		// InitROM found the BIOS cart
void  S9xSuperDiscDeactivate (void);	// a different ROM is loading
void  S9xSuperDiscPowerOn (void);		// S9xReset
void  S9xSuperDiscSoftReset (void);		// the reset button: the drive keeps spinning
void  S9xSuperDiscEndScanline (void);

// Disc swapping. Insert closes the tray on the new image; Eject opens it.
bool8 S9xSuperDiscInsertDisc (const char *path);
void  S9xSuperDiscEjectDisc (void);
bool8 S9xSuperDiscHasDisc (void);
const char *S9xSuperDiscDiscPath (void);
const char *S9xSuperDiscTitle (void);	// "Super Disc (v0.95) - <disc name>"

uint8 *S9xSuperDiscDRAM (void);
void  S9xSuperDiscRemapSRAM (void);		// apply the SRAM write lock to WriteMap

uint8 S9xGetSuperDisc (uint16 address);
void  S9xSetSuperDisc (uint8 byte, uint16 address);

// CD-DA and XA-ADPCM output, 44.1kHz stereo, mixed like the MSU-1's.
class Resampler;
void  S9xSuperDiscSetOutput (Resampler *resampler);
void  S9xSuperDiscGenerate (size_t sample_count);

size_t S9xSuperDiscStateSize (void);
void  S9xSuperDiscStateSave (uint8 *buf);
bool8 S9xSuperDiscStateLoad (const uint8 *buf, size_t size);

#endif
