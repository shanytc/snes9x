/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Non-blocking TCP transport for the Game Boy link cable. See gb_link.h
// for the contract; this file is sockets and byte framing only.
//
// Everything is single-threaded and driven from LinkPoll(), which the GB
// serial engine calls from the emulation thread. No locks, no worker
// thread: a link session moves at most a few hundred 8-byte packets per
// second, so polling costs less than the synchronisation would.

// getaddrinfo and friends sit behind feature-test macros that a strict
// -std=c++NN dialect switches off. Ask for them before anything pulls in
// a libc header.
#if !defined(_WIN32)
	#ifndef _POSIX_C_SOURCE
		#define _POSIX_C_SOURCE 200112L
	#endif
	#ifndef _DEFAULT_SOURCE
		#define _DEFAULT_SOURCE 1
	#endif
	#ifndef __BSD_VISIBLE
		#define __BSD_VISIBLE 1
	#endif
#endif

#include "gb_link.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
	// getaddrinfo/getnameinfo are gated behind XP-era _WIN32_WINNT in both
	// the MSVC SDK and MinGW headers, and MinGW may have already picked a
	// lower default — raise it before winsock2.h is pulled in.
	#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0501
		#undef _WIN32_WINNT
	#endif
	#ifndef _WIN32_WINNT
		#define _WIN32_WINNT 0x0601
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	using socket_t = SOCKET;
	#define SGB_INVALID_SOCKET INVALID_SOCKET
	#define SGB_CLOSESOCKET    closesocket
	#define SGB_SOCKET_ERRNO   WSAGetLastError()
	#define SGB_EWOULDBLOCK    WSAEWOULDBLOCK
	#define SGB_EINPROGRESS    WSAEWOULDBLOCK
#else
	#include <arpa/inet.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <unistd.h>
	using socket_t = int;
	#define SGB_INVALID_SOCKET (-1)
	#define SGB_CLOSESOCKET    ::close
	#define SGB_SOCKET_ERRNO   errno
	#define SGB_EWOULDBLOCK    EWOULDBLOCK
	#define SGB_EINPROGRESS    EINPROGRESS
#endif

namespace SGB {

namespace {

constexpr size_t kPacketBytes = 8;
constexpr size_t kRxCap       = 4096;   // ~500 packets; a stalled peer can't grow it
constexpr size_t kTxCap       = 4096;

struct LinkCtx
{
	socket_t  listen_fd = SGB_INVALID_SOCKET;
	socket_t  peer_fd   = SGB_INVALID_SOCKET;
	LinkState state     = LinkState::Off;

	uint8_t rx[kRxCap];
	size_t  rx_len = 0;
	size_t  rx_out = 0;   // read cursor; packets are consumed from here

	uint8_t tx[kTxCap];
	size_t  tx_len = 0;

	char err[192]  = {0};
	char peer[160] = {0};
};

LinkCtx g_link;

#ifdef _WIN32
bool g_wsa_started = false;

bool EnsureWinsock(char *err, size_t err_cap)
{
	if (g_wsa_started) return true;
	WSADATA data;
	const int rc = WSAStartup(MAKEWORD(2, 2), &data);
	if (rc != 0)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "WSAStartup failed (%d)", rc);
		return false;
	}
	g_wsa_started = true;
	return true;
}
#else
bool EnsureWinsock(char *, size_t) { return true; }
#endif

void SetError(const char *fmt, int code)
{
	std::snprintf(g_link.err, sizeof g_link.err, fmt, code);
}

void SetNonBlocking(socket_t fd)
{
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(fd, FIONBIO, &nb);
#else
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Latency, not throughput: an 8-byte sync packet held back by Nagle would
// add up to 40 ms to every byte the linked games exchange.
void SetNoDelay(socket_t fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
	           reinterpret_cast<const char *>(&one), sizeof one);
}

void CloseSocket(socket_t &fd)
{
	if (fd != SGB_INVALID_SOCKET)
	{
		SGB_CLOSESOCKET(fd);
		fd = SGB_INVALID_SOCKET;
	}
}

// Close a live connection without parking it in TIME_WAIT. On the server
// side the accepted socket owns the well-known port, and a lingering one
// makes the next bind fail for minutes under SO_EXCLUSIVEADDRUSE — which
// is exactly the unplug-then-plug-back-in path. Nothing in the protocol
// needs a graceful flush at close, so a reset is the right trade.
void CloseConnection(socket_t &fd)
{
	if (fd == SGB_INVALID_SOCKET) return;

	linger lg;
	lg.l_onoff  = 1;
	lg.l_linger = 0;
	setsockopt(fd, SOL_SOCKET, SO_LINGER,
	           reinterpret_cast<const char *>(&lg), sizeof lg);

	SGB_CLOSESOCKET(fd);
	fd = SGB_INVALID_SOCKET;
}

// Peer went away. The server owns the session and keeps hosting: it drops
// back to Listening so the client can unplug and plug back in on its own,
// with one click and no second window. A client has no listening socket to
// fall back to, so it goes Off and its tick clears.
//
// Tearing the listener down here as well would mean re-establishing always
// cost a click in each window, which is the wrong trade for the side that
// did not ask to disconnect.
void DropPeer(const char *reason)
{
	CloseConnection(g_link.peer_fd);
	g_link.rx_len = g_link.rx_out = 0;
	g_link.tx_len = 0;
	if (reason && *reason)
		std::snprintf(g_link.err, sizeof g_link.err, "%s", reason);

	g_link.state = (g_link.listen_fd != SGB_INVALID_SOCKET)
	                   ? LinkState::Listening
	                   : LinkState::Off;
}

// Compact the receive buffer so a long session doesn't walk rx_out off
// the end. Called once the read cursor has consumed everything buffered.
void CompactRx()
{
	if (g_link.rx_out == 0) return;
	if (g_link.rx_out >= g_link.rx_len)
	{
		g_link.rx_len = g_link.rx_out = 0;
		return;
	}
	const size_t left = g_link.rx_len - g_link.rx_out;
	std::memmove(g_link.rx, g_link.rx + g_link.rx_out, left);
	g_link.rx_len = left;
	g_link.rx_out = 0;
}

void FlushTx()
{
	while (g_link.tx_len > 0 && g_link.peer_fd != SGB_INVALID_SOCKET)
	{
		const int sent = static_cast<int>(
			send(g_link.peer_fd, reinterpret_cast<const char *>(g_link.tx),
			     static_cast<int>(g_link.tx_len), 0));
		if (sent > 0)
		{
			const size_t n = static_cast<size_t>(sent);
			if (n >= g_link.tx_len) { g_link.tx_len = 0; return; }
			std::memmove(g_link.tx, g_link.tx + n, g_link.tx_len - n);
			g_link.tx_len -= n;
			continue;
		}

		const int e = SGB_SOCKET_ERRNO;
		if (sent < 0 && e == SGB_EWOULDBLOCK) return;   // retry next poll
		DropPeer("Link closed by peer (send failed)");
		return;
	}
}

void PumpRx()
{
	while (g_link.peer_fd != SGB_INVALID_SOCKET)
	{
		CompactRx();
		if (g_link.rx_len >= kRxCap) return;   // consumer is behind; stop reading

		const int got = static_cast<int>(
			recv(g_link.peer_fd, reinterpret_cast<char *>(g_link.rx + g_link.rx_len),
			     static_cast<int>(kRxCap - g_link.rx_len), 0));
		if (got > 0) { g_link.rx_len += static_cast<size_t>(got); continue; }
		if (got == 0) { DropPeer("Link closed by peer"); return; }

		if (SGB_SOCKET_ERRNO == SGB_EWOULDBLOCK) return;
		DropPeer("Link closed by peer (recv failed)");
		return;
	}
}

// A Game Boy has one link port. Anything that dials in while a peer is
// already attached gets accepted and closed at once, so it sees an honest
// refusal instead of sitting unnoticed in the backlog looking connected.
void RejectExtraPeers()
{
	if (g_link.listen_fd == SGB_INVALID_SOCKET) return;
	for (;;)
	{
		const socket_t fd = accept(g_link.listen_fd, nullptr, nullptr);
		if (fd == SGB_INVALID_SOCKET) return;
		SGB_CLOSESOCKET(fd);
	}
}

void AdoptPeer(socket_t fd)
{
	SetNonBlocking(fd);
	SetNoDelay(fd);
	g_link.peer_fd = fd;
	g_link.rx_len = g_link.rx_out = 0;
	g_link.tx_len = 0;
	g_link.state  = LinkState::Connected;
	g_link.err[0] = '\0';
}

void PollAccept()
{
	sockaddr_storage from{};
#ifdef _WIN32
	int from_len = static_cast<int>(sizeof from);
#else
	socklen_t from_len = sizeof from;
#endif
	const socket_t fd = accept(g_link.listen_fd,
	                           reinterpret_cast<sockaddr *>(&from), &from_len);
	if (fd == SGB_INVALID_SOCKET) return;

	char host[NI_MAXHOST] = {0};
	char serv[NI_MAXSERV] = {0};
	if (getnameinfo(reinterpret_cast<sockaddr *>(&from), from_len,
	                host, sizeof host, serv, sizeof serv,
	                NI_NUMERICHOST | NI_NUMERICSERV) == 0)
		std::snprintf(g_link.peer, sizeof g_link.peer, "%s:%s", host, serv);

	AdoptPeer(fd);
}

// Non-blocking connect finishes asynchronously: the socket becomes
// writable on success and (on POSIX) also on failure, so the pending
// SO_ERROR is what actually distinguishes the two.
void PollConnect()
{
	fd_set wr, ex;
	FD_ZERO(&wr);
	FD_ZERO(&ex);
	FD_SET(g_link.peer_fd, &wr);
	FD_SET(g_link.peer_fd, &ex);

	timeval tv{0, 0};
	const int rc = select(static_cast<int>(g_link.peer_fd) + 1, nullptr, &wr, &ex, &tv);
	if (rc <= 0) return;

	int       so_err = 0;
#ifdef _WIN32
	int       len    = static_cast<int>(sizeof so_err);
#else
	socklen_t len    = sizeof so_err;
#endif
	getsockopt(g_link.peer_fd, SOL_SOCKET, SO_ERROR,
	           reinterpret_cast<char *>(&so_err), &len);

	if (FD_ISSET(g_link.peer_fd, &ex) || so_err != 0)
	{
		CloseSocket(g_link.peer_fd);
		g_link.state = LinkState::Off;
		SetError("Connect failed (%d)", so_err ? so_err : SGB_SOCKET_ERRNO);
		return;
	}

	SetNoDelay(g_link.peer_fd);
	g_link.state  = LinkState::Connected;
	g_link.err[0] = '\0';
}

} // anonymous

bool LinkStartServer(uint16_t port, char *err, size_t err_cap)
{
	LinkStop();
	if (!EnsureWinsock(err, err_cap)) return false;

	const socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == SGB_INVALID_SOCKET)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "socket() failed (%d)", SGB_SOCKET_ERRNO);
		return false;
	}

	// Binding is how the auto-sense picks a role, so it must fail cleanly
	// when the other instance already holds the port. Windows SO_REUSEADDR
	// would let a second socket bind the same port and silently steal it,
	// so ask for the opposite there; on POSIX SO_REUSEADDR only recycles
	// TIME_WAIT and still rejects a live listener, which is what we want.
	int one = 1;
#ifdef _WIN32
	setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
	           reinterpret_cast<const char *>(&one), sizeof one);
#else
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
	           reinterpret_cast<const char *>(&one), sizeof one);
#endif

	// Loopback only: the link is same-PC by design, and not listening on
	// every interface means no firewall prompt and nothing exposed to the
	// network.
	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = htons(port);

	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0)
	{
		if (err && err_cap)
			std::snprintf(err, err_cap, "Port %u is already in use (%d)",
			              static_cast<unsigned>(port), SGB_SOCKET_ERRNO);
		SGB_CLOSESOCKET(fd);
		return false;
	}

	if (listen(fd, 1) != 0)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "listen() failed (%d)", SGB_SOCKET_ERRNO);
		SGB_CLOSESOCKET(fd);
		return false;
	}

	SetNonBlocking(fd);
	g_link.listen_fd = fd;
	g_link.state     = LinkState::Listening;
	g_link.err[0]    = '\0';
	std::snprintf(g_link.peer, sizeof g_link.peer, "listening on port %u",
	              static_cast<unsigned>(port));
	return true;
}

bool LinkStartClient(const char *host, uint16_t port, char *err, size_t err_cap)
{
	LinkStop();
	if (!EnsureWinsock(err, err_cap)) return false;
	if (!host || !*host) host = "127.0.0.1";

	char port_text[16];
	std::snprintf(port_text, sizeof port_text, "%u", static_cast<unsigned>(port));

	addrinfo hints{};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo *list = nullptr;
	if (getaddrinfo(host, port_text, &hints, &list) != 0 || !list)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "Cannot resolve host \"%s\"", host);
		return false;
	}

	socket_t fd = SGB_INVALID_SOCKET;
	for (addrinfo *ai = list; ai; ai = ai->ai_next)
	{
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == SGB_INVALID_SOCKET) continue;

		SetNonBlocking(fd);
		const int rc = connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
		if (rc == 0)
		{
			SetNoDelay(fd);
			g_link.peer_fd = fd;
			g_link.state   = LinkState::Connected;
			break;
		}
		if (SGB_SOCKET_ERRNO == SGB_EINPROGRESS)
		{
			g_link.peer_fd = fd;
			g_link.state   = LinkState::Connecting;
			break;
		}
		SGB_CLOSESOCKET(fd);
		fd = SGB_INVALID_SOCKET;
	}
	freeaddrinfo(list);

	if (fd == SGB_INVALID_SOCKET)
	{
		if (err && err_cap)
			std::snprintf(err, err_cap, "Cannot connect to %s:%u", host,
			              static_cast<unsigned>(port));
		g_link.state = LinkState::Off;
		return false;
	}

	g_link.err[0] = '\0';
	std::snprintf(g_link.peer, sizeof g_link.peer, "%s:%u", host,
	              static_cast<unsigned>(port));
	return true;
}

void LinkStop()
{
	CloseConnection(g_link.peer_fd);
	CloseSocket(g_link.listen_fd);
	g_link.rx_len = g_link.rx_out = 0;
	g_link.tx_len = 0;
	g_link.state  = LinkState::Off;
	g_link.peer[0] = '\0';
}

LinkState LinkPoll()
{
	switch (g_link.state)
	{
		case LinkState::Off:
			break;

		case LinkState::Listening:
			PollAccept();
			break;

		case LinkState::Connecting:
			PollConnect();
			break;

		case LinkState::Connected:
			// Read first. A peer that dropped and immediately re-dialled
			// would otherwise be turned away by RejectExtraPeers as a third
			// instance, because we had not yet noticed the old connection
			// was gone — the reconnect died before we knew we were free.
			FlushTx();
			PumpRx();
			if (g_link.state == LinkState::Connected) RejectExtraPeers();
			break;
	}
	return g_link.state;
}

LinkState   LinkGetState()    { return g_link.state; }
bool        LinkIsConnected() { return g_link.state == LinkState::Connected; }
const char *LinkLastError()   { return g_link.err; }
const char *LinkPeerName()    { return g_link.peer; }

bool LinkSend(const LinkPacket &p)
{
	if (g_link.state != LinkState::Connected) return false;

	if (g_link.tx_len + kPacketBytes > kTxCap)
	{
		// The peer has stopped draining for long enough to fill the queue;
		// treating that as a dead link beats silently dropping transfers.
		DropPeer("Link stalled (send queue full)");
		return false;
	}

	uint8_t *w = g_link.tx + g_link.tx_len;
	w[0] = p.b1;
	w[1] = p.b2;
	w[2] = p.b3;
	w[3] = p.b4;
	w[4] = static_cast<uint8_t>(p.i1 & 0xFF);
	w[5] = static_cast<uint8_t>((p.i1 >> 8) & 0xFF);
	w[6] = static_cast<uint8_t>((p.i1 >> 16) & 0xFF);
	w[7] = static_cast<uint8_t>((p.i1 >> 24) & 0xFF);
	g_link.tx_len += kPacketBytes;

	FlushTx();
	return true;
}

bool LinkRecv(LinkPacket &out)
{
	if (g_link.rx_len - g_link.rx_out < kPacketBytes) return false;

	const uint8_t *r = g_link.rx + g_link.rx_out;
	out.b1 = r[0];
	out.b2 = r[1];
	out.b3 = r[2];
	out.b4 = r[3];
	out.i1 = static_cast<uint32_t>(r[4]) |
	         (static_cast<uint32_t>(r[5]) << 8) |
	         (static_cast<uint32_t>(r[6]) << 16) |
	         (static_cast<uint32_t>(r[7]) << 24);
	g_link.rx_out += kPacketBytes;
	return true;
}

bool LinkWaitReadable(int timeout_ms)
{
	if (g_link.state != LinkState::Connected) return false;
	if (g_link.rx_len - g_link.rx_out >= kPacketBytes) return true;

	fd_set rd;
	FD_ZERO(&rd);
	FD_SET(g_link.peer_fd, &rd);

	timeval tv;
	tv.tv_sec  = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	const int rc = select(static_cast<int>(g_link.peer_fd) + 1, &rd, nullptr, nullptr, &tv);
	if (rc <= 0) return false;

	PumpRx();
	return g_link.rx_len - g_link.rx_out >= kPacketBytes;
}

} // namespace SGB
