/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Non-blocking TCP transport for the Game Boy link cable — sockets and
// byte framing only. Single-threaded, driven entirely from LinkPoll().
// One peer channel is the direct cable; up to three carry a DMG-07 hub.

// getaddrinfo sits behind feature-test macros a strict -std=c++NN turns
// off; ask for them before any libc header is pulled in.
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
	// getaddrinfo is gated behind XP-era _WIN32_WINNT; MinGW may default
	// lower, so raise it before winsock2.h.
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

struct PeerCtx
{
	socket_t fd = SGB_INVALID_SOCKET;

	uint8_t rx[kRxCap];
	size_t  rx_len = 0;
	size_t  rx_out = 0;   // read cursor; packets are consumed from here

	uint8_t tx[kTxCap];
	size_t  tx_len = 0;
};

struct LinkCtx
{
	socket_t  listen_fd = SGB_INVALID_SOCKET;
	LinkState state     = LinkState::Off;
	int       max_peers = 1;
	bool      client    = false;

	PeerCtx peers[kLinkMaxChannels];
	int     gen[kLinkMaxChannels] = {0};

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

// Close without parking the well-known port in TIME_WAIT, which would
// make the next bind fail for minutes under SO_EXCLUSIVEADDRUSE.
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

int CountPeers()
{
	int n = 0;
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		if (g_link.peers[ch].fd != SGB_INVALID_SOCKET) ++n;
	return n;
}

void ResetChannel(PeerCtx &p)
{
	p.rx_len = p.rx_out = 0;
	p.tx_len = 0;
}

// Losing the only peer of a direct cable ends the session, listener and
// all, as it always has. On a hub the seat is merely freed: the adapter
// keeps pinging, the player's status bit clears, and a new instance can
// dial into the empty port.
void DropChannel(int ch, const char *reason)
{
	PeerCtx &p = g_link.peers[ch];
	CloseConnection(p.fd);
	ResetChannel(p);

	if (reason && *reason)
		std::snprintf(g_link.err, sizeof g_link.err, "%s", reason);

	if (g_link.client || g_link.max_peers <= 1)
	{
		CloseSocket(g_link.listen_fd);
		g_link.state = LinkState::Off;
		return;
	}

	g_link.state = CountPeers() ? LinkState::Connected : LinkState::Listening;
}

// Compact a receive buffer so a long session doesn't walk rx_out off the
// end. Called once the read cursor has consumed everything buffered.
void CompactRx(PeerCtx &p)
{
	if (p.rx_out == 0) return;
	if (p.rx_out >= p.rx_len)
	{
		p.rx_len = p.rx_out = 0;
		return;
	}
	const size_t left = p.rx_len - p.rx_out;
	std::memmove(p.rx, p.rx + p.rx_out, left);
	p.rx_len = left;
	p.rx_out = 0;
}

void FlushTx(int ch)
{
	PeerCtx &p = g_link.peers[ch];
	while (p.tx_len > 0 && p.fd != SGB_INVALID_SOCKET)
	{
		const int sent = static_cast<int>(
			send(p.fd, reinterpret_cast<const char *>(p.tx),
			     static_cast<int>(p.tx_len), 0));
		if (sent > 0)
		{
			const size_t n = static_cast<size_t>(sent);
			if (n >= p.tx_len) { p.tx_len = 0; return; }
			std::memmove(p.tx, p.tx + n, p.tx_len - n);
			p.tx_len -= n;
			continue;
		}

		const int e = SGB_SOCKET_ERRNO;
		if (sent < 0 && e == SGB_EWOULDBLOCK) return;   // retry next poll
		DropChannel(ch, "Link closed by peer");
		return;
	}
}

void PumpRx(int ch)
{
	PeerCtx &p = g_link.peers[ch];
	while (p.fd != SGB_INVALID_SOCKET)
	{
		CompactRx(p);
		if (p.rx_len >= kRxCap) return;   // consumer is behind; stop reading

		const int got = static_cast<int>(
			recv(p.fd, reinterpret_cast<char *>(p.rx + p.rx_len),
			     static_cast<int>(kRxCap - p.rx_len), 0));
		if (got > 0) { p.rx_len += static_cast<size_t>(got); continue; }
		if (got == 0) { DropChannel(ch, "Link closed by peer"); return; }

		if (SGB_SOCKET_ERRNO == SGB_EWOULDBLOCK) return;
		DropChannel(ch, "Link closed by peer");
		return;
	}
}

void AdoptPeer(int ch, socket_t fd)
{
	SetNonBlocking(fd);
	SetNoDelay(fd);
	PeerCtx &p = g_link.peers[ch];
	p.fd = fd;
	ResetChannel(p);
	++g_link.gen[ch];
	g_link.state  = LinkState::Connected;
	g_link.err[0] = '\0';
}

// Seat arrivals in the lowest free channel; on a hub that seat is the
// player number. Anything dialling in with every seat taken is accepted
// and closed, so it gets an honest refusal.
void PollAccept()
{
	if (g_link.listen_fd == SGB_INVALID_SOCKET) return;
	for (;;)
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

		int seat = -1;
		for (int ch = 0; ch < g_link.max_peers && ch < kLinkMaxChannels; ++ch)
			if (g_link.peers[ch].fd == SGB_INVALID_SOCKET) { seat = ch; break; }

		if (seat < 0)
		{
			SGB_CLOSESOCKET(fd);
			continue;
		}

		if (seat == 0)
		{
			char host[NI_MAXHOST] = {0};
			char serv[NI_MAXSERV] = {0};
			if (getnameinfo(reinterpret_cast<sockaddr *>(&from), from_len,
			                host, sizeof host, serv, sizeof serv,
			                NI_NUMERICHOST | NI_NUMERICSERV) == 0)
				std::snprintf(g_link.peer, sizeof g_link.peer, "%s:%s", host, serv);
		}

		AdoptPeer(seat, fd);
	}
}

// A non-blocking connect goes writable on both success and failure, so
// the pending SO_ERROR is what distinguishes them.
void PollConnect()
{
	PeerCtx &p = g_link.peers[0];

	fd_set wr, ex;
	FD_ZERO(&wr);
	FD_ZERO(&ex);
	FD_SET(p.fd, &wr);
	FD_SET(p.fd, &ex);

	timeval tv{0, 0};
	const int rc = select(static_cast<int>(p.fd) + 1, nullptr, &wr, &ex, &tv);
	if (rc <= 0) return;

	int       so_err = 0;
#ifdef _WIN32
	int       len    = static_cast<int>(sizeof so_err);
#else
	socklen_t len    = sizeof so_err;
#endif
	getsockopt(p.fd, SOL_SOCKET, SO_ERROR,
	           reinterpret_cast<char *>(&so_err), &len);

	if (FD_ISSET(p.fd, &ex) || so_err != 0)
	{
		CloseSocket(p.fd);
		g_link.state = LinkState::Off;
		SetError("Connect failed (%d)", so_err ? so_err : SGB_SOCKET_ERRNO);
		return;
	}

	SetNoDelay(p.fd);
	++g_link.gen[0];
	g_link.state  = LinkState::Connected;
	g_link.err[0] = '\0';
}

} // anonymous

bool LinkStartServer(uint16_t port, int max_peers, char *err, size_t err_cap)
{
	LinkStop();
	if (!EnsureWinsock(err, err_cap)) return false;

	const socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == SGB_INVALID_SOCKET)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "socket() failed (%d)", SGB_SOCKET_ERRNO);
		return false;
	}

	// Bind is the role probe, so it must fail when the peer holds the port.
	// Windows SO_REUSEADDR would allow a second bind, so invert it there.
	int one = 1;
#ifdef _WIN32
	setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
	           reinterpret_cast<const char *>(&one), sizeof one);
#else
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
	           reinterpret_cast<const char *>(&one), sizeof one);
#endif

	// Loopback only: same-PC by design, so nothing is exposed and Windows
	// raises no firewall prompt.
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

	if (listen(fd, kLinkMaxChannels) != 0)
	{
		if (err && err_cap) std::snprintf(err, err_cap, "listen() failed (%d)", SGB_SOCKET_ERRNO);
		SGB_CLOSESOCKET(fd);
		return false;
	}

	SetNonBlocking(fd);
	g_link.listen_fd = fd;
	g_link.state     = LinkState::Listening;
	g_link.client    = false;
	g_link.max_peers = max_peers < 1 ? 1
	                 : max_peers > kLinkMaxChannels ? kLinkMaxChannels : max_peers;
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

	g_link.client    = true;
	g_link.max_peers = 1;

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
			g_link.peers[0].fd = fd;
			++g_link.gen[0];
			g_link.state       = LinkState::Connected;
			break;
		}
		if (SGB_SOCKET_ERRNO == SGB_EINPROGRESS)
		{
			g_link.peers[0].fd = fd;
			g_link.state       = LinkState::Connecting;
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
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
	{
		CloseConnection(g_link.peers[ch].fd);
		ResetChannel(g_link.peers[ch]);
	}
	CloseSocket(g_link.listen_fd);
	g_link.state     = LinkState::Off;
	g_link.client    = false;
	g_link.max_peers = 1;
	g_link.peer[0]   = '\0';
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
			// Read first: a peer that dropped and re-dialled would otherwise
			// be refused for the seat it still appeared to hold.
			for (int ch = 0; ch < kLinkMaxChannels; ++ch)
			{
				if (g_link.peers[ch].fd == SGB_INVALID_SOCKET) continue;
				FlushTx(ch);
				if (g_link.peers[ch].fd != SGB_INVALID_SOCKET) PumpRx(ch);
			}
			if (g_link.state == LinkState::Connected ||
			    g_link.state == LinkState::Listening)
				PollAccept();
			break;
	}
	return g_link.state;
}

LinkState LinkGetState()    { return g_link.state; }
bool      LinkIsConnected() { return g_link.state == LinkState::Connected; }

bool LinkChannelConnected(int ch)
{
	if (ch < 0 || ch >= kLinkMaxChannels) return false;
	return g_link.state == LinkState::Connected &&
	       g_link.peers[ch].fd != SGB_INVALID_SOCKET;
}

int LinkChannelCount()
{
	return g_link.state == LinkState::Connected ? CountPeers() : 0;
}

const char *LinkLastError() { return g_link.err; }
const char *LinkPeerName()  { return g_link.peer; }

void LinkCloseChannel(int ch)
{
	if (ch < 0 || ch >= kLinkMaxChannels) return;
	if (g_link.peers[ch].fd == SGB_INVALID_SOCKET) return;
	DropChannel(ch, nullptr);
}

int LinkChannelGeneration(int ch)
{
	if (ch < 0 || ch >= kLinkMaxChannels) return 0;
	return g_link.gen[ch];
}

bool LinkSend(int ch, const LinkPacket &p, bool droppable)
{
	if (!LinkChannelConnected(ch)) return false;
	PeerCtx &peer = g_link.peers[ch];

	if (peer.tx_len + kPacketBytes > kTxCap)
	{
		// A peer that is merely paused -- loading a state, sitting in a menu
		// -- stops draining for a while. Status packets are dropped rather
		// than killing a link that is about to come back; only a backlog of
		// real transfers means it is genuinely gone.
		if (droppable) return false;
		DropChannel(ch, "Link stalled (send queue full)");
		return false;
	}

	uint8_t *w = peer.tx + peer.tx_len;
	w[0] = p.b1;
	w[1] = p.b2;
	w[2] = p.b3;
	w[3] = p.b4;
	w[4] = static_cast<uint8_t>(p.i1 & 0xFF);
	w[5] = static_cast<uint8_t>((p.i1 >> 8) & 0xFF);
	w[6] = static_cast<uint8_t>((p.i1 >> 16) & 0xFF);
	w[7] = static_cast<uint8_t>((p.i1 >> 24) & 0xFF);
	peer.tx_len += kPacketBytes;

	FlushTx(ch);
	return true;
}

bool LinkRecv(int ch, LinkPacket &out)
{
	if (ch < 0 || ch >= kLinkMaxChannels) return false;
	PeerCtx &p = g_link.peers[ch];
	if (p.rx_len - p.rx_out < kPacketBytes) return false;

	const uint8_t *r = p.rx + p.rx_out;
	out.b1 = r[0];
	out.b2 = r[1];
	out.b3 = r[2];
	out.b4 = r[3];
	out.i1 = static_cast<uint32_t>(r[4]) |
	         (static_cast<uint32_t>(r[5]) << 8) |
	         (static_cast<uint32_t>(r[6]) << 16) |
	         (static_cast<uint32_t>(r[7]) << 24);
	p.rx_out += kPacketBytes;
	return true;
}

void LinkFlush()
{
	for (int ch = 0; ch < kLinkMaxChannels; ++ch)
		ResetChannel(g_link.peers[ch]);
}

bool LinkWaitReadable(int ch, int timeout_ms)
{
	if (!LinkChannelConnected(ch)) return false;
	PeerCtx &p = g_link.peers[ch];
	if (p.rx_len - p.rx_out >= kPacketBytes) return true;

	fd_set rd;
	FD_ZERO(&rd);
	FD_SET(p.fd, &rd);

	timeval tv;
	tv.tv_sec  = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	const int rc = select(static_cast<int>(p.fd) + 1, &rd, nullptr, nullptr, &tv);
	if (rc <= 0) return false;

	PumpRx(ch);
	return p.rx_len - p.rx_out >= kPacketBytes;
}

} // namespace SGB
