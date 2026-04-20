/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_memory.h"
#include "gb_cart.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_mbc.h"
#include "sgb.h"

#include <cstring>

namespace SGB {

namespace {
SerialByteCallback g_serial_cb = nullptr;
uint8_t            g_dma_last  = 0xFF;  // 0xFF46 last written byte — register reads echo this.
}

void SetSerialCallback(SerialByteCallback cb) { g_serial_cb = cb; }

void MemReset(Memory &m)
{
	std::memset(m.wram, 0, sizeof m.wram);
	std::memset(m.hram, 0, sizeof m.hram);
	m.ie          = 0;
	m.if_         = 0xE1;   // bits 5-7 always set, VBlank latent
	m.serial_data = 0;
	g_dma_last    = 0xFF;
}

static uint8_t ReadIO(Memory &m, uint16_t addr);
static void    WriteIO(Memory &m, uint16_t addr, uint8_t value);
static void    DoOamDma(Memory &m, uint8_t value);

uint8_t MemRead(Memory &m, uint16_t addr)
{
	if (addr < 0x8000)
	{
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr) : 0xFF;
	}
	if (addr < 0xA000)
	{
		return m.ppu ? m.ppu->vram[addr - 0x8000] : 0xFF;
	}
	if (addr < 0xC000)
	{
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr) : 0xFF;
	}
	if (addr < 0xE000)
	{
		return m.wram[addr - 0xC000];
	}
	if (addr < 0xFE00)
	{
		return m.wram[addr - 0xE000];  // Echo RAM mirrors C000-DDFF
	}
	if (addr < 0xFEA0)
	{
		return m.ppu ? m.ppu->oam[addr - 0xFE00] : 0xFF;
	}
	if (addr < 0xFF00)
	{
		return 0xFF;  // unusable
	}
	if (addr < 0xFF80)
	{
		return ReadIO(m, addr);
	}
	if (addr < 0xFFFF)
	{
		return m.hram[addr - 0xFF80];
	}
	return m.ie;
}

void MemWrite(Memory &m, uint16_t addr, uint8_t value)
{
	if (addr < 0x8000)
	{
		if (m.cart) MbcWrite(m.cart->mbc, m.cart->sram, addr, value);
		return;
	}
	if (addr < 0xA000)
	{
		if (m.ppu) m.ppu->vram[addr - 0x8000] = value;
		return;
	}
	if (addr < 0xC000)
	{
		if (m.cart) MbcWrite(m.cart->mbc, m.cart->sram, addr, value);
		return;
	}
	if (addr < 0xE000)
	{
		m.wram[addr - 0xC000] = value;
		return;
	}
	if (addr < 0xFE00)
	{
		m.wram[addr - 0xE000] = value;
		return;
	}
	if (addr < 0xFEA0)
	{
		if (m.ppu) m.ppu->oam[addr - 0xFE00] = value;
		return;
	}
	if (addr < 0xFF00)
	{
		return;  // unusable
	}
	if (addr < 0xFF80)
	{
		WriteIO(m, addr, value);
		return;
	}
	if (addr < 0xFFFF)
	{
		m.hram[addr - 0xFF80] = value;
		return;
	}
	m.ie = value;
}

uint16_t MemRead16(Memory &m, uint16_t addr)
{
	uint16_t lo = MemRead(m, addr);
	uint16_t hi = MemRead(m, static_cast<uint16_t>(addr + 1));
	return static_cast<uint16_t>(lo | (hi << 8));
}

void MemWrite16(Memory &m, uint16_t addr, uint16_t value)
{
	MemWrite(m, addr,                              static_cast<uint8_t>(value & 0xFF));
	MemWrite(m, static_cast<uint16_t>(addr + 1),  static_cast<uint8_t>((value >> 8) & 0xFF));
}

// ---------------------------------------------------------------------------
// I/O register dispatch
// ---------------------------------------------------------------------------

static uint8_t ReadIO(Memory &m, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF00: return m.joypad ? JoypadRead(*m.joypad) : 0xFF;
		case 0xFF01: return m.serial_data;
		case 0xFF02: return 0x7E;            // SC: bits 1-6 unused, reads as 1
		case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
			return m.timer ? TimerRead(*m.timer, addr) : 0xFF;
		case 0xFF0F: return static_cast<uint8_t>(m.if_ | 0xE0);
		case 0xFF46: return g_dma_last;
	}
	if (addr >= 0xFF10 && addr <= 0xFF3F)
	{
		return m.apu ? ApuRead(*m.apu, addr) : 0xFF;
	}
	if (addr >= 0xFF40 && addr <= 0xFF4B)
	{
		return m.ppu ? PpuReadReg(*m.ppu, addr) : 0xFF;
	}
	return 0xFF;
}

static void WriteIO(Memory &m, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF00:
			if (m.joypad) JoypadWrite(*m.joypad, value);
			// Feed SGB command-packet sniffer. Benign when SGB mode inactive.
			S9xSGBOnJoyserWrite(value);
			return;
		case 0xFF01:
			m.serial_data = value;
			return;
		case 0xFF02:
			// Bit 7 = start. We don't emulate a real link — fire the observer
			// callback with the captured byte and signal instant completion.
			if (value & 0x80)
			{
				if (g_serial_cb) g_serial_cb(m.serial_data);
				m.if_ = static_cast<uint8_t>(m.if_ | IRQ_SERIAL);
			}
			return;
		case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
			if (m.timer) TimerWrite(*m.timer, addr, value);
			return;
		case 0xFF0F:
			m.if_ = static_cast<uint8_t>((value & 0x1F) | 0xE0);
			return;
		case 0xFF46:
			g_dma_last = value;
			DoOamDma(m, value);
			return;
	}
	if (addr >= 0xFF10 && addr <= 0xFF3F)
	{
		if (m.apu) ApuWrite(*m.apu, addr, value);
		return;
	}
	if (addr >= 0xFF40 && addr <= 0xFF4B)
	{
		if (m.ppu) PpuWriteReg(*m.ppu, addr, value);
		return;
	}
	// Remaining I/O addresses (CGB regs, boot-ROM disable, etc.) are ignored.
}

// ---------------------------------------------------------------------------
// OAM DMA — instant copy of 160 bytes from (value << 8) to OAM.
// Real HW takes 160 T-cycles and blocks non-HRAM access during that time.
// P3 may tighten the timing; most games are unaffected by the simplification.
// ---------------------------------------------------------------------------

static void DoOamDma(Memory &m, uint8_t value)
{
	if (!m.ppu) return;
	const uint16_t src = static_cast<uint16_t>(value << 8);
	for (int i = 0; i < 0xA0; ++i)
	{
		m.ppu->oam[i] = MemRead(m, static_cast<uint16_t>(src + i));
	}
}

} // namespace SGB
