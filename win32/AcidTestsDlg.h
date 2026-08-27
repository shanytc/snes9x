#ifndef ACID_TESTS_DLG_H
#define ACID_TESTS_DLG_H

// Emulation > Acid Tests: modal GB Emulator Shootout runner. Runs on its
// own emulator cores, so the loaded session is left alone.
void WinShowAcidTestsDialog();

// True when the test pack is installed - an acid/ folder with a readable
// manifest.txt next to the exe (or one or two levels up, for a build tree).
// The pack ships separately, so the menu entry only appears when it is
// there; nothing else in the emulator depends on it.
bool WinAcidTestsAvailable();

#endif
