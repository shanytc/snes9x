#ifndef CDISASM_PANEL_H
#define CDISASM_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void DisasmPanelRegisterClass(HINSTANCE hInst);
HWND DisasmPanelCreate(HWND parent, DbgSystem sys, int id);
void DisasmPanelRefresh(HWND h);
void DisasmPanelInvalidateCache(HWND h);

// Center the view on an arbitrary address (e.g. a call-stack click) and
// select that row. The view holds there while paused; stepping re-follows PC.
void DisasmPanelGotoAddress(HWND h, uint32_t addr);

#endif
