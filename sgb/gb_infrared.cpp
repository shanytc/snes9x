/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// RP bits per Pan Docs "Infrared Communication": 0 = LED, 1 = sensor (0 = light
// seen, only while bits 6-7 enable reading), 2-5 read 1.

#include "gb_infrared.h"
#include "gb_cart.h"
#include "gb_memory.h"
#include "gb_ppu.h"

namespace SGB {

namespace {

Memory *g_ir[2] = { nullptr, nullptr };
bool    g_port_on = false;   // the Game Boy Color port (Infra-Red)
bool    g_cart_on = false;   // HuC1/HuC3 cart LEDs (GB KISS)

// The sensor adapts to a steady light and reads "normal" again after ~3 ms.
constexpr int64_t kIrFadeDots = 12583;

// A cart's receiver answers ~1 us after the light changes. Hudson's handshake
// needs it: read instantly, a reply's first pulse can land just before the
// leader listens, and cycle-exact cores then miss it on every retry.
constexpr int64_t kCartSensorDots = 4;

// The split loop runs the paired cores a few dots apart, so a sensor reading
// the partner's live LED would see it early or late by up to an instruction.
// Each LED edge is kept on one shared clock instead, and a read looks up the
// level at the reader's own moment.
constexpr int kEdges = 16;
struct Led
{
	int64_t t[kEdges];
	bool    on[kEdges];
	int     n = 0, head = 0;
};
struct Seat
{
	int64_t local0 = 0, common0 = 0;   // this core's cycle count at a known shared time
	Led     port, cart;
};
Seat g_seat[2];

int SeatOf(const Memory &m)
{
	return (&m == g_ir[0]) ? 0 : (&m == g_ir[1]) ? 1 : -1;
}

int64_t Now(int s, const Memory &m)
{
	return g_seat[s].common0 + ((m.ppu ? m.ppu->t_cycles : 0) - g_seat[s].local0);
}

void Record(Led &l, int64_t t, bool on)
{
	if (l.n && l.on[l.head] == on) return;
	l.head = (l.head + 1) % kEdges;
	l.t[l.head]  = t;
	l.on[l.head] = on;
	if (l.n < kEdges) ++l.n;
}

// The level at t, and since when it has been on; `live` if the log is too short.
bool LevelAt(const Led &l, int64_t t, bool live, int64_t *since)
{
	for (int k = 0, i = l.head; k < l.n; ++k, i = (i + kEdges - 1) % kEdges)
		if (l.t[i] <= t)
		{
			if (since) *since = l.t[i];
			return l.on[i];
		}
	if (since) *since = t;
	return live;
}

} // anonymous

void IrPair(Memory *a, Memory *b)
{
	if (!a || !b || a == b) { IrUnpair(); return; }
	g_seat[0] = g_seat[1] = Seat();   // a session (re)start: no edge from before it counts
	g_ir[0] = a;
	g_ir[1] = b;
}

void IrUnpair()
{
	g_ir[0] = g_ir[1] = nullptr;
	g_seat[0] = g_seat[1] = Seat();
}

bool IrPaired()
{
	return g_ir[0] != nullptr;
}

void IrSetMedia(bool port, bool cart)
{
	g_port_on = port;
	g_cart_on = cart;
}

void IrSetClock(const Memory *m, int64_t local, int64_t common)
{
	const int s = m ? SeatOf(*m) : -1;
	if (s < 0) return;
	if (common < g_seat[s].common0)   // the clock went back (a reset): drop both logs
		g_seat[0].port.n = g_seat[0].cart.n = g_seat[1].port.n = g_seat[1].cart.n = 0;
	g_seat[s].local0  = local;
	g_seat[s].common0 = common;
	if (!g_seat[s].port.n) Record(g_seat[s].port, common, m->rp & 0x01);
	if (!g_seat[s].cart.n) Record(g_seat[s].cart, common, m->cart_ir & 0x01);
}

bool IrActive()
{
	if (!g_ir[0]) return false;
	for (const Memory *m : g_ir)
		if ((g_port_on && (m->rp & 0xC1)) ||
		    (g_cart_on && ((m->cart_ir & 0x01) || IrCartSelected(*m)))) return true;
	return false;
}

uint8_t IrReadRP(const Memory &m)
{
	uint8_t v = static_cast<uint8_t>((m.rp & 0xC1) | 0x3E);
	const int s = SeatOf(m);
	if ((m.rp & 0xC0) != 0xC0 || !g_port_on || s < 0) return v;
	const int64_t now = Now(s, m);
	int64_t since = now;
	if (LevelAt(g_seat[1 - s].port, now, g_ir[1 - s]->rp & 0x01, &since) && now - since < kIrFadeDots)
		v = static_cast<uint8_t>(v & ~0x02);
	return v;
}

void IrWriteRP(Memory &m, uint8_t value)
{
	if ((value & 0x01) && !(m.rp & 0x01))
		m.rp_lit_at = m.ppu ? m.ppu->t_cycles : 0;
	const int s = SeatOf(m);
	if (s >= 0) Record(g_seat[s].port, Now(s, m), value & 0x01);
	m.rp = static_cast<uint8_t>(value & 0xC1);
}

bool IrCartSelected(const Memory &m)
{
	if (!m.cart) return false;
	if (m.cart->mbc.type == MbcType::HuC1) return !m.cart->mbc.ram_enable;
	return m.cart->mbc.type == MbcType::HuC3 && m.cart->mbc.rtc_select == 0x0E;
}

// No fade here: Chousoku Spinner's pulses are at most ~560 dots and every wait
// for light gives up after 256 polls, so a steady LED never outlasts its reads.
uint8_t IrReadCart(const Memory &m)
{
	const int s = SeatOf(m);
	const bool lit = s >= 0 && g_cart_on &&
	                 LevelAt(g_seat[1 - s].cart, Now(s, m) - kCartSensorDots, g_ir[1 - s]->cart_ir & 0x01, nullptr);
	const uint8_t base = (m.cart && m.cart->mbc.type == MbcType::HuC1) ? 0xC0 : 0x00;
	return static_cast<uint8_t>(base | (lit ? 0x01 : 0x00));
}

void IrWriteCart(Memory &m, uint8_t value)
{
	const int s = SeatOf(m);
	if (s >= 0) Record(g_seat[s].cart, Now(s, m), value & 0x01);
	m.cart_ir = static_cast<uint8_t>(value & 0x01);
}

} // namespace SGB
