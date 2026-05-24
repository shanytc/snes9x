#ifndef CDEBUGGER_DLG_H
#define CDEBUGGER_DLG_H

#include <windows.h>
#include "CDebugger.h"

HWND DebuggerDlgCreate(DbgSystem sys);
void DebuggerDlgClose(HWND h);
void DebuggerDlgRefresh(HWND h);

void DebuggerDlgGlobalInit(HINSTANCE hInst);

HACCEL S9xDebuggerGetAcceleratorsForWindow(HWND wnd);

#endif
