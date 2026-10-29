#ifndef WBARCODEBOY_H
#define WBARCODEBOY_H

#include <windows.h>

// Input > Barcode Boy > Scan Barcode: a card's 13 digits, typed in, made up,
// picked from the known cards, or read off a picture or a webcam held up to the
// card, as the Barcode Battler II dialog does. The webcam is the Game Boy
// Camera's; the dialog runs it while open, then puts it back as
// Settings.GBVideoCamera has it. Scan hands the code to the Barcode Boy on the
// link port (sgb/gb_barcodeboy.h). Returns true when a code was scanned.
bool WinBarcodeBoyDialog(HWND parent);

// The OSD line for the code just scanned: the card's name when it is a known one.
void WinBarcodeBoyScanned();

// Input > Bardigun Card Reader > Scan Barcode: the same dialog for Barcode Taisen
// Bardigun's reader (sgb/gb_bardigun.h), which also takes 8-digit EAN-8 codes.
bool WinBardigunDialog(HWND parent);
void WinBardigunScanned();

#endif
