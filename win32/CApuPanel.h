#ifndef CAPU_PANEL_H
#define CAPU_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void ApuPanelRegisterClass(HINSTANCE hInst);
HWND ApuPanelCreate(HWND parent, DbgSystem sys, int id);
void ApuPanelRefresh(HWND h);

#endif
