/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_KISSLINK_H_
#define _SGB_GB_KISSLINK_H_

#include <cstddef>
#include <cstdint>

namespace SGB {

struct Memory;

// Hudson's GB KISS LINK (HC-749): a PC's infrared modem that faces a HuC1/HuC3
// cart. It always leads; the cart's own GB KISS code answers its commands.
enum class KissJob : uint8_t
{
	None,
	SendFile,     // a .gbf into the GB KISS menu's receive screen
	SendMap,      // a Nectaris GB map record ("receive from PC")
	ReceiveMap,   // a Nectaris GB map record back out ("send to PC")
};

enum class KissStatus : uint8_t
{
	Idle,
	Waiting,      // flashing its handshake until the game answers
	Busy,
	Done,
	Failed,
};

struct KissProgress
{
	KissJob     job;
	KissStatus  status;
	uint32_t    serial;     // which job (one per Start)
	int         percent;
	const char *error;      // why it failed (static text)
};

// A Nectaris GB map record: what the cart keeps per map and sends whole.
constexpr size_t kNectarisMapBytes = 512;

// Start a job against this cart. SendFile takes a .gbf, SendMap a map record.
bool KissLinkStart(Memory &m, KissJob job, const uint8_t *data, size_t size, const char **error);
void KissLinkCancel();
KissProgress KissLinkProgress();
// The map a ReceiveMap job brought back.
bool KissLinkReceivedMap(uint8_t out[kNectarisMapBytes]);

// Hudson's PC maps (.NMD, "NEC MAP V1.01") to the cart's record: the map ID is
// the built-in map whose terrain it is, read from this cart's ROM.
bool KissLinkNmdToMap(const Memory &m, const uint8_t *nmd, size_t size,
                      uint8_t out[kNectarisMapBytes], const char **error);
bool KissLinkIsMapRecord(const uint8_t *data, size_t size);
bool KissLinkIsNectaris(const Memory &m);

// gb_infrared: the LINK faces this cart while a job runs.
bool KissLinkFacing(const Memory &m);
bool KissLinkLit(const Memory &m);              // its LED as the cart's sensor sees it now
void KissLinkCartLed(const Memory &m, bool on);
// Once per frame: timeouts and progress while the cart is not reading.
void KissLinkFrame(const Memory &m);

// Savestates carry the job's progress (the job's input stays with the LINK).
size_t KissLinkStateBytes();
void   KissLinkStateSave(const Memory &m, uint8_t *out);
void   KissLinkStateLoad(Memory &m, const uint8_t *in);

} // namespace SGB

#endif
