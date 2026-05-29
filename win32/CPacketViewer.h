#ifndef CPACKET_VIEWER_H
#define CPACKET_VIEWER_H

#include <windows.h>

void PacketViewerGlobalInit(HINSTANCE hInst);
HWND OpenPacketViewer(void);
void PacketViewerRefreshAll(void);

#endif
