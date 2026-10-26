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

// The sensor adapts to a steady light and reads "normal" again after ~3 ms.
constexpr int64_t kIrFadeDots = 12583;

const Memory *Facing(const Memory &m)
{
	if (&m == g_ir[0]) return g_ir[1];
	if (&m == g_ir[1]) return g_ir[0];
	return nullptr;
}

} // anonymous

void IrPair(Memory *a, Memory *b)
{
	if (!a || !b || a == b) { IrUnpair(); return; }
	g_ir[0] = a;
	g_ir[1] = b;
}

void IrUnpair()
{
	g_ir[0] = g_ir[1] = nullptr;
}

bool IrPaired()
{
	return g_ir[0] != nullptr;
}

bool IrActive()
{
	if (!g_ir[0]) return false;
	for (const Memory *m : g_ir)
		if ((m->rp & 0xC1) || (m->cart_ir & 0x01) || IrCartSelected(*m)) return true;
	return false;
}

uint8_t IrReadRP(const Memory &m)
{
	uint8_t v = static_cast<uint8_t>((m.rp & 0xC1) | 0x3E);
	if ((m.rp & 0xC0) != 0xC0) return v;
	const Memory *p = Facing(m);
	if (p && (p->rp & 0x01) && p->ppu && p->ppu->t_cycles - p->rp_lit_at < kIrFadeDots)
		v = static_cast<uint8_t>(v & ~0x02);
	return v;
}

void IrWriteRP(Memory &m, uint8_t value)
{
	if ((value & 0x01) && !(m.rp & 0x01))
		m.rp_lit_at = m.ppu ? m.ppu->t_cycles : 0;
	m.rp = static_cast<uint8_t>(value & 0xC1);
}

bool IrCartSelected(const Memory &m)
{
	return m.cart && m.cart->mbc.type == MbcType::HuC1 && !m.cart->mbc.ram_enable;
}

// No fade here: Chousoku Spinner's pulses are at most ~560 dots and every wait
// for light gives up after 256 polls, so a steady LED never outlasts its reads.
uint8_t IrReadCart(const Memory &m)
{
	const Memory *p = Facing(m);
	return (p && (p->cart_ir & 0x01)) ? 0xC1 : 0xC0;
}

void IrWriteCart(Memory &m, uint8_t value)
{
	m.cart_ir = static_cast<uint8_t>(value & 0x01);
}

} // namespace SGB
