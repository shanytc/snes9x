/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _BARCODEBATTLER_H_
#define _BARCODEBATTLER_H_

#include "port.h"

// Epoch Barcode Battler II, linked to controller port 2 through the BBII
// Interface. The interface turns the handheld's 1200-baud ASCII output into a
// controller stream (fullsnes, SNES Add-On Barcode Transmission I/O):
//
//   bits  1-12  zero
//   bits 13-16  controller id 0Eh
//   bits 17-24  extended id 00h
//   bit  25     a barcode is waiting
//   bits 26-28  status, zero
//   bits 29-80  13 digits, 4 bits each, MSB first
//
// A scanned code waits in the interface until the game has clocked out all
// of its digits; then the "waiting" bit drops until the next scan. While
// connected the device replaces the port-2 joypad. The barcode is not part of
// the recorded input, so movies and netplay don't carry it.

void S9xBarcodeBattlerConnect(bool connect);
bool S9xBarcodeBattlerConnected(void);

// Hands the interface a scanned code: 13 digits (EAN-13), 12 (UPC-A, sent
// with a leading zero) or 8 (EAN-8, sent with five leading zeros, as the
// handheld pads it with spaces). Spaces and dashes are ignored. Connects the
// device if it isn't already. Returns false, changing nothing, for anything
// else.
bool S9xBarcodeBattlerScan(const char *code);
bool S9xBarcodeBattlerPending(void);	// scanned, not yet read by the game
void S9xBarcodeBattlerClear(void);		// drop a code the game hasn't read

// The 13 digits last scanned, as a C string ("" before the first scan).
const char *S9xBarcodeBattlerLastCode(void);

// True when the check digit of an 8-, 12- or 13-digit code adds up.
bool S9xBarcodeChecksumValid(const char *code);

// Writes a random EAN-13 code with a valid check digit into out[14].
void S9xBarcodeRandom(char out[14]);

// Looks for an EAN-13, UPC-A or EAN-8 barcode in an 8-bit greyscale picture,
// across rows and columns and in both directions. On success writes the
// digits (13 or 8, NUL-terminated) into out[14] and returns true.
bool S9xBarcodeDecode(const uint8 *luma, int width, int height, int stride, char out[14]);

// Core hooks: the controller bit at stream position idx (0-based), and the
// 16 bits automatic joypad reading latches.
uint8 S9xBarcodeBattlerReadBit(int idx);
uint16 S9xBarcodeBattlerAutoJoypad(void);

#endif
