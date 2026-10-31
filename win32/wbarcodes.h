#ifndef WBARCODES_H
#define WBARCODES_H

#include <windows.h>

#include "../barcodereaders.h"

// Input > Barcodes > Scan Barcode: a card's code, typed in, made up, picked
// from the reader's known cards, or read off a picture or a webcam held up to
// the card. The reader is the one the loaded game's system takes: the Barcode
// Battler II, the Barcode Boy or Barcode Taisen Bardigun's (barcodereaders.h).
// The webcam is the one Video > Display Configuration turns on and picks for
// the Game Boy Camera; the dialog only reads it. Modeless, so the game keeps
// running while cards are browsed and scanned; showing it again brings it
// forward.
void WinShowBarcodeDialog(HWND parent, S9xBarcodeReader reader);
HWND WinBarcodeDialog();		// the open dialog, or NULL
void WinCloseBarcodeDialog();	// a game load: the dialog was for the last one
void WinBarcodeCameraChanged();	// Display Configuration applied its camera

// The OSD line for the code just scanned: the card's name when it is a known one.
void WinBarcodeScanned(S9xBarcodeReader reader);

#endif
