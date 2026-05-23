#ifndef CSTATUS_PANEL_H
#define CSTATUS_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void StatusPanelRegisterClass(HINSTANCE hInst);
HWND StatusPanelCreate(HWND parent, DbgSystem sys, int id);
void StatusPanelRefresh(HWND h);

#endif
