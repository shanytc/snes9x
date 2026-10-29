/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_BARDIGUN_H_
#define _SGB_GB_BARDIGUN_H_

#include "gb_barcodeboy.h"

#include <cstddef>
#include <cstdint>

namespace SGB {

// TAM's card reader for Barcode Taisen Bardigun (1998), on player 1's link
// port (Dan Docs, Barcode Taisen Bardigun). The game clocks $FF out on its
// own clock and each byte back is eight samples of the light sensor, first
// in bit 7: 1 white, 0 black. Idle, the reader answers $00.

void BardigunConnect(bool on);
bool BardigunConnected();

// Swipes a card: a JAN/EAN-13 or EAN-8 code (spaces and dashes are ignored).
// It waits in the reader until the game samples it. Returns false, changing
// nothing, for anything else.
bool BardigunScan(const char *code);
bool BardigunPending();
const char *BardigunLastCode();     // "" before the first scan
uint32_t BardigunReadCount();       // swipes the game sampled to the end

// gb_serial, while the reader holds the port. Exchange: the samples it shifts
// back for a byte the game clocks. Clock: `cycles` real T-cycles passing.
uint8_t BardigunExchange();
uint8_t BardigunReply();            // what the last exchange shifts back
void    BardigunClock(int32_t cycles);

// Savestates carry the swipe in flight and a card waiting in the reader.
size_t BardigunStateBytes();
void   BardigunStateSave(uint8_t *out);
void   BardigunStateLoad(const uint8_t *in);

// The bundled cards, then other barcodes known to hatch a Bardigun. Photos,
// when there are any, sit beside the Barcode Boy's.
const BarcodeBoyCard *BardigunCards(int *count);

// "Barcode Taisen Bardigun" for its header title, else nullptr.
const char *BardigunGame(const char *title);

// The bars of an EAN-13 or EAN-8 code as the reader sweeps them, a module each
// (true = bar): 95 or 67 written to bars, 0 for anything else.
int EanBars(const char *code, bool *bars);

} // namespace SGB

#endif
