#ifndef ACID_TESTS_DLG_H
#define ACID_TESTS_DLG_H

// Tests > Acid Tests: modeless GB Emulator Shootout runner. Runs on its own
// emulator cores off the UI thread, so the loaded session keeps playing.
// Brings the dialog forward when it is already open.
void WinShowAcidTestsDialog();

// The open dialog, or NULL; the main loop routes its keys (IsDialogMessage).
HWND WinAcidTestsDialog();

// True when a complete test pack is installed next to the exe (or one or
// two levels up, for a build tree): manifest.txt, the ROMs under tests/,
// and the reference screens under baseline/default/. The pack ships
// separately, so the Tests menu only appears when all three are there;
// nothing else in the emulator depends on it.
bool WinAcidTestsAvailable();

#endif
