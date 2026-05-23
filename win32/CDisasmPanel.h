#ifndef CDISASM_PANEL_H
#define CDISASM_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void DisasmPanelRegisterClass(HINSTANCE hInst);
HWND DisasmPanelCreate(HWND parent, DbgSystem sys, int id);
void DisasmPanelRefresh(HWND h);

#endif
