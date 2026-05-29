#ifndef CDEBUGGER_DLG_H
#define CDEBUGGER_DLG_H

#include <windows.h>
#include "CDebugger.h"

HWND DebuggerDlgCreate(DbgSystem sys);
void DebuggerDlgClose(HWND h);
void DebuggerDlgRefresh(HWND h);
void DebuggerDlgInvalidateCache(HWND h);

void DebuggerDlgGlobalInit(HINSTANCE hInst);

HACCEL S9xDebuggerGetAcceleratorsForWindow(HWND wnd);

// Modal hex-input prompt (reuses the IDD_DBG_INPUT dialog). Returns true and
// writes *out_value when the user accepts a parseable hex value (accepts an
// optional `$` or `0x` prefix). Shared so other debugger windows (e.g. the
// memory viewer's "Go to Address") can prompt without duplicating the dialog.
bool DebuggerPromptHex(HWND parent, const TCHAR *title, const TCHAR *prompt,
                       const TCHAR *initial, uint32_t *out_value);

#endif
