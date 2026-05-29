#ifndef CMAPPER_PANEL_H
#define CMAPPER_PANEL_H

#include <windows.h>
#include "CDebugger.h"

void MapperPanelRegisterClass(HINSTANCE hInst);
HWND MapperPanelCreate(HWND parent, DbgSystem sys, int id);
void MapperPanelRefresh(HWND h);

#endif
