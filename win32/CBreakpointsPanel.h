#ifndef CBREAKPOINTS_PANEL_H
#define CBREAKPOINTS_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void BreakpointsPanelRegisterClass(HINSTANCE hInst);
HWND BreakpointsPanelCreate(HWND parent, DbgSystem sys, int id);
void BreakpointsPanelRefresh(HWND h);

bool ShowEditBreakpointDialog(HWND parent, DbgSystem sys, DbgBreakpoint *bp);

#endif
