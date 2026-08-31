#ifndef WIN32_AUDIT_TESTS_DLG_H
#define WIN32_AUDIT_TESTS_DLG_H

// Tests > Audit Tests - regression audit of every ROM under Roms/ in every
// boot combination, against baselines under audit/baseline/. See sgb/audit.h.

bool WinAuditTestsAvailable();
void WinShowAuditTestsDialog();

#endif
