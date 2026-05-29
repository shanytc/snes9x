#ifndef CWATCH_PANEL_H
#define CWATCH_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void WatchPanelRegisterClass(HINSTANCE hInst);
HWND WatchPanelCreate(HWND parent, DbgSystem sys, int id);
void WatchPanelRefresh(HWND h);

#endif
