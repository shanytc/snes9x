#ifndef WIN32_AUDIT_TESTS_DLG_H
#define WIN32_AUDIT_TESTS_DLG_H

// Tests > Audit Tests - regression audit of every ROM under Roms/ in every
// boot combination, against baselines under audit/baseline/. See sgb/audit.h.

bool WinAuditTestsAvailable();
void WinShowAuditTestsDialog();
// Modeless: NULL when the dialog is not up. The main message pumps route
// keyboard through IsDialogMessage with it.
HWND WinAuditTestsDialogHwnd();
// Defined in wsnes9x.cpp: the full app load path (the drag-drop one), for
// the dialog's double-click-to-play.
bool WinLoadROMFromDialog(const TCHAR *path);

#endif
