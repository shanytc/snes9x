/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_LINK_H_
#define _SGB_GB_LINK_H_

#include <cstdint>
#include <cstddef>

namespace SGB {

// TCP transport for the link cable: BGB 1.4 packet framing only, with the
// command semantics in gb_serial.cpp. Entirely non-blocking.
//
// A direct cable is one peer on channel 0. A DMG-07 hub session keeps the
// listener open and seats up to three peers, one channel per adapter port,
// so the channel index is the player number minus two.

constexpr int kLinkMaxChannels = 3;

// A BGB wire packet: four command bytes plus a 32-bit LE value, usually
// a 2 MiHz timestamp.
struct LinkPacket
{
	uint8_t  b1 = 0;   // command
	uint8_t  b2 = 0;
	uint8_t  b3 = 0;
	uint8_t  b4 = 0;
	uint32_t i1 = 0;   // timestamp / value, little-endian on the wire
};

enum class LinkState : uint8_t
{
	Off        = 0,
	Listening  = 1,   // server socket open, no peer yet
	Connecting = 2,   // client connect() in flight
	Connected  = 3    // at least one peer attached
};

// Open a listening socket on `port`, seating up to `max_peers` (1..3).
// One peer is the direct cable; more is the DMG-07 hub, where a dropped
// peer frees its seat instead of ending the session.
bool LinkStartServer(uint16_t port, int max_peers, char *err, size_t err_cap);

// Start a non-blocking connect (always channel 0); completion shows up as
// LinkPoll going to LinkState::Connected.
bool LinkStartClient(const char *host, uint16_t port, char *err, size_t err_cap);

// Close everything and return to LinkState::Off.
void LinkStop();

// Complete pending accepts/connects, flush the send queues and read what
// has arrived, on every channel. Cheap enough to call at kilohertz rates.
LinkState LinkPoll();

LinkState   LinkGetState();
bool        LinkIsConnected();            // any channel attached
bool        LinkChannelConnected(int ch); // this seat specifically
int         LinkChannelCount();           // attached channels
const char *LinkLastError();

// Close one seat: on a hub it frees the port, on a direct cable it ends
// the session. For refusing a peer at the protocol level.
void LinkCloseChannel(int ch);

// Bumps every time a new peer takes the seat, so a drop-and-refill that
// happens inside one poll cannot inherit the old occupant's handshake.
int LinkChannelGeneration(int ch);

// "host:port" of the current or intended peer, for the status line.
// With several peers it names the first one; the caller counts the rest.
const char *LinkPeerName();

// Queue one packet on a channel; false if that seat is empty. Mark status
// packets droppable so a paused peer's backlog cannot kill the link --
// only undeliverable real transfers mean it is really gone.
bool LinkSend(int ch, const LinkPacket &p, bool droppable = false);

// Discard buffered traffic both ways on every channel. Used on state
// load, where anything still queued belongs to the timeline replaced.
void LinkFlush();

// Pop one fully-received packet from a channel. False when none is there.
bool LinkRecv(int ch, LinkPacket &out);

// Block up to `timeout_ms` for readable data on a channel, giving the
// clocking side a bounded chance to hear back before writing it off.
bool LinkWaitReadable(int ch, int timeout_ms);

} // namespace SGB

#endif
