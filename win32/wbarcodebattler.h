#ifndef WBARCODEBATTLER_H
#define WBARCODEBATTLER_H

#include <windows.h>

// Input > Barcodes > Scan Barcode: the code to swipe, typed in,
// made up, read off a picture or off a webcam held up to the card. Scan hands
// it to the BBII interface on port 2 (barcodebattler.h). The webcam is the one
// Video > Display Configuration turns on and picks for the Game Boy Camera;
// the dialog only reads it. Modeless, so the game keeps running while cards
// are browsed and scanned; showing it again brings it forward.
void WinShowBarcodeBattlerDialog(HWND parent);
HWND WinBarcodeBattlerDialog();		// the open dialog, or NULL
void WinCloseBarcodeBattlerDialog();	// a game load: the dialog was for the last one
void WinBarcodeBattlerCameraChanged();	// Display Configuration applied its camera

#endif
