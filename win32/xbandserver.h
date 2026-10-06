/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// XBAND local server (Netplay > XBand > Start Server): the xbserver.retrocomputing.network
// link protocol (ID line, then ADSP) plus the two-window switchboard, as the harness's xbserver.py.

#ifndef _XBANDSERVER_H_
#define _XBANDSERVER_H_

#include <cstddef>
#include <cstdint>
#include <string>

// Listens on every interface; dir holds patches/ and server.log. False with why on failure.
// netlink: matches go peer to peer (TCP 65433 / UDP 20001) instead of through the switchboard.
bool XBandServerStart(int port, const std::string &dir, bool netlink, std::string &why);
void XBandServerStop(void);
bool XBandServerRunning(void);
// A Netlink caller (opens with RESET) on the server's port: true takes the socket for this PC's box.
typedef bool (*XBandPeerHandler)(intptr_t sock, const char *ip, const uint8_t *data, size_t len);
void XBandServerSetPeerHandler(XBandPeerHandler handler);
int  XBandServerPort(void);
int  XBandServerPatchCount(const std::string &dir);

#endif
