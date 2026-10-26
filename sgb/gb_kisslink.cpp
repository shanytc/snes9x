/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Hudson's GB KISS LINK, the PC end of the GB KISS infrared protocol. Every
// GB KISS cart carries the same IR bank: the leader handshakes AA/55 C3/3C,
// then sends "Hu", an 8-byte command and its checksum, each byte led by a
// pulse the other side answers ("ping"); replies come back the same way with
// the cart leading. Command flow per Dan Docs (GB KISS LINK) and a traced
// Pocket Bomberman -> Pocket Family GB transfer.

#include "gb_kisslink.h"
#include "gb_cart.h"
#include "gb_memory.h"
#include "gb_ppu.h"

#include <cstring>
#include <vector>

namespace SGB {

namespace {

// Pulse periods in dots as the cart's own IR code sends them; a short pulse
// stays lit 240, not the cart's 172, since a cart can be busy ~176 dots after
// an edge before it polls again (Dan Docs: ON ~200 cycles at minimum).
constexpr int64_t kZeroOn  = 240, kZeroOff = 220;
constexpr int64_t kOneOn   = 300, kOneOff  = 420;
constexpr int64_t kStopOn  = 572, kStopOff = 650;   // ends a handshake byte
constexpr int64_t kTrailOn = 240;                   // closes every byte
// The cart's pulses, read by their period (rise to rise) as its receiver does.
constexpr int64_t kOnePeriod  = 600;
constexpr int64_t kStopPeriod = 1000;
// The LINK's own pace.
constexpr int64_t kReact    = 100;    // answering a ping edge
constexpr int64_t kHsReply  = 400;    // a handshake reply after the cart's last pulse
constexpr int64_t kByteGap  = 380;    // a byte's closing pulse to the next byte's lead
constexpr int64_t kBitsLead = 150;    // ping off to the first bit
constexpr int64_t kNextTxn  = 800;
constexpr int64_t kRetryGap = 2000;
// How long it listens: a handshake reply (the cart polls 256 times, 36 dots
// apart) and any edge within a byte (256 polls, 88 dots apart).
constexpr int64_t kHsWait    = 10700;
constexpr int64_t kEdgeWait  = 24500;
constexpr int64_t kNever     = INT64_MAX / 4;
constexpr int64_t kFrameDots = 70224;
constexpr int64_t kSensorDots = 4;    // the cart's receiver, as for two carts
constexpr uint32_t kStallFrames = 300;    // a started transfer, 5 s without a byte
constexpr uint32_t kWaitFrames  = 3600;   // a minute with no answer

enum Op : uint8_t { kOpIdle, kOpWait, kOpHsSend, kOpHsRecv, kOpByteSend, kOpByteRecv };
enum Phase : uint8_t { kTxnMasterHs, kTxnSend, kTxnSlaveHs, kTxnReply };
enum Reply : uint8_t { kReplyNone, kReplyEcho, kReplyPacket, kReplyPacketData, kReplyData };

enum : uint8_t   // SendFile
{
	kFileId, kFileStart, kFileOpen, kFileTitle, kFileReplace, kFileSlot, kFileSearch,
	kFileProbe, kFilePrep, kFileHistory, kFileData, kFileEnd, kFileCart, kFileFinish, kFileClose,
};
enum : uint8_t   // SendMap, ReceiveMap
{
	kMapId, kMapLow, kMapHigh, kMapClose,
};

enum : uint8_t
{
	kErrNone, kErrNotKiss, kErrNotMapReceive, kErrNotMapSend, kErrStalled, kErrNoAnswer,
	kErrChecksum,
};
const char *const kErrors[] = {
	nullptr,
	"the game is not on its GB KISS receive screen",
	"Nectaris GB is not waiting for a map from the PC",
	"Nectaris GB is not sending a map to the PC",
	"the game stopped answering",
	"no game answered",
	"a checksum did not match",
};

constexpr int kQueue = 48;

// Everything a savestate carries; plain bytes.
struct Core
{
	uint32_t serial;
	uint8_t  job, status, error, step;
	uint8_t  phase, hs, op, sub;
	uint8_t  byte, nbits, led, cart;
	uint8_t  q_n, q_head, slot, probe;
	uint8_t  replaced, restart, pad[2];
	uint16_t shift, idx;
	uint32_t stall, offset;
	int64_t  now, wake, mark;
	int64_t  q_t[kQueue];
	uint8_t  q_on[kQueue];
	uint8_t  rx[266];
	uint8_t  search[256];
	uint8_t  map[kNectarisMapBytes];
};

Core     g;
Memory  *g_target = nullptr;
std::vector<uint8_t> g_in;   // the job's .gbf or map record
uint32_t g_serials = 0;

struct Txn
{
	uint8_t        pkt[8];
	const uint8_t *out;
	int            out_len;
	Reply          reply;
	int            data_len;
};

struct Gbf
{
	const uint8_t *d;
	size_t  size, meta, raw;
	uint8_t flags, cart, tlen;
	bool    history;
};

const uint8_t kFlagStart = 0x01, kFlagSlot = 0x05, kFlagEnd = 0x02;

bool ParseGbf(const uint8_t *d, size_t n, Gbf &f)
{
	if (!d || n < 7) return false;
	f.d    = d;
	f.size = static_cast<size_t>(d[0] | d[1] << 8);
	if (f.size > n || f.size < 7) return false;
	f.flags   = d[2];
	f.cart    = d[3];
	f.tlen    = d[4];
	f.history = (f.flags & 0x01) != 0;
	f.meta    = 5 + f.tlen + (f.history ? 46 : 0);
	if (!f.tlen || f.meta >= f.size) return false;
	f.raw = f.size - f.meta;
	return true;
}

bool Active()
{
	return g.status == static_cast<uint8_t>(KissStatus::Waiting) ||
	       g.status == static_cast<uint8_t>(KissStatus::Busy);
}

int64_t Clock(const Memory &m)
{
	return m.ppu ? m.ppu->t_cycles : 0;
}

// ---- the LINK's LED: edges queued ahead, consumed as the cart reads ----

void Led(int64_t t, bool on)
{
	if (g.q_n == kQueue)
	{
		g.led    = g.q_on[g.q_head];
		g.q_head = static_cast<uint8_t>((g.q_head + 1) % kQueue);
		--g.q_n;
	}
	const int i = (g.q_head + g.q_n) % kQueue;
	g.q_t[i]  = t;
	g.q_on[i] = on;
	++g.q_n;
}

bool LedAt(int64_t t)
{
	while (g.q_n && g.q_t[g.q_head] <= t)
	{
		g.led    = g.q_on[g.q_head];
		g.q_head = static_cast<uint8_t>((g.q_head + 1) % kQueue);
		--g.q_n;
	}
	return g.led != 0;
}

void LedDark()
{
	g.q_n = 0;
	g.led = 0;
}

int64_t Pulse(int64_t t, int64_t on, int64_t off)
{
	Led(t, true);
	Led(t + on, false);
	return t + on + off;
}

int64_t Bits(int64_t t, uint8_t v)
{
	for (int b = 7; b >= 0; --b)
		t = (v >> b & 1) ? Pulse(t, kOneOn, kOneOff) : Pulse(t, kZeroOn, kZeroOff);
	return t;
}

// ---- byte-level operations ----

void HsSend(uint8_t v, int64_t t)
{
	t = Pulse(Bits(t, v), kStopOn, kStopOff);
	Led(t, true);
	Led(t + kTrailOn, false);
	g.op   = kOpHsSend;
	g.wake = t + kTrailOn;
}

void HsRecv(int64_t deadline)
{
	g.op    = kOpHsRecv;
	g.sub   = 0;
	g.nbits = 0;
	g.shift = 0;
	g.wake  = deadline;
}

void ByteSend(uint8_t v, int64_t t)
{
	g.op   = kOpByteSend;
	g.sub  = 0;
	g.byte = v;
	Led(t, true);
	g.wake = t + kEdgeWait;
}

void ByteRecv(int64_t t)
{
	g.op    = kOpByteRecv;
	g.sub   = 0;
	g.nbits = 0;
	g.shift = 0;
	g.wake  = t + 2 * kEdgeWait;
}

void Fail(uint8_t why)
{
	g.status = static_cast<uint8_t>(KissStatus::Failed);
	g.error  = why;
	g.op     = kOpIdle;
	LedDark();
}

// ---- transactions ----

void Packet(Txn &x, uint8_t cmd, uint16_t local, uint16_t remote, uint8_t len, uint8_t param)
{
	const uint8_t p[8] = { 0x30, cmd, static_cast<uint8_t>(local), static_cast<uint8_t>(local >> 8),
	                       static_cast<uint8_t>(remote), static_cast<uint8_t>(remote >> 8), len, param };
	std::memcpy(x.pkt, p, 8);
	x.out = nullptr;
	x.out_len = 0;
	x.reply = kReplyNone;
	x.data_len = 0;
}

uint8_t Checksum(const uint8_t *d, int n)
{
	uint8_t s = 0;
	for (int i = 0; i < n; ++i) s = static_cast<uint8_t>(s + d[i]);
	return static_cast<uint8_t>(-s);
}

// Nectaris GB's ID at $D000 for each map mode, then a map offset (send to PC).
const char kMapReceiveId[] = "RECEIVE";
const char kMapSendId[]    = "TRANCE";

void BuildTxn(Txn &x)
{
	const KissJob job = static_cast<KissJob>(g.job);
	if (job == KissJob::SendFile)
	{
		Gbf f;
		ParseGbf(g_in.data(), g_in.size(), f);
		switch (g.step)
		{
		case kFileId:      Packet(x, 0x08, 0xCE00, 0xCE00, 0x10, 0x00); x.reply = kReplyData; x.data_len = 16; break;
		case kFileStart:   Packet(x, 0x0B, 0xCE00, 0xCE00, 0x01, 0x00); x.out = &kFlagStart; x.out_len = 1; x.reply = kReplyEcho; break;
		case kFileOpen:    Packet(x, 0x00, 0xCE00, 0xCE00, 0x01, 0x00); break;
		case kFileTitle:   Packet(x, 0x02, 0xC700, 0xC50C, f.tlen, 0x00); x.out = f.d + 5; x.out_len = f.tlen; x.reply = kReplyPacketData; break;
		case kFileReplace:
		{
			static const uint8_t p[8] = { 0x30, 0x06, 0x00, 0xC7, 0x10, 0x09, 0x08, 0x23 };
			Packet(x, 0, 0, 0, 0, 0);
			std::memcpy(x.pkt, p, 8);
			x.out = g.search; x.out_len = 256; x.reply = kReplyPacket;
			break;
		}
		case kFileSlot:    Packet(x, 0x0B, 0xCE00, 0xCE00, 0x01, g.slot); x.out = &kFlagSlot; x.out_len = 1; x.reply = kReplyEcho; break;
		case kFileSearch:
			Packet(x, 0x03, 0xC700, static_cast<uint16_t>(f.raw), g.slot, f.flags);
			x.out = g.search; x.out_len = 256; x.reply = kReplyPacketData;
			break;
		case kFileProbe:   Packet(x, 0x08, 0xCE00, 0xDFFC, 0x02, g.probe); x.reply = kReplyData; x.data_len = 2; break;
		case kFilePrep:    Packet(x, 0x04, 0xC500, 0xFFD2, 0x2E, 0x00); x.reply = kReplyPacket; break;
		case kFileHistory: Packet(x, 0x0A, 0xC500, 0xC600, 0x2E, 0x00); x.out = f.d + 5 + f.tlen; x.out_len = 46; x.reply = kReplyPacket; break;
		case kFileData:
		{
			const size_t left  = f.raw - g.offset;
			const int    chunk = static_cast<int>(left < 256 ? left : 256);
			Packet(x, 0x0A, 0xC500, 0xC600, static_cast<uint8_t>(chunk), left > 256 ? 0x01 : 0x00);
			x.out = f.d + f.meta + g.offset; x.out_len = chunk; x.reply = kReplyPacket;
			break;
		}
		case kFileEnd:     Packet(x, 0x04, 0xC50B, static_cast<uint16_t>(3 - static_cast<int>(f.meta)), 0x00, 0x00); x.reply = kReplyPacket; break;
		case kFileCart:    Packet(x, 0x0A, 0xC500, 0xC50A, 0x01, 0x00); x.out = f.d + 3; x.out_len = 1; x.reply = kReplyPacket; break;
		case kFileFinish:  Packet(x, 0x0B, 0xCE00, 0xCE00, 0x01, 0x00); x.out = &kFlagEnd; x.out_len = 1; x.reply = kReplyEcho; break;
		default:           Packet(x, 0x00, 0xCE00, 0xCE00, 0x01, 0x00); break;
		}
		return;
	}
	const bool send = job == KissJob::SendMap;
	const uint16_t sram = static_cast<uint16_t>(0x4000 + g.slot * 0x200);
	switch (g.step)
	{
	case kMapId:   Packet(x, 0x08, 0xD000, 0xD000, 0x10, 0x00); x.reply = kReplyData; x.data_len = 16; break;
	case kMapLow:
	case kMapHigh:
	{
		const int half = (g.step == kMapHigh) ? 0x100 : 0;
		if (send)
		{
			Packet(x, 0x0B, static_cast<uint16_t>(0xD000 + half), static_cast<uint16_t>(0xD000 + half), 0x00, 0x00);
			x.out = g_in.data() + half; x.out_len = 256; x.reply = kReplyEcho;
		}
		else
		{
			Packet(x, 0x09, static_cast<uint16_t>(sram + half), static_cast<uint16_t>(sram + half), 0x00, 0x00);
			x.reply = kReplyData; x.data_len = 256;
		}
		break;
	}
	default:       Packet(x, 0x00, 0xCE00, 0xCE00, 0x01, 0x00); break;
	}
}

int SendLength(const Txn &x)
{
	return 11 + (x.out_len ? x.out_len + 1 : 0);
}

uint8_t SendByteAt(const Txn &x, int i)
{
	if (i == 0) return 0x48;
	if (i == 1) return 0x75;
	if (i < 10) return x.pkt[i - 2];
	if (i == 10) return Checksum(x.pkt, 8);
	if (i - 11 < x.out_len) return x.out[i - 11];
	return Checksum(x.out, x.out_len);
}

int ReplyLength(const Txn &x)
{
	switch (x.reply)
	{
	case kReplyEcho:       return 1;
	case kReplyPacket:     return 9;
	case kReplyPacketData: return 9 + 257;
	case kReplyData:       return x.data_len + 1;
	default:               return 0;
	}
}

void TxnStart(int64_t t)
{
	g.phase = kTxnMasterHs;
	g.hs    = 0;
	HsSend(0xAA, t);
}

bool Sums(const uint8_t *d, int n)
{
	uint8_t s = 0;
	for (int i = 0; i < n; ++i) s = static_cast<uint8_t>(s + d[i]);
	return s == 0;
}

void Finish()
{
	g.status = static_cast<uint8_t>(KissStatus::Done);
	g.op     = kOpIdle;
	LedDark();
}

// A transaction's reply is in: check it, pick the next step.
void TxnDone(int64_t t)
{
	Txn x;
	BuildTxn(x);
	const int n = ReplyLength(x);
	bool ok = true;
	if (x.reply == kReplyEcho)            ok = g.rx[0] == Checksum(x.out, x.out_len);
	else if (x.reply == kReplyPacketData) ok = Sums(g.rx, 9) && Sums(g.rx + 9, 257);
	else if (n)                           ok = Sums(g.rx, n);
	if (!ok) { Fail(kErrChecksum); return; }

	const KissJob job = static_cast<KissJob>(g.job);
	if (job == KissJob::SendFile)
	{
		Gbf f;
		ParseGbf(g_in.data(), g_in.size(), f);
		switch (g.step)
		{
		case kFileId:
			if (std::memcmp(g.rx + 2, "GB KISS MENU ", 13)) { Fail(kErrNotKiss); return; }
			g.slot = g.rx[15];
			g.step = kFileStart;
			break;
		case kFileStart:   g.step = kFileOpen; break;
		case kFileOpen:    g.step = kFileTitle; break;
		case kFileTitle:
			std::memcpy(g.search, g.rx + 9, 256);
			// $80: the cart holds a file of that title; replace it once.
			g.step = (g.rx[0] == 0x80 && !g.replaced) ? kFileReplace : kFileSlot;
			break;
		case kFileReplace: g.replaced = 1; g.step = kFileTitle; break;
		case kFileSlot:    g.step = kFileSearch; break;
		case kFileSearch:  g.probe = g.rx[7]; g.step = kFileProbe; break;
		case kFileProbe:   g.step = f.history ? kFilePrep : kFileData; break;
		case kFilePrep:    g.step = kFileHistory; break;
		case kFileHistory: g.step = kFileData; break;
		case kFileData:
			g.offset += static_cast<uint32_t>(x.out_len);
			if (g.offset >= f.raw) g.step = kFileEnd;
			break;
		case kFileEnd:     g.step = kFileCart; break;
		case kFileCart:    g.step = kFileFinish; break;
		case kFileFinish:  g.step = kFileClose; break;
		default:           Finish(); return;
		}
	}
	else
	{
		const bool send = job == KissJob::SendMap;
		switch (g.step)
		{
		case kMapId:
			if (send ? std::memcmp(g.rx, kMapReceiveId, sizeof kMapReceiveId - 1)
			         : std::memcmp(g.rx, kMapSendId, sizeof kMapSendId - 1))
			{
				Fail(send ? kErrNotMapReceive : kErrNotMapSend);
				return;
			}
			if (!send) g.slot = g.rx[sizeof kMapSendId - 1];
			g.step = kMapLow;
			break;
		case kMapLow:
			if (!send) std::memcpy(g.map, g.rx, 256);
			g.step = kMapHigh;
			break;
		case kMapHigh:
			if (!send) std::memcpy(g.map + 256, g.rx, 256);
			g.step = kMapClose;
			break;
		default: Finish(); return;
		}
	}
	TxnStart(t + kNextTxn);
}

// A byte-level operation ended at t: ok = finished, else it timed out.
void OnOp(int64_t t, bool ok)
{
	Txn x;
	switch (g.phase)
	{
	case kTxnMasterHs:
		switch (g.hs)
		{
		case 0: g.hs = 1; HsRecv(t + kHsWait); break;
		case 1:
			if (ok && g.byte == 0x55) { g.hs = 2; HsSend(0xC3, t + kHsReply); }
			else                      { g.hs = 0; HsSend(0xAA, t + kRetryGap); }
			break;
		case 2: g.hs = 3; HsRecv(t + kHsWait); break;
		default:
			if (!ok || g.byte != 0x3C) { g.hs = 0; HsSend(0xAA, t + kRetryGap); break; }
			g.status = static_cast<uint8_t>(KissStatus::Busy);
			g.stall  = 0;
			g.phase  = kTxnSend;
			g.idx    = 0;
			BuildTxn(x);
			ByteSend(SendByteAt(x, 0), t + kByteGap);
			break;
		}
		break;
	case kTxnSend:
		if (!ok) { Fail(kErrStalled); break; }
		g.stall = 0;
		BuildTxn(x);
		if (++g.idx < SendLength(x)) { ByteSend(SendByteAt(x, g.idx), t + kByteGap); break; }
		g.idx = 0;
		if (x.reply == kReplyNone)      TxnDone(t);
		else if (x.reply == kReplyEcho) { g.phase = kTxnReply; ByteRecv(t); }
		else                            { g.phase = kTxnSlaveHs; g.hs = 0; HsRecv(kNever); }
		break;
	case kTxnSlaveHs:
		switch (g.hs)
		{
		case 0:
			if (ok && g.byte == 0xAA) { g.hs = 1; HsSend(0x55, t + kHsReply); }
			else                      HsRecv(kNever);
			break;
		case 1: g.hs = 2; HsRecv(t + kHsWait); break;
		case 2:
			if (ok && g.byte == 0xC3)      { g.hs = 3; HsSend(0x3C, t + kHsReply); }
			else if (ok && g.byte == 0xAA) { g.hs = 1; HsSend(0x55, t + kHsReply); }
			else                           { g.hs = 0; HsRecv(kNever); }
			break;
		default:
			g.phase = kTxnReply;
			g.idx   = 0;
			ByteRecv(t);
			break;
		}
		break;
	default:   // kTxnReply
		if (!ok) { Fail(kErrStalled); break; }
		g.stall = 0;
		BuildTxn(x);
		g.rx[g.idx] = g.byte;
		if (++g.idx < ReplyLength(x)) ByteRecv(t);
		else                          TxnDone(t);
		break;
	}
}

void OpEnd(int64_t t, bool ok)
{
	g.op = kOpIdle;
	OnOp(t, ok);
}

void OnTimer(int64_t t)
{
	switch (g.op)
	{
	case kOpWait:
	case kOpHsSend:   OpEnd(t, true); break;
	case kOpByteSend: OpEnd(t, g.sub == 2); break;
	case kOpHsRecv:
	case kOpByteRecv: OpEnd(t, false); break;
	default:          g.wake = kNever; break;
	}
}

void OnEdge(int64_t t, bool on)
{
	switch (g.op)
	{
	case kOpHsRecv:
		if (on && !g.sub)
		{
			if (g.nbits)
			{
				const int64_t p = t - g.mark;
				if (p >= kStopPeriod) { g.byte = static_cast<uint8_t>(g.shift); g.sub = 1; }
				else g.shift = static_cast<uint16_t>(g.shift << 1 | (p >= kOnePeriod ? 1 : 0));
			}
			g.mark = t;
			if (g.nbits < 255) ++g.nbits;
			g.wake = t + kEdgeWait;
		}
		else if (!on && g.sub)
			OpEnd(t, true);
		break;
	case kOpByteSend:
		if (on && g.sub == 0)
		{
			Led(t + kReact, false);
			g.sub  = 1;
			g.wake = t + kEdgeWait;
		}
		else if (!on && g.sub == 1)
		{
			const int64_t s = Bits(t + kBitsLead, g.byte);
			Led(s, true);
			Led(s + kTrailOn, false);
			g.sub  = 2;
			g.wake = s + kTrailOn;
		}
		break;
	case kOpByteRecv:
		if (on && g.sub == 0)
		{
			Led(t + kReact, true);
			g.sub  = 1;
			g.wake = t + kEdgeWait;
		}
		else if (!on && g.sub == 1)
		{
			Led(t + kReact, false);
			g.sub   = 2;
			g.nbits = 0;
			g.shift = 0;
			g.wake  = t + kEdgeWait;
		}
		else if (on && g.sub == 2)
		{
			if (g.nbits) g.shift = static_cast<uint16_t>(g.shift << 1 | (t - g.mark >= kOnePeriod ? 1 : 0));
			g.mark = t;
			if (++g.nbits == 9) { g.byte = static_cast<uint8_t>(g.shift); g.sub = 3; }
			g.wake = t + kEdgeWait;
		}
		else if (!on && g.sub == 3)
			OpEnd(t, true);
		break;
	default:
		break;
	}
}

void Restart(int64_t now)
{
	const uint32_t serial = g.serial;
	const uint8_t  job    = g.job;
	std::memset(&g, 0, sizeof g);
	g.serial = serial;
	g.job    = job;
	g.status = static_cast<uint8_t>(KissStatus::Waiting);
	g.now    = now;
	TxnStart(now + kRetryGap);
}

void Advance(int64_t now)
{
	if (g.restart || now < g.now) { Restart(now); return; }
	// A cart that stopped reading while the handshake repeated: start afresh.
	if (g.phase == kTxnMasterHs && g.op != kOpIdle && g.wake < now - kFrameDots)
	{
		LedDark();
		TxnStart(now);
	}
	for (int guard = 0; Active() && g.op != kOpIdle && g.wake <= now && guard < 4096; ++guard)
		OnTimer(g.wake);
	g.now = now;
}

// ---- Nectaris GB maps ----

constexpr size_t kMapTable  = 0x14F04;   // bank, address of each built-in map
constexpr int    kFirstEmap = 66;        // EMAP01-40, the CAMPAIGN MAP mode's maps
constexpr int    kEmaps     = 40;

bool IsNectaris(const Memory &m)
{
	return m.cart && std::strcmp(m.cart->header.title, "NECTARIS GB") == 0 &&
	       m.cart->rom.size() >= 0x80000;
}

// A built-in map: width, height, then a terrain byte per cell.
const uint8_t *BuiltinMap(const Memory &m, int index)
{
	const std::vector<uint8_t> &rom = m.cart->rom;
	const size_t e = kMapTable + 3 * static_cast<size_t>(index);
	const size_t off = static_cast<size_t>(rom[e]) * 0x4000 + ((rom[e + 1] | rom[e + 2] << 8) - 0x4000);
	if (off + 2 >= rom.size()) return nullptr;
	const size_t cells = static_cast<size_t>(rom[off]) * rom[off + 1];
	return (off + 2 + cells <= rom.size()) ? &rom[off] : nullptr;
}

} // anonymous

bool KissLinkStart(Memory &m, KissJob job, const uint8_t *data, size_t size, const char **error)
{
	const char *err = nullptr;
	Gbf f;
	if (job == KissJob::SendFile && !ParseGbf(data, size, f))
		err = "not a GB KISS file (.gbf)";
	else if ((job == KissJob::SendMap || job == KissJob::ReceiveMap) && !IsNectaris(m))
		err = "maps go to Nectaris GB only";
	else if (job == KissJob::SendMap && !KissLinkIsMapRecord(data, size))
		err = "not a Nectaris GB map";
	else if (job == KissJob::None)
		err = "nothing to do";
	if (error) *error = err;
	if (err) return false;

	if (job == KissJob::ReceiveMap) g_in.clear();
	else                            g_in.assign(data, data + (job == KissJob::SendFile ? f.size : size));
	g_target = &m;
	std::memset(&g, 0, sizeof g);
	g.serial = ++g_serials;
	g.job    = static_cast<uint8_t>(job);
	Restart(Clock(m));
	return true;
}

void KissLinkCancel()
{
	if (Active()) Fail(kErrNone);
	g.status = static_cast<uint8_t>(KissStatus::Idle);
}

KissProgress KissLinkProgress()
{
	KissProgress p;
	p.job     = static_cast<KissJob>(g.job);
	p.status  = static_cast<KissStatus>(g.status);
	p.serial  = g.serial;
	p.error   = g.error < sizeof kErrors / sizeof kErrors[0] ? kErrors[g.error] : nullptr;
	p.percent = 0;
	if (p.job == KissJob::SendFile)
	{
		Gbf f;
		if (ParseGbf(g_in.data(), g_in.size(), f))
			p.percent = g.step < kFileData ? g.step : g.step > kFileData ? 95 + (g.step - kFileEnd)
			          : 15 + static_cast<int>(80 * g.offset / f.raw);
	}
	else
		p.percent = g.step * 25;
	if (p.status == KissStatus::Done) p.percent = 100;
	return p;
}

bool KissLinkReceivedMap(uint8_t out[kNectarisMapBytes])
{
	if (static_cast<KissJob>(g.job) != KissJob::ReceiveMap ||
	    g.status != static_cast<uint8_t>(KissStatus::Done)) return false;
	std::memcpy(out, g.map, kNectarisMapBytes);
	return true;
}

bool KissLinkIsMapRecord(const uint8_t *data, size_t size)
{
	if (!data || size != kNectarisMapBytes || std::memcmp(data, "NECTAR", 6)) return false;
	uint8_t s = 0;
	for (size_t i = 0; i < kNectarisMapBytes - 1; ++i) s = static_cast<uint8_t>(s + data[i]);
	return s == data[kNectarisMapBytes - 1];
}

bool KissLinkNmdToMap(const Memory &m, const uint8_t *nmd, size_t size,
                      uint8_t out[kNectarisMapBytes], const char **error)
{
	const char *dummy;
	const char *&err = error ? *error : dummy;
	err = nullptr;
	if (!IsNectaris(m)) { err = "maps go to Nectaris GB only"; return false; }
	if (!nmd || size < 0x1B || std::memcmp(nmd, "NEC MAP", 7)) { err = "not a Nectaris PC map (.nmd)"; return false; }
	const int w = nmd[0x18], h = nmd[0x19];
	const size_t units_at = 0x1A + 2 * static_cast<size_t>(w) * h;
	if (!w || !h || units_at >= size) { err = "not a Nectaris PC map (.nmd)"; return false; }

	// The PC terrain is drawn with finer tiles; the built-in map whose terrain
	// it is maps every PC tile to one Game Boy terrain. The PC's odd columns sit
	// a row lower.
	int best = -1;
	for (int i = 0; i < kEmaps; ++i)
	{
		const uint8_t *b = BuiltinMap(m, kFirstEmap + i);
		if (!b || b[0] != w || b[1] != h) continue;
		static uint16_t votes[256][16];
		std::memset(votes, 0, sizeof votes);
		int cells = 0;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const int gy = y - (x & 1);
				if (gy < 0) continue;
				++cells;
				const uint8_t gb = b[2 + gy * w + x];
				if (gb < 16) ++votes[nmd[0x1A + 2 * (y * w + x)]][gb];
			}
		int same = 0;
		for (const auto &v : votes)
		{
			int top = 0;
			for (uint16_t n : v) top = n > top ? n : top;
			same += top;
		}
		if (cells && same * 100 >= cells * 98) { best = i; break; }
	}
	if (best < 0) { err = "no Nectaris GB map has this map's terrain"; return false; }

	std::memset(out, 0, kNectarisMapBytes);
	std::memcpy(out, "NECTAR", 6);
	out[6] = static_cast<uint8_t>(0x20 + best);
	out[7] = 0x04;
	out[8] = 'E'; out[9] = 'M'; out[10] = 'A'; out[11] = 'P';
	out[12] = static_cast<uint8_t>('0' + (best + 1) / 10);
	out[13] = static_cast<uint8_t>('0' + (best + 1) % 10);
	// As the cart's own editor saves one: no terrain changes, its cursor at 5,5.
	out[0x0E] = out[0x0F] = 0xFF;
	std::memset(out + 0x14, 0xFF, 0x17C - 0x14);
	out[0x17C] = out[0x17D] = 0x05;
	int n = 0;
	for (size_t p = units_at; p + 2 < size && nmd[p] != 0xFF && n < 56; p += 3, ++n)
	{
		uint8_t *u = out + 0x2C + 6 * n;
		u[0] = nmd[p];
		u[1] = nmd[p + 1];
		u[2] = nmd[p + 2];
		u[3] = 0x00;     // level
		u[4] = 0xFF;
		u[5] = 0x08;     // quantity
	}
	std::memset(out + 0x180, 0xFF, 0x1E4 - 0x180);
	uint8_t s = 0;
	for (size_t i = 0; i < kNectarisMapBytes - 1; ++i) s = static_cast<uint8_t>(s + out[i]);
	out[kNectarisMapBytes - 1] = s;
	return true;
}

bool KissLinkIsNectaris(const Memory &m)
{
	return IsNectaris(m);
}

bool KissLinkFacing(const Memory &m)
{
	return &m == g_target && Active();
}

bool KissLinkLit(const Memory &m)
{
	if (!KissLinkFacing(m)) return false;
	const int64_t t = Clock(m);
	Advance(t);
	return Active() && LedAt(t - kSensorDots);
}

void KissLinkCartLed(const Memory &m, bool on)
{
	if (!KissLinkFacing(m)) return;
	const int64_t t = Clock(m);
	Advance(t);
	if (!Active() || (g.cart != 0) == on) return;
	g.cart = on;
	OnEdge(t, on);
}

void KissLinkFrame(const Memory &m)
{
	if (!KissLinkFacing(m)) return;
	Advance(Clock(m));
	if (!Active()) return;
	++g.stall;
	if (g.status == static_cast<uint8_t>(KissStatus::Busy) && g.stall > kStallFrames) Fail(kErrStalled);
	else if (g.status == static_cast<uint8_t>(KissStatus::Waiting) && g.stall > kWaitFrames) Fail(kErrNoAnswer);
}

size_t KissLinkStateBytes()
{
	return sizeof(Core);
}

void KissLinkStateSave(const Memory &m, uint8_t *out)
{
	if (&m == g_target) std::memcpy(out, &g, sizeof g);
	else                std::memset(out, 0, sizeof g);
}

// The state's job is this job: resume it. A state from before it: the job
// starts over at the next read (the cart's clock is not linked up yet here).
void KissLinkStateLoad(Memory &m, const uint8_t *in)
{
	if (&m != g_target || !g.serial) return;
	Core c;
	std::memcpy(&c, in, sizeof c);
	if (c.serial == g.serial) { g = c; return; }
	if (Active()) g.restart = 1;
}

} // namespace SGB
