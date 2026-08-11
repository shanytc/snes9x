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
	Connected  = 3
};

// Open a listening socket on `port`; false and `err` filled on failure.
bool LinkStartServer(uint16_t port, char *err, size_t err_cap);

// Start a non-blocking connect; completion shows up as LinkPoll going
// to LinkState::Connected.
bool LinkStartClient(const char *host, uint16_t port, char *err, size_t err_cap);

// Close everything and return to LinkState::Off.
void LinkStop();

// Complete a pending accept/connect, flush the send queue and read what
// has arrived. Cheap enough to call at kilohertz rates.
LinkState LinkPoll();

LinkState   LinkGetState();
bool        LinkIsConnected();
const char *LinkLastError();

// "host:port" of the current or intended peer, for the status line.
const char *LinkPeerName();

// Queue one packet; false if not connected or the peer is gone. Mark
// status packets droppable so a paused peer's backlog cannot kill the
// link -- only undeliverable real transfers mean it is really gone.
bool LinkSend(const LinkPacket &p, bool droppable = false);

// Discard buffered traffic both ways. Used on state load, where
// anything still queued belongs to the timeline being replaced.
void LinkFlush();

// Pop one fully-received packet. Returns false when none is buffered.
bool LinkRecv(LinkPacket &out);

// Block up to `timeout_ms` for readable data, giving the master side a
// bounded chance to hear back before writing the byte off.
bool LinkWaitReadable(int timeout_ms);

} // namespace SGB

#endif
