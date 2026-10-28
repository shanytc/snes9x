#ifndef WBARCODEBATTLER_H
#define WBARCODEBATTLER_H

#include <windows.h>

// Input > Barcode Battler II > Scan Barcode: the code to swipe, typed in,
// made up, read off a picture or off a webcam held up to the card. Scan hands
// it to the BBII interface on port 2 (barcodebattler.h). The webcam is the one
// the Game Boy Camera uses; the dialog runs it while open, then puts it back
// as Settings.GBVideoCamera has it. Returns true when a code was scanned.
bool WinBarcodeBattlerDialog(HWND parent);

#endif
