/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// XBAND local server (Netplay > XBand > Start Server): the xbserver.retrocomputing.network
// link protocol (ID line, then ADSP) plus the two-window switchboard, as the harness's xbserver.py.

#ifndef _XBANDSERVER_H_
#define _XBANDSERVER_H_

#include <string>

// Listens on every interface; dir holds patches/ and server.log. False with why on failure.
bool XBandServerStart(int port, const std::string &dir, std::string &why);
void XBandServerStop(void);
bool XBandServerRunning(void);
int  XBandServerPort(void);
int  XBandServerPatchCount(const std::string &dir);

#endif
