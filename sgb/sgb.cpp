/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "sgb.h"

#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_cart.h"
#include "sgb_packet.h"
#include "sgb_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace SGB {

struct Emulator::Impl
{
	Cpu         cpu;
	Memory      mem;
	Ppu         ppu;
	Apu         apu;
	Timer       timer;
	Joypad      joypad;
	Cart        cart;
	PacketState sgb_pkt;
	SgbState    sgb_state;

	RunMode     run_mode  = RunMode::SGB;
	FrameBuffer fb{};
	bool        has_rom   = false;
	float       clock_mul = 1.0f;
};

// File-local trampoline — lets the process-global SgbCommandCallback
// forward into the singleton Emulator's Impl.
static void SgbCommandTrampoline(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	Instance().OnSgbCommandInternal(cmd, data, len);
}

Emulator::Emulator() : impl_(new Impl) {}

Emulator::~Emulator() { delete impl_; }

bool Emulator::Init()
{
	Reset();
	SetSgbCommandCallback(&SgbCommandTrampoline);
	return true;
}

void Emulator::Deinit()
{
	UnloadROM();
}

void Emulator::Reset()
{
	impl_->cpu.Reset();
	MemReset(impl_->mem);
	PpuReset(impl_->ppu);
	ApuReset(impl_->apu);
	TimerReset(impl_->timer);
	JoypadReset(impl_->joypad);
	PacketReset(impl_->sgb_pkt);
	SgbReset(impl_->sgb_state);

	impl_->mem.ppu    = &impl_->ppu;
	impl_->mem.apu    = &impl_->apu;
	impl_->mem.timer  = &impl_->timer;
	impl_->mem.joypad = &impl_->joypad;
	impl_->mem.cart   = &impl_->cart;

	impl_->fb.pixels = impl_->ppu.framebuffer;
	impl_->fb.width  = GB_SCREEN_WIDTH;
	impl_->fb.height = GB_SCREEN_HEIGHT;
	impl_->fb.pitch  = GB_SCREEN_WIDTH;
}

bool Emulator::LoadROM(const uint8_t *data, size_t size, const char *path)
{
	if (!CartLoad(impl_->cart, data, size, path))
		return false;
	impl_->has_rom = true;
	Reset();
	return true;
}

void Emulator::UnloadROM()
{
	if (impl_->has_rom) CartSaveBattery(impl_->cart);
	CartUnload(impl_->cart);
	impl_->has_rom = false;
}

bool Emulator::HasROM() const { return impl_->has_rom; }

void Emulator::SetRunMode(RunMode m) { impl_->run_mode = m; }
RunMode Emulator::GetRunMode() const { return impl_->run_mode; }

void Emulator::RunFrame()
{
	if (!impl_->has_rom) return;

	impl_->ppu.frame_ready = false;

	// Run CPU steps, ticking every subsystem by the same number of
	// T-cycles the CPU actually consumed, until the PPU signals VBlank.
	// A nominal GB frame is 70224 T-cycles; safety cap at ~3× that to
	// prevent a broken ROM from hanging the emulator thread.
	int32_t safety = 250000;

	while (!impl_->ppu.frame_ready && safety > 0)
	{
		const int64_t pre_t = impl_->cpu.State().t_cycles;
		impl_->cpu.Step(impl_->mem);
		int32_t consumed = static_cast<int32_t>(
			impl_->cpu.State().t_cycles - pre_t);
		if (consumed <= 0) consumed = 4;

		PpuStep  (impl_->ppu,   impl_->mem, consumed);
		TimerStep(impl_->timer, impl_->mem, consumed);
		ApuStep  (impl_->apu,                consumed);

		safety -= consumed;
	}
}

void Emulator::RunCycles(int32_t tcycles)
{
	if (!impl_->has_rom) return;

	// Partial-frame advance. Used when a host driving its own clock wants
	// to interleave GB execution with other work at finer granularity
	// than one frame. Same per-step ticking as RunFrame.
	int32_t remaining = tcycles;
	while (remaining > 0)
	{
		const int64_t pre_t = impl_->cpu.State().t_cycles;
		impl_->cpu.Step(impl_->mem);
		int32_t consumed = static_cast<int32_t>(
			impl_->cpu.State().t_cycles - pre_t);
		if (consumed <= 0) consumed = 4;

		PpuStep  (impl_->ppu,   impl_->mem, consumed);
		TimerStep(impl_->timer, impl_->mem, consumed);
		ApuStep  (impl_->apu,                consumed);

		remaining -= consumed;
	}
}

const FrameBuffer &Emulator::GetFrameBuffer() const { return impl_->fb; }

void Emulator::BlitScreen(uint16_t *dest, uint32_t pitch_pixels)
{
	if (!impl_->has_rom || !dest) return;

	// Stage border into a packed 256 × 224 buffer. Border leaves the
	// centered 20 × 18 tile area untouched — we overwrite that next.
	uint16_t staging[SGB_BORDER_W * SGB_BORDER_H];
	SgbRenderBorder(impl_->sgb_state, staging);

	// Pick the source pixels for the GB screen area based on MASK_EN.
	const uint8_t *src_fb = impl_->ppu.framebuffer;
	if (impl_->sgb_state.mask_mode == SGB_MASK_FREEZE &&
	    impl_->sgb_state.frozen_frame_valid)
	{
		src_fb = impl_->sgb_state.frozen_frame;
	}

	const uint32_t origin_x = SGB_GB_TILE_X * 8;  // 48
	const uint32_t origin_y = SGB_GB_TILE_Y * 8;  // 40

	for (uint32_t py = 0; py < GB_SCREEN_HEIGHT; ++py)
	{
		const uint32_t dst_y     = origin_y + py;
		uint16_t *const dst_row  = staging + dst_y * SGB_BORDER_W + origin_x;
		for (uint32_t px = 0; px < GB_SCREEN_WIDTH; ++px)
		{
			uint16_t color;
			switch (impl_->sgb_state.mask_mode)
			{
				case SGB_MASK_BLACK:
					color = 0x0000;
					break;
				case SGB_MASK_BLANK:
					color = impl_->sgb_state.active[0].colors[0];
					break;
				default:
				{
					const uint8_t  shade   = src_fb[py * GB_SCREEN_WIDTH + px];
					const uint32_t tile_x  = px / 8;
					const uint32_t tile_y  = py / 8;
					color = SgbResolveColor(impl_->sgb_state, tile_x, tile_y, shade);
					break;
				}
			}
			dst_row[px] = color;
		}
	}

	// Copy to destination with pitch. For a packed 256-wide dest this
	// degenerates into a flat memcpy per row.
	for (uint32_t y = 0; y < SGB_BORDER_H; ++y)
	{
		std::memcpy(dest + y * pitch_pixels,
		            staging + y * SGB_BORDER_W,
		            SGB_BORDER_W * sizeof(uint16_t));
	}
}

int32_t Emulator::DrainAudio(int16_t *out, int32_t max_samples)
{
	return ApuDrain(impl_->apu, out, max_samples);
}

int32_t Emulator::GetAudioSampleRate() const
{
	return impl_->apu.output_rate;
}

void Emulator::SetJoypad(uint16_t snes_pad_mask)
{
	// SNES->GB button mapping. B/Y map to A/B (SNES has extra shoulders & face buttons).
	uint8_t gb = 0;
	if (snes_pad_mask & (1 << 15)) gb |= GB_A;       // SNES B  → GB A
	if (snes_pad_mask & (1 << 14)) gb |= GB_B;       // SNES Y  → GB B
	if (snes_pad_mask & (1 << 12)) gb |= GB_START;
	if (snes_pad_mask & (1 << 13)) gb |= GB_SELECT;
	if (snes_pad_mask & (1 << 11)) gb |= GB_UP;
	if (snes_pad_mask & (1 << 10)) gb |= GB_DOWN;
	if (snes_pad_mask & (1 <<  9)) gb |= GB_LEFT;
	if (snes_pad_mask & (1 <<  8)) gb |= GB_RIGHT;
	JoypadSet(impl_->joypad, impl_->mem, gb);
}

void Emulator::OnJoyserWrite(uint8_t value)
{
	// Only meaningful when the cart declares SGB features. Feeding always
	// is harmless (non-SGB games don't produce RESET pulses) but we gate
	// on run_mode anyway so the packet state doesn't accumulate noise.
	if (impl_->run_mode == RunMode::DMG) return;
	PacketFeed(impl_->sgb_pkt, value);
}

void Emulator::OnSgbCommandInternal(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	// *_TRN commands source their 4KB from GB VRAM $8000..$8FFF — our
	// first half of Ppu::vram. MASK_EN freeze also needs the current
	// GB framebuffer. Non-consuming commands ignore both pointers.
	SgbHandleCommand(impl_->sgb_state, cmd, data, len,
	                 impl_->ppu.vram,
	                 impl_->ppu.framebuffer);
}

size_t Emulator::StateSize() const { return 0; }
void   Emulator::StateSave(uint8_t * /*buffer*/) const {}
bool   Emulator::StateLoad(const uint8_t * /*buffer*/, size_t /*size*/) { return false; }

void  Emulator::SetClockMultiplier(float m) { impl_->clock_mul = m; }
float Emulator::GetClockMultiplier() const  { return impl_->clock_mul; }

Emulator &Instance()
{
	static Emulator g;
	return g;
}

} // namespace SGB

// C-style facade used by snes9x integration code.
bool S9xSGBInit(void)               { return SGB::Instance().Init(); }
void S9xSGBDeinit(void)             { SGB::Instance().Deinit(); }
void S9xSGBReset(void)              { SGB::Instance().Reset(); }
bool S9xSGBIsActive(void)           { return SGB::Instance().HasROM(); }
void S9xSGBRunFrame(void)           { SGB::Instance().RunFrame(); }
void S9xSGBSetJoypad(uint16_t m)    { SGB::Instance().SetJoypad(m); }
void S9xSGBOnJoyserWrite(uint8_t v) { SGB::Instance().OnJoyserWrite(v); }

void S9xSGBBlitScreen(uint16_t *dest, uint32_t pitch_pixels)
{
	SGB::Instance().BlitScreen(dest, pitch_pixels);
}

bool S9xSGBLoadROM(const char *filename)
{
	if (!filename || !*filename) return false;

	std::FILE *f = std::fopen(filename, "rb");
	if (!f) return false;

	if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
	const long sz = std::ftell(f);
	if (sz <= 0 || sz > 16 * 1024 * 1024)   // 16 MB is the MBC5/MBC6 ceiling
	{ std::fclose(f); return false; }
	std::fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	const size_t got = std::fread(buf.data(), 1, static_cast<size_t>(sz), f);
	std::fclose(f);
	if (got != static_cast<size_t>(sz)) return false;

	return SGB::Instance().LoadROM(buf.data(), buf.size(), filename);
}
