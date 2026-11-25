/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_TVREMOTE_H_
#define _SGB_GB_TVREMOTE_H_

#include <cstddef>
#include <cstdint>

namespace SGB {

struct Memory;

// A household TV remote aimed at a Game Boy Color's infrared port, and the TV it
// belongs to: the remote sends its maker's real frames on a modulated carrier,
// the TV names what the console's own LED sends back.
constexpr int kTvRemoteKeys = 18;   // Power, Mute, 1-12, CH+, CH-, VOL+, VOL-

int         TvRemoteVendors();
const char *TvRemoteVendorName(int vendor);
bool        TvRemoteHasKey(int vendor, int key);
const char *TvRemoteKeyName(int key);

// The console the remote and TV face (nullptr: put away) and the remote's maker.
void TvRemoteFace(const Memory *m, int vendor);
int  TvRemoteVendor();
// A remote button goes down or up now; one button sends at a time.
void TvRemoteKey(int key, bool down);

// gb_infrared: the remote's light as this console's sensor sees it now.
bool TvRemoteSeen(const Memory &m);
// gb_infrared: this console's LED changed; once per frame: a finished signal is decoded.
void TvRemoteLed(const Memory &m, bool on);
void TvRemoteFrame(const Memory &m);

// What the TV last received ("Sony: VOL+"), counted in *serial; false before the first.
bool TvReceived(unsigned *serial, const char **text);
// Called with that text as each command arrives.
void TvSetNotify(void (*fn)(const char *text));

} // namespace SGB

#endif
