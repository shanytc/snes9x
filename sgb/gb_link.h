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

// TCP transport for the Game Boy link cable. Speaks the BGB 1.4 link
// protocol framing (https://bgb.bircd.org/bgblink.html) so a session can
// pair with BGB, SameBoy, Emulicious or another Snes9x instance. This
// header is the socket layer only — the command semantics (version
// handshake, sync1/sync2/sync3 transfer exchange) live in gb_serial.cpp.
//
// One side listens, the other connects; localhost and remote hosts are
// the same code path. Everything here is non-blocking: LinkPoll() drives
// accept/connect completion and LinkRecv() returns whole packets as they
// arrive, so the emulation thread never stalls on the network.

// A BGB wire packet. Always 8 bytes: four command/parameter bytes plus a
// 32-bit little-endian value that is usually a 2 MiHz timestamp.
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

// Open a listening socket on `port` (all interfaces). Returns false and
// fills `err` on failure. A server keeps listening after a peer drops, so
// the other side can reconnect without touching the UI.
bool LinkStartServer(uint16_t port, char *err, size_t err_cap);

// Resolve `host` and start a non-blocking connect to `host:port`. Returns
// true once the connect is under way — completion is reported by LinkPoll
// switching to LinkState::Connected.
bool LinkStartClient(const char *host, uint16_t port, char *err, size_t err_cap);

// Close everything and return to LinkState::Off.
void LinkStop();

// Service the socket: complete a pending accept or connect, flush queued
// outgoing bytes, and read incoming bytes into the packet buffer. Cheap
// enough to call at kilohertz rates; returns the state after servicing.
LinkState LinkPoll();

LinkState   LinkGetState();
bool        LinkIsConnected();
const char *LinkLastError();

// "host:port" of the current or intended peer, for the status line.
const char *LinkPeerName();

// Queue one packet. Returns false if the link is not connected or the
// peer is gone (which also tears the connection down).
bool LinkSend(const LinkPacket &p);

// Pop one fully-received packet. Returns false when none is buffered.
bool LinkRecv(LinkPacket &out);

// Block up to `timeout_ms` waiting for readable data, then poll. Used by
// the master side of a transfer to give the peer a bounded chance to
// answer before the byte is written off as "no partner".
bool LinkWaitReadable(int timeout_ms);

} // namespace SGB

#endif
