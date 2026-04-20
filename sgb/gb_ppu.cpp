/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_ppu.h"
#include "gb_memory.h"

#include <cstring>

namespace SGB {

void PpuReset(Ppu &p)
{
	std::memset(p.vram, 0, sizeof p.vram);
	std::memset(p.oam,  0, sizeof p.oam);
	std::memset(p.framebuffer, 0, sizeof p.framebuffer);
	p.lcdc = 0x91;
	p.stat = 0x85;
	p.scy = p.scx = p.ly = p.lyc = 0;
	p.bgp = 0xFC; p.obp0 = 0xFF; p.obp1 = 0xFF;
	p.wy = p.wx = 0;
	p.mode = PpuMode::OamScan;
	p.mode_clock = 0;
	p.frame_ready = false;
}

void PpuStep(Ppu & /*p*/, Memory & /*mem*/, int32_t /*tcycles*/)
{
	// P3 wires up mode state machine and renderer.
}

uint8_t PpuReadReg(const Ppu &p, uint16_t addr)
{
	switch (addr)
	{
		case 0xFF40: return p.lcdc;
		case 0xFF41: return static_cast<uint8_t>(p.stat | 0x80);  // bit 7 always 1
		case 0xFF42: return p.scy;
		case 0xFF43: return p.scx;
		case 0xFF44: return p.ly;
		case 0xFF45: return p.lyc;
		// 0xFF46 DMA is handled in gb_memory.cpp (writes only — reads return last-write).
		case 0xFF47: return p.bgp;
		case 0xFF48: return p.obp0;
		case 0xFF49: return p.obp1;
		case 0xFF4A: return p.wy;
		case 0xFF4B: return p.wx;
	}
	return 0xFF;
}

void PpuWriteReg(Ppu &p, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF40: p.lcdc = value; break;
		case 0xFF41:
			// Lower 3 bits are mode/coincidence (read-only); top 4 are IRQ enables.
			p.stat = static_cast<uint8_t>((p.stat & 0x07) | (value & 0x78));
			break;
		case 0xFF42: p.scy = value; break;
		case 0xFF43: p.scx = value; break;
		case 0xFF44: p.ly = 0; break;         // LY is reset on any write per Pan Docs
		case 0xFF45: p.lyc = value; break;
		case 0xFF47: p.bgp  = value; break;
		case 0xFF48: p.obp0 = value; break;
		case 0xFF49: p.obp1 = value; break;
		case 0xFF4A: p.wy = value; break;
		case 0xFF4B: p.wx = value; break;
	}
}

} // namespace SGB
