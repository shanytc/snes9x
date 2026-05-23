#ifndef CDEBUGGER_SPLITTER_H
#define CDEBUGGER_SPLITTER_H

#include <windows.h>

enum SplitterOrient { SPLIT_HORZ = 0, SPLIT_VERT = 1 };

void SplitterRegisterClass(HINSTANCE hInst);
HWND SplitterCreate(HWND parent, SplitterOrient orient, int id);
void SplitterSetChildren(HWND splitter, HWND a, HWND b);
void SplitterSetRatio(HWND splitter, float ratio);
float SplitterGetRatio(HWND splitter);

#endif
