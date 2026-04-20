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

#include <cstring>

namespace SGB {

struct Emulator::Impl
{
	Cpu    cpu;
	Memory mem;
	Ppu    ppu;
	Apu    apu;
	Timer  timer;
	Joypad joypad;
	Cart   cart;

	RunMode     run_mode = RunMode::SGB;
	FrameBuffer fb{};
	bool        has_rom = false;
	float       clock_mul = 1.0f;
};

Emulator::Emulator() : impl_(new Impl) {}

Emulator::~Emulator() { delete impl_; }

bool Emulator::Init()
{
	Reset();
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
	// P7 drives this once per snes9x frame. For now just tick the stubs.
	if (!impl_->has_rom) return;
	impl_->cpu.Step(impl_->mem);
	PpuStep(impl_->ppu, impl_->mem, 4);
	ApuStep(impl_->apu, 4);
	TimerStep(impl_->timer, impl_->mem, 4);
}

void Emulator::RunCycles(int32_t /*tcycles*/) {}

const FrameBuffer &Emulator::GetFrameBuffer() const { return impl_->fb; }

int32_t Emulator::DrainAudio(int16_t *out, int32_t max_samples)
{
	return ApuDrain(impl_->apu, out, max_samples);
}

int32_t Emulator::GetAudioSampleRate() const
{
	// Matches snes9x's default SoundPlaybackRate; P4 may choose to resample differently.
	return 32000;
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

void Emulator::OnJoyserWrite(uint8_t /*value*/)
{
	// P6a hooks the SGB command-packet sniffer here.
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

bool S9xSGBLoadROM(const char * /*filename*/)
{
	// P7 hooks file I/O (reuses snes9x's loader infrastructure to unzip etc).
	return false;
}
