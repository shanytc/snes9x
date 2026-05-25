#ifndef CMEMORY_VIEWER_H
#define CMEMORY_VIEWER_H

#include <windows.h>
#include "CDebugger.h"

void MemoryViewerGlobalInit(HINSTANCE hInst);
HWND OpenMemoryViewer(DbgSystem sys);
void MemoryViewerRefreshAll();

#endif
