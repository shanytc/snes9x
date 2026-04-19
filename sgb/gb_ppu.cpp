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

} // namespace SGB
