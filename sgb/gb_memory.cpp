/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_memory.h"
#include "gb_knob.h"
#include "gb_cart.h"
#include "gb_cpu.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_mbc.h"
#include "sgb.h"

#include <cstring>

namespace SGB {

namespace {
}

// CPU-visible unblock leads the render machine's mode-0 entry by a few
// dots, like the STAT mode bits (mooneye lcdon_timing access tables).
inline bool CpuVisibleMode0(const Memory &m)
{
	static const int ub = AcidKnob("ACID_UB", 0);
	return ub > 0 && !m.ppu->cgb && !m.ppu->lcdon_line &&
	       m.ppu->tm.lcd_x >= GB_SCREEN_WIDTH - ub;
}

// The LCD is still taking pixels for this line: the output machine trails
// the timing skeleton, and the CPU's locks follow the pixels.
inline bool PpuStillRendering(const Memory &m)
{
	return PpuLcdDraining(*m.ppu);
}

inline bool VramBlocked(const Memory &m, bool write = false)
{
	if (m.dma_vram_bypass || !m.ppu || !(m.ppu->lcdc & 0x80)) return false;
	if (PpuStillRendering(m)) return !CpuVisibleMode0(m);
	// Only READS lock ahead of the transfer; writes pass until mode 3
	// (Coffee GB isVramAvailableForCpu; mooneye lcdon_write_timing).
	if (write) return false;
	// The lock engages a few dots before the machine's mode-3 entry, in
	// step with the visible STAT flip (mooneye lcdon_timing VRAM table).
	static const int vlk = AcidKnob("ACID_VLK", 5);
	return vlk > 0 && !m.ppu->cgb && m.ppu->mode == PpuMode::OamScan &&
	       !m.ppu->lcdon_first && m.ppu->mode_clock >= 87 - vlk;
}

// OAM is CPU-inaccessible during the OAM scan and pixel transfer. The scan
// state is entered 4 dots early on our grid; the hardware lock engages at
// line dot 1 (SameBoy), i.e. mode_clock 1.
inline bool OamBlocked(const Memory &m)
{
	if (!m.ppu || !(m.ppu->lcdc & 0x80)) return false;
	if (PpuStillRendering(m)) return !CpuVisibleMode0(m);
	// The glitched first line after LCD enable never locks OAM until its
	// (early) mode 3 (mooneye lcdon_write_timing).
	static const int oe = AcidKnob("ACID_OE", 1);
	return m.ppu->mode == PpuMode::OamScan && !m.ppu->lcdon_first &&
	       m.ppu->mode_clock >= oe;
}

// ------------------------------------------------------------------
// DMG OAM corruption bug (SameBoy memory.c port). A CPU access — or a
// 16-bit inc/dec whose register rides the bus — with an address in
// $FE00-$FEFF while the PPU scans OAM garbles the row being scanned.
// ------------------------------------------------------------------
static inline uint16_t BgGlitch(uint16_t a, uint16_t b, uint16_t c)
{
	return static_cast<uint16_t>(((a ^ c) & (b ^ c)) ^ c);
}
static inline uint16_t BgGlitchRead(uint16_t a, uint16_t b, uint16_t c)
{
	return static_cast<uint16_t>(b | (a & c));
}
static inline uint16_t BgGlitchReadSec(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
	return static_cast<uint16_t>((b & (a | c | d)) | (a & c & d));
}
static inline uint16_t BgGlitchTert1(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
	return static_cast<uint16_t>(c | (a & b & d & e));
}
static inline uint16_t BgGlitchTert2(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
	return static_cast<uint16_t>((c & (a | b | d | e)) | (a & b & d & e));
}
static inline uint16_t BgGlitchTert3(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
	return static_cast<uint16_t>((c & (a | b | d | e)) | (b & d & e));
}
static inline uint16_t BgGlitchQuatDmg(uint16_t a, uint16_t b, uint16_t c, uint16_t d,
                                       uint16_t e, uint16_t f, uint16_t g, uint16_t h)
{
	(void)a;
	return static_cast<uint16_t>((e & (h | g | (~d & f) | c | b)) | (c & g & h));
}

static inline uint16_t OamW(const uint8_t *o) { return static_cast<uint16_t>(o[0] | (o[1] << 8)); }
static inline void OamWSet(uint8_t *o, uint16_t v) { o[0] = static_cast<uint8_t>(v); o[1] = static_cast<uint8_t>(v >> 8); }

// Row (byte offset) the OAM scan is currently touching, or -1.
static int AccessedOamRow(const Memory &m)
{
	const Ppu *p = m.ppu;
	if (!p || p->cgb || !(p->lcdc & 0x80)) return -1;
	if (p->mode != PpuMode::OamScan || p->lcdon_first) return -1;
	static const int obw = AcidKnob("ACID_OBW", -4);
	const int d = p->mode_clock + obw;   // true line dot on our -4 grid
	if (d < 0 || d >= 82) return -1;
	if (d < 2)  return 0;
	int i = (d - 2) >> 1;
	if (i > 39) i = 39;
	return (i & ~1) * 4 + 8;
}

static void OamBugWriteCorrupt(Memory &m)
{
	const int row = AccessedOamRow(m);
	if (row < 8 || row > 0x98) return;
	uint8_t *oam = m.ppu->oam;
	OamWSet(&oam[row], BgGlitch(OamW(&oam[row]), OamW(&oam[row - 8]), OamW(&oam[row - 4])));
	for (int i = 2; i < 8; ++i)
		oam[row + i] = oam[row - 8 + i];
}

static void OamBugReadCorrupt(Memory &m)
{
	const int row = AccessedOamRow(m);
	if (row < 8 || row > 0x98) return;
	uint8_t *oam = m.ppu->oam;
	if ((row & 0x18) == 0x10)
	{
		OamWSet(&oam[row - 8],
			BgGlitchReadSec(OamW(&oam[row - 0x10]), OamW(&oam[row - 8]),
			                OamW(&oam[row]), OamW(&oam[row - 4])));
		for (int i = 0; i < 8; ++i)
			oam[row - 0x10 + i] = oam[row - 0x08 + i];
	}
	else if ((row & 0x18) == 0x00)
	{
		if (row == 0x40)
		{
			OamWSet(&oam[row - 8],
				BgGlitchQuatDmg(OamW(&oam[0]), OamW(&oam[row]), OamW(&oam[row - 4]),
				                OamW(&oam[row - 6]), OamW(&oam[row - 8]),
				                OamW(&oam[row - 14]), OamW(&oam[row - 16]),
				                OamW(&oam[row - 32])));
		}
		else
		{
			uint16_t (*op)(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) =
				row == 0x20 ? BgGlitchTert2 : row == 0x60 ? BgGlitchTert3 : BgGlitchTert1;
			OamWSet(&oam[row - 8],
				op(OamW(&oam[row]), OamW(&oam[row - 4]), OamW(&oam[row - 8]),
				   OamW(&oam[row - 16]), OamW(&oam[row - 32])));
		}
		for (int i = 0; i < 8; ++i)
			oam[row - 0x10 + i] = oam[row - 0x20 + i] = oam[row - 0x08 + i];
	}
	else
	{
		const uint16_t v = BgGlitchRead(OamW(&oam[row]), OamW(&oam[row - 8]), OamW(&oam[row - 4]));
		OamWSet(&oam[row], v);
		OamWSet(&oam[row - 8], v);
	}
	for (int i = 0; i < 8; ++i)
		oam[row + i] = oam[row - 8 + i];
}

// OAM WRITE blocking differs from read blocking: it engages later in the
// scan and releases for the scan's last dots (Coffee GB
// isOamAvailableForCpu(write); mooneye lcdon_write_timing).
static bool OamWriteBlocked(const Memory &m)
{
	if (!m.ppu || !(m.ppu->lcdc & 0x80)) return false;
	const Ppu &p = *m.ppu;
	if (p.cgb) return OamBlocked(m);
	if (PpuStillRendering(m)) return !CpuVisibleMode0(m);
	if (p.mode != PpuMode::OamScan || p.lcdon_first) return false;
	static const int oew = AcidKnob("ACID_OEW", 5);
	static const int owf = AcidKnob("ACID_OWF", 82);
	if (owf > 0 && p.mode_clock >= owf && p.mode_clock < owf + 4) return false;
	return p.mode_clock >= oew;
}

// Hook for 16-bit inc/dec whose register value sits on the bus.
void MemOamBugIncDec(Memory &m, uint16_t value)
{
	if (value >= 0xFE00 && value < 0xFF00)
		OamBugWriteCorrupt(m);
}

inline bool CramBlocked(const Memory &m)
{
	return m.ppu && m.ppu->cgb &&
	       m.ppu->mode == PpuMode::Transfer && (m.ppu->lcdc & 0x80) &&
	       m.ppu->mode_clock > GB_MODE3_SETUP_DOTS;
}

void SetSerialCallback(Memory &m, SerialByteCallback cb, void *user)
{
	m.serial_cb   = cb;
	m.serial_user = user;
}

void MemReset(Memory &m, bool cgb)
{
	std::memset(m.wram, 0, sizeof m.wram);
	std::memset(m.hram, 0, sizeof m.hram);
	m.ie             = 0;
	m.if_            = 0xE1;   // bits 5-7 always set, VBlank latent
	m.serial_data    = 0;
	m.serial_control = 0;
	m.serial_bits    = 0;
	// Zeroed on reset; S9xSGBLoadBootROM refills before the GB CPU starts.
	std::memset(m.boot_rom, 0, sizeof m.boot_rom);
	m.boot_rom_size    = 0;
	m.boot_rom_enabled = false;
	m.dma_last         = cgb ? 0x00 : 0xFF;

	m.svbk         = 1;
	m.key1_armed   = false;
	m.double_speed = false;
	m.ff72 = m.ff73 = m.ff74 = m.ff75 = 0;
	m.hdma1 = m.hdma2 = m.hdma3 = m.hdma4 = 0;
	m.hdma5        = 0xFF;
	m.hdma_src = m.hdma_dst = m.hdma_len = 0;
	m.hdma_active  = false;
	m.hdma_hblank_latch = false;
	m.ds_tick_rem  = 0;
	m.cgb_hw       = cgb;
	m.dma_active   = false;
	m.dma_index    = 0;
	m.dma_src      = 0;
	m.dma_setup    = 0;
	m.dma_src_next = 0;
	m.dma_bus_byte = 0xFF;
}

static uint8_t ReadIO(Memory &m, uint16_t addr);
static void    WriteIO(Memory &m, uint16_t addr, uint8_t value);
static void    HdmaTrigger(Memory &m, uint8_t value);

namespace {
inline uint32_t VramBankBase(const Memory &m)
{
	return (m.ppu && (m.ppu->vbk & 1)) ? 0x2000u : 0u;
}

inline uint32_t WramBankBase(const Memory &m)
{
	if (m.ppu && m.ppu->cgb)
	{
		const uint8_t b = m.svbk & 0x07;
		return static_cast<uint32_t>(b ? b : 1) * 0x1000u;
	}
	return 0x1000u;
}

inline void CgbWritePalette(uint8_t *pal, uint8_t &idx, uint8_t value, bool store)
{
	if (store) pal[idx & 0x3F] = value;
	if (idx & 0x80) idx = static_cast<uint8_t>((idx & 0x80) | ((idx + 1) & 0x3F));
}
} // namespace

// Which physical bus an address lives on for OAM-DMA conflict purposes:
// 0 = external (ROM/SRAM, plus WRAM on DMG), 1 = video (VRAM), 2 = the
// CGB's separate WRAM bus.
static int DmaBusOf(const Memory &m, uint16_t addr)
{
	if (addr >= 0x8000 && addr < 0xA000) return 1;
	if (m.ppu && m.ppu->cgb && addr >= 0xC000) return 2;
	return 0;
}

// DMA's own source reads: normal address decoding but no PPU blocking, and
// the echo region + $FE00-$FFFF fold down to WRAM (external-bus mirror).
static uint8_t DmaReadByte(Memory &m, uint16_t addr)
{
	if (addr >= 0xE000) addr = static_cast<uint16_t>(addr - 0x2000);
	if (addr < 0x8000)
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr, m.cart->mbc1_multicart) : 0xFF;
	if (addr < 0xA000)
		return m.ppu ? m.ppu->vram[(addr - 0x8000) + VramBankBase(m)] : 0xFF;
	if (addr < 0xC000)
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr, m.cart->mbc1_multicart) : 0xFF;
	if (addr < 0xD000)
		return m.wram[addr - 0xC000];
	return m.wram[WramBankBase(m) + (addr - 0xD000)];
}

// Advance the OAM DMA engine by one M-cycle.
static void DmaTickM(Memory &m)
{
	if (m.dma_setup > 0)
	{
		// The staged transfer goes live one M-cycle after the $FF46 write;
		// an already-running transfer keeps copying during that window.
		if (--m.dma_setup == 0)
		{
			m.dma_active = true;
			m.dma_index  = 0;
			m.dma_src    = m.dma_src_next;
			if (m.ppu)
				std::memcpy(m.dma_oam_old, m.ppu->oam, sizeof m.dma_oam_old);
		}
	}
	if (m.dma_active)
	{
		if (m.dma_index >= 0xA0)
		{
			m.dma_active = false;
			return;
		}
		const uint8_t b = DmaReadByte(m, static_cast<uint16_t>(m.dma_src + m.dma_index));
		m.dma_bus_byte  = b;
		if (m.ppu) m.ppu->oam[m.dma_index] = b;
		++m.dma_index;
		// The bus stays claimed through the last byte's M-cycle; the
		// head-of-tick check above releases it one cycle later
		// (mooneye oam_dma_timing wants the +161 cycle still blocked).
	}
}

void MemTick(Memory &m, int32_t tcycles, bool tick_dma)
{
	// STOP halts the oscillator: DIV/TIMA and OAM DMA freeze; the APU
	// freezes too on DMG (it keeps running on CGB hardware).
	const bool stopped = m.cpu && m.cpu->stopped;

	if (!stopped && m.timer) TimerStep(*m.timer, m, tcycles);

	// One DMA byte per 4 CPU T-cycles (a split write cycle ticks DMA only
	// in its first half so the engine still sees whole M-cycles).
	if (!stopped && tick_dma)
		for (int32_t t = 0; t < tcycles; t += 4)
			DmaTickM(m);

	int32_t rt = tcycles;
	if (m.double_speed)
	{
		const int32_t acc = m.ds_tick_rem + tcycles;
		rt            = acc >> 1;
		m.ds_tick_rem = static_cast<uint8_t>(acc & 1);
	}
	if (rt > 0)
	{
		if (m.ppu) PpuStep(*m.ppu, m, rt);
		if (m.apu && !(stopped && !m.cgb_hw)) ApuStep(*m.apu, rt);
		if (m.cart) MbcTickRtc(m.cart->mbc, rt);
	}
}


uint8_t MemRead(Memory &m, uint16_t addr)
{
	// OAM DMA bus conflict: a CPU read on the bus the DMA source occupies
	// returns the byte currently on the DMA bus (OAM itself reads $FF,
	// handled below; HRAM/IO live on neither bus).
	if (m.dma_active && !m.dma_vram_bypass && addr < 0xFE00 &&
	    DmaBusOf(m, m.dma_src) == DmaBusOf(m, addr))
		return m.dma_bus_byte;
	// Boot ROM overlay. A CGB boot ROM also covers 0x0200-0x08FF; the gap at
	// 0x0100-0x01FF stays cart ROM, which is where it reads the header from.
	if (m.boot_rom_enabled &&
	    (addr < 0x0100 || (addr >= 0x0200 && addr < m.boot_rom_size)))
	{
		return m.boot_rom[addr];
	}
	// While the boot ROM is mapped, a Sachen cart's mapper supplies the Nintendo
	// logo at 0x0104-0x0133 so the DMG/SGB boot logo check passes — what the
	// hardware does (logo-less single-game carts store none). Gated on
	// boot_rom_enabled, so once the boot hands off the running game reads its
	// real address-swapped header and the cart's own self-check still matches.
	if (m.boot_rom_enabled && m.cart)
	{
		uint8_t logo;
		if (SachenBootLogoByte(*m.cart, addr, logo)) return logo;
	}
	if (addr < 0x8000)
	{
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr, m.cart->mbc1_multicart) : 0xFF;
	}
	if (addr < 0xA000)
	{
		if (!m.ppu || VramBlocked(m)) return 0xFF;
		return m.ppu->vram[(addr - 0x8000) + VramBankBase(m)];
	}
	if (addr < 0xC000)
	{
		return m.cart ? MbcRead(m.cart->mbc, m.cart->rom, m.cart->sram, addr, m.cart->mbc1_multicart) : 0xFF;
	}
	if (addr < 0xD000)
	{
		return m.wram[addr - 0xC000];
	}
	if (addr < 0xE000)
	{
		return m.wram[WramBankBase(m) + (addr - 0xD000)];
	}
	if (addr < 0xF000)
	{
		return m.wram[addr - 0xE000];  // Echo of C000-CFFF
	}
	if (addr < 0xFE00)
	{
		return m.wram[WramBankBase(m) + (addr - 0xF000)];  // Echo of D000-DDFF
	}
	if (addr < 0xFF00)
	{
		if (!m.dma_active && AccessedOamRow(m) >= 0)
		{
			OamBugReadCorrupt(m);
			return 0xFF;
		}
		if (addr >= 0xFEA0) return 0xFF;   // unusable
		if (m.dma_active || OamBlocked(m)) return 0xFF;
		return m.ppu ? m.ppu->oam[addr - 0xFE00] : 0xFF;
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
	// OAM DMA bus conflict — CPU writes on the DMA source's bus are lost
	// (BullyGB "DMA allows RAM writes"). MBC register writes below $8000
	// still land: the cart latches them off the address/data lines.
	if (m.dma_active && !m.dma_vram_bypass && addr >= 0x8000 && addr < 0xFE00 &&
	    DmaBusOf(m, m.dma_src) == DmaBusOf(m, addr))
		return;
	if (addr < 0x8000)
	{
		if (m.cart) MbcWrite(*m.cart, addr, value);
		return;
	}
	if (m.cart && m.cart->mbc.type == MbcType::SachenMMC1)
	{
		MbcNotifyHighWrite(m.cart->mbc, addr, value);
	}
	if (addr < 0xA000)
	{
		if (m.ppu)
		{
			if (VramBlocked(m, true)) return;
			m.ppu->vram[(addr - 0x8000) + VramBankBase(m)] = value;
			m.ppu->vram_writes++;
		}
		return;
	}
	if (addr < 0xC000)
	{
		if (m.cart) MbcWrite(*m.cart, addr, value);
		return;
	}
	if (addr < 0xD000)
	{
		m.wram[addr - 0xC000] = value;
		return;
	}
	if (addr < 0xE000)
	{
		m.wram[WramBankBase(m) + (addr - 0xD000)] = value;
		return;
	}
	if (addr < 0xF000)
	{
		m.wram[addr - 0xE000] = value;
		return;
	}
	if (addr < 0xFE00)
	{
		m.wram[WramBankBase(m) + (addr - 0xF000)] = value;
		return;
	}
	if (addr < 0xFF00)
	{
		if (!m.dma_active && AccessedOamRow(m) >= 0)
		{
			// The scan's row reads finish a few dots before mode 2 ends; a
			// store landing after that goes through normally.
			static const int kw = AcidKnob("ACID_KW", 76);
			if (m.ppu->mode_clock < kw)
			{
				OamBugWriteCorrupt(m);
				return;
			}
		}
		if (addr >= 0xFEA0) return;      // unusable
		if (m.dma_active) return;        // OAM writes lost during OAM DMA
		if (m.ppu && !OamWriteBlocked(m))
			m.ppu->oam[addr - 0xFE00] = value;
		return;
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
		case 0xFF02: return static_cast<uint8_t>((m.serial_control & 0x81) | 0x7E);
		case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
			return m.timer ? TimerRead(*m.timer, addr) : 0xFF;
		case 0xFF0F: return static_cast<uint8_t>(m.if_ | 0xE0);
		case 0xFF46: return m.dma_last;
		case 0xFF4D:
			return (m.ppu && m.ppu->cgb)
				? static_cast<uint8_t>((m.double_speed ? 0x80 : 0x00) |
				                       (m.key1_armed   ? 0x01 : 0x00) | 0x7E)
				: 0xFF;
		case 0xFF4F:
			return (m.ppu && m.ppu->cgb) ? static_cast<uint8_t>(m.ppu->vbk | 0xFE) : 0xFF;
		case 0xFF55:
			return (m.ppu && m.ppu->cgb) ? m.hdma5 : 0xFF;
		case 0xFF68:
			return (m.ppu && m.ppu->cgb) ? static_cast<uint8_t>(m.ppu->bcps | 0x40) : 0xFF;
		case 0xFF69:
			return (m.ppu && m.ppu->cgb) ? m.ppu->bg_pal[m.ppu->bcps & 0x3F] : 0xFF;
		case 0xFF6A:
			return (m.ppu && m.ppu->cgb) ? static_cast<uint8_t>(m.ppu->ocps | 0x40) : 0xFF;
		case 0xFF6B:
			return (m.ppu && m.ppu->cgb) ? m.ppu->obj_pal[m.ppu->ocps & 0x3F] : 0xFF;
		case 0xFF70:
			return (m.ppu && m.ppu->cgb) ? static_cast<uint8_t>(m.svbk | 0xF8) : 0xFF;
		case 0xFF72:
			return (m.ppu && m.ppu->cgb) ? m.ff72 : 0xFF;
		case 0xFF73:
			return (m.ppu && m.ppu->cgb) ? m.ff73 : 0xFF;
		case 0xFF74:
			return (m.ppu && m.ppu->cgb) ? m.ff74 : 0xFF;
		case 0xFF75:
			return (m.ppu && m.ppu->cgb) ? static_cast<uint8_t>(m.ff75 | 0x8F) : 0xFF;
		case 0xFF76:
			return (m.ppu && m.ppu->cgb && m.apu) ? ApuReadPcm12(*m.apu) : 0xFF;
		case 0xFF77:
			return (m.ppu && m.ppu->cgb && m.apu) ? ApuReadPcm34(*m.apu) : 0xFF;
	}
	if (addr >= 0xFF10 && addr <= 0xFF3F)
	{
		return m.apu ? ApuRead(*m.apu, addr, m.ppu && m.ppu->cgb) : 0xFF;
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
			m.serial_control = value;
			// Internal clock (bit 0 = 1) completes instantly with no peer:
			// push the byte to the observer callback and fire the serial IRQ,
			// then clear the start bit. External clock (bit 0 = 0) has no
			// partner clocking bits in, so bit 7 stays set and no IRQ fires —
			// matching real DMG with a disconnected link cable. Games like
			// Tetris Plus rely on this silence to detect "no link partner".
			// SB latches $FF: disconnected MISO floats high, so each clock
			// shifts in a 1. Alleyway's serial-IRQ input loop depends on this.
			if ((value & 0x81) == 0x81)
			{
				if (m.serial_cb) m.serial_cb(m.serial_user, m.serial_data);
				m.serial_bits  = 8;  // clocked off DIV bit 8 in TimerStep
				m.serial_guard = 0;
			}
			else
				m.serial_bits = 0;
			return;
		case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
			if (m.timer) TimerWrite(*m.timer, m, addr, value);
			return;
		case 0xFF0F:
			m.if_ = static_cast<uint8_t>((value & 0x1F) | 0xE0);
			return;
		case 0xFF46:
		{
			m.dma_last = value;
			// Echo-fold the page so the bus-conflict test and source reads
			// see the address the hardware actually drives.
			uint16_t src = static_cast<uint16_t>(value << 8);
			if (src >= 0xE000) src = static_cast<uint16_t>(src - 0x2000);
			m.dma_src_next = src;
			// Goes live after one free M-cycle (DmaTickM counts this down);
			// a transfer already running keeps copying until then.
			m.dma_setup = 2; /*DMASETUP*/
			return;
		}
		case 0xFF50:
			// Boot-ROM disable: writing any non-zero value (canonically 0x01)
			// latches off the boot overlay so the cart bytes at 0x0000-0x00FF
			// become visible. Real hardware: bit 0 disables, can't be re-enabled.
			if (value != 0)
			{
				m.boot_rom_enabled = false;
				if (m.cart && m.cart->sachen_runs_raw) m.cart->mbc.sachen_locked = false;
			}
			return;
		case 0xFF4D:
			if (m.ppu && m.ppu->cgb) m.key1_armed = (value & 0x01) != 0;
			return;
		case 0xFF4F:
			if (m.ppu && m.ppu->cgb) m.ppu->vbk = value & 0x01;
			return;
		case 0xFF51: if (m.ppu && m.ppu->cgb) m.hdma1 = value; return;
		case 0xFF52: if (m.ppu && m.ppu->cgb) m.hdma2 = value; return;
		case 0xFF53: if (m.ppu && m.ppu->cgb) m.hdma3 = value; return;
		case 0xFF54: if (m.ppu && m.ppu->cgb) m.hdma4 = value; return;
		case 0xFF55: if (m.ppu && m.ppu->cgb) HdmaTrigger(m, value); return;
		case 0xFF68: if (m.ppu && m.ppu->cgb) m.ppu->bcps = value; return;
		case 0xFF69: if (m.ppu && m.ppu->cgb) CgbWritePalette(m.ppu->bg_pal, m.ppu->bcps, value, !CramBlocked(m)); return;
		case 0xFF6A: if (m.ppu && m.ppu->cgb) m.ppu->ocps = value; return;
		case 0xFF6B: if (m.ppu && m.ppu->cgb) CgbWritePalette(m.ppu->obj_pal, m.ppu->ocps, value, !CramBlocked(m)); return;
		case 0xFF70: if (m.ppu && m.ppu->cgb) m.svbk = value & 0x07; return;
		case 0xFF72: if (m.ppu && m.ppu->cgb) m.ff72 = value; return;
		case 0xFF73: if (m.ppu && m.ppu->cgb) m.ff73 = value; return;
		case 0xFF74: if (m.ppu && m.ppu->cgb) m.ff74 = value; return;
		case 0xFF75: if (m.ppu && m.ppu->cgb) m.ff75 = static_cast<uint8_t>(value & 0x70); return;
	}
	if (addr >= 0xFF10 && addr <= 0xFF3F)
	{
		if (m.apu) ApuWrite(*m.apu, addr, value, m.ppu && m.ppu->cgb,
		                    m.timer ? m.timer->div_counter : 0, m.double_speed);
		return;
	}
	if (addr >= 0xFF40 && addr <= 0xFF4B)
	{
		if (m.ppu) PpuWriteReg(*m.ppu, m, addr, value);
		return;
	}
	// Remaining I/O addresses (CGB regs, etc.) are ignored.
}

static void DoGdma(Memory &m, uint16_t src, uint16_t dst, uint16_t blocks)
{
	const uint32_t n = static_cast<uint32_t>(blocks) * 0x10u;
	m.dma_vram_bypass = true;
	for (uint32_t i = 0; i < n; ++i)
	{
		const uint8_t b = MemRead(m, static_cast<uint16_t>(src + i));
		MemWrite(m, static_cast<uint16_t>(0x8000 + ((dst + i) & 0x1FFF)), b);
	}
	m.dma_vram_bypass = false;
}

static void HdmaTransferBlock(Memory &m)
{
	m.dma_vram_bypass = true;
	for (uint32_t i = 0; i < 0x10; ++i)
	{
		const uint8_t b = MemRead(m, static_cast<uint16_t>(m.hdma_src + i));
		MemWrite(m, static_cast<uint16_t>(0x8000 + ((m.hdma_dst + i) & 0x1FFF)), b);
	}
	m.dma_vram_bypass = false;
	m.hdma_src = static_cast<uint16_t>(m.hdma_src + 0x10);
	m.hdma_dst = static_cast<uint16_t>(m.hdma_dst + 0x10);
	m.hdma1 = static_cast<uint8_t>(m.hdma_src >> 8);
	m.hdma2 = static_cast<uint8_t>(m.hdma_src & 0xF0);
	m.hdma3 = static_cast<uint8_t>(m.hdma_dst >> 8);
	m.hdma4 = static_cast<uint8_t>(m.hdma_dst & 0xF0);
	--m.hdma_len;
	if (m.hdma_len == 0)
	{
		m.hdma_active = false;
		m.hdma5       = 0xFF;
	}
	else
	{
		m.hdma5 = static_cast<uint8_t>((m.hdma_len - 1) & 0x7F);
	}
}

static void HdmaTrigger(Memory &m, uint8_t value)
{
	const uint16_t src    = static_cast<uint16_t>(((m.hdma1 << 8) | m.hdma2) & 0xFFF0);
	const uint16_t dst    = static_cast<uint16_t>(((m.hdma3 << 8) | m.hdma4) & 0x1FF0);
	const uint16_t blocks = static_cast<uint16_t>((value & 0x7F) + 1);

	// Every HDMA5 write reloads the step counter — including the pause
	// write, whose low bits + 1 become the new remaining count (SameBoy;
	// samesuite hdma_mode0/hdma_lcd_off read $80 after pausing with $00).
	if (value & 0x80)
	{
		m.hdma_src    = src;
		m.hdma_dst    = dst;
		m.hdma_len    = blocks;
		m.hdma_active = true;
		m.hdma5       = static_cast<uint8_t>(value & 0x7F);
		if (m.ppu && !(m.ppu->lcdc & 0x80))
		{
			HdmaTransferBlock(m);
		}
		else if (m.ppu && m.ppu->mode == PpuMode::HBlank &&
		         !m.hdma_hblank_latch)
		{
			HdmaTransferBlock(m);
			m.hdma_hblank_latch = true;
		}
	}
	else if (m.hdma_active)
	{
		m.hdma_active = false;
		m.hdma_len    = blocks;
		m.hdma5       = static_cast<uint8_t>(0x80 | ((m.hdma_len - 1) & 0x7F));
	}
	else
	{
		DoGdma(m, src, dst, blocks);
		const uint16_t end_src = static_cast<uint16_t>(src + blocks * 0x10);
		const uint16_t end_dst = static_cast<uint16_t>(dst + blocks * 0x10);
		m.hdma1 = static_cast<uint8_t>(end_src >> 8);
		m.hdma2 = static_cast<uint8_t>(end_src & 0xF0);
		m.hdma3 = static_cast<uint8_t>(end_dst >> 8);
		m.hdma4 = static_cast<uint8_t>(end_dst & 0xF0);
		m.hdma5 = 0xFF;
	}
}

bool MemLcdcPartial(Memory &m, uint8_t value, uint8_t *partial)
{
	if (!m.ppu || m.ppu->cgb) return false;
	const Ppu &p = *m.ppu;
	uint8_t old = p.lcdc;
	if (!(value & 0x02) && (p.om.pos == 0 || p.om.during_obj || p.tm.during_obj))
		old = static_cast<uint8_t>(old & ~0x02);
	*partial = static_cast<uint8_t>(old | (value & 0x01));
	return true;
}

void MemHdmaHBlank(Memory &m)
{
	m.hdma_hblank_latch = false;
	if (!m.hdma_active) return;
	HdmaTransferBlock(m);
	m.hdma_hblank_latch = true;
}

void MemHdmaLcdOff(Memory &m)
{
	if (!m.hdma_active) return;
	HdmaTransferBlock(m);
}

} // namespace SGB
