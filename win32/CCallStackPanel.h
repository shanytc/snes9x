#ifndef CCALLSTACK_PANEL_H
#define CCALLSTACK_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void CallStackPanelRegisterClass(HINSTANCE hInst);
HWND CallStackPanelCreate(HWND parent, DbgSystem sys, int id);
void CallStackPanelRefresh(HWND h);

#endif
