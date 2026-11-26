/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_MOBILE_H_
#define _SGB_GB_MOBILE_H_

#include <cstddef>
#include <cstdint>

namespace SGB {

// Nintendo's Mobile Adapter GB (CGB-005, blue PDC model) on player 1's link
// port, with a built-in stand-in for the Mobile System GB servers: no real
// network. The game clocks every byte; the adapter answers on the same one.

void MobileConnect(bool on);
bool MobileConnected();

// `config`: the adapter's 256-byte memory (what Mobile Trainer writes); a
// registered one is made when the file is missing. `root`: the server's
// files - GET /cgb/download?name=<n> reads <root>/download<n>, other GETs
// <root>/www<path>.
void MobileSetPaths(const char *config, const char *root);

// gb_serial, while the adapter holds the port. Exchange: what shifts back for
// a byte the game clocks. Clock: `cycles` real T-cycles passing.
uint8_t MobileExchange(uint8_t in);
uint8_t MobileReply();
void    MobileClock(int32_t cycles);

// Savestates carry the packet in flight and the open connections.
size_t MobileStateBytes();
void   MobileStateSave(uint8_t *out);
void   MobileStateLoad(const uint8_t *in);

// A line per packet each way and per server request, for test drivers.
void MobileSetLog(void (*log)(const char *line));

} // namespace SGB

#endif
