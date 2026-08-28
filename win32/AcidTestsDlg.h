#ifndef ACID_TESTS_DLG_H
#define ACID_TESTS_DLG_H

// Tests > Acid Tests: modal GB Emulator Shootout runner. Runs on its own
// emulator cores, so the loaded session is left alone.
void WinShowAcidTestsDialog();

// True when a complete test pack is installed next to the exe (or one or
// two levels up, for a build tree): manifest.txt, the ROMs under tests/,
// and the reference screens under baseline/default/. The pack ships
// separately, so the Tests menu only appears when all three are there;
// nothing else in the emulator depends on it.
bool WinAcidTestsAvailable();

#endif
