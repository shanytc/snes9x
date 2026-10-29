#ifndef WBARCODES_H
#define WBARCODES_H

#include <windows.h>

#include "../barcodereaders.h"

// Input > Barcodes > Scan Barcode: a card's code, typed in, made up, picked
// from the reader's known cards, or read off a picture or a webcam held up to
// the card. The reader is the one the loaded game's system takes: the Barcode
// Battler II, the Barcode Boy or Barcode Taisen Bardigun's (barcodereaders.h).
// The webcam is the Game Boy Camera's; the dialog runs it while open, then
// puts it back as Settings.GBVideoCamera has it. Returns true when a code was
// scanned.
bool WinBarcodeDialog(HWND parent, S9xBarcodeReader reader);

// The OSD line for the code just scanned: the card's name when it is a known one.
void WinBarcodeScanned(S9xBarcodeReader reader);

#endif
