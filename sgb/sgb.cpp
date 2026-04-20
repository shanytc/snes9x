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

	// 256×224 composite staging buffer — heap-resident to keep a
	// ~112 KB allocation off the stack of whatever thread drives
	// S9xMainLoop (snes9x's per-thread stacks aren't always large).
	uint16_t    composite[SGB_BORDER_W * SGB_BORDER_H];

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

	// Apply run-mode specific post-boot register values. The SGB BIOS
	// hands control to the cart with slightly different register state
	// than a DMG boot ROM does — some games (notably Donkey Kong and
	// Pokemon) check these to detect whether they're running on a real
	// SGB host.
	CpuState &cs = impl_->cpu.State();
	switch (impl_->run_mode)
	{
		case RunMode::SGB:
			cs.r.af = 0x0100;
			cs.r.bc = 0x0014;
			cs.r.de = 0x0000;
			cs.r.hl = 0xC060;
			break;
		case RunMode::SGB2:
			cs.r.af = 0xFF00;
			cs.r.bc = 0x0014;
			cs.r.de = 0x0000;
			cs.r.hl = 0xC060;
			break;
		case RunMode::DMG:
		default:
			// gb_cpu.cpp Reset() already set DMG values.
			break;
	}
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

	// Cycle budget per SNES frame depends on run mode and the user
	// overclock/underclock knob:
	//   SGB1: SNES master clock / 5 = 4.2955 MHz (2.4% faster than real GB)
	//   SGB2: exact GB clock         = 4.1943 MHz
	//   DMG:  exact GB clock         = 4.1943 MHz
	// At NTSC SNES refresh (60.099 fps) that's ~71485 / 69801 T-cycles per
	// SNES frame. Clamp the multiplier to a sane range so users can't
	// accidentally freeze the emulator with a 0x or 1000x setting.
	constexpr double SNES_FPS = 60.09881389744051;
	double base_hz;
	switch (impl_->run_mode)
	{
		case RunMode::SGB:  base_hz = 21477272.727272 / 5.0; break;
		case RunMode::SGB2: base_hz = 4194304.0;             break;
		case RunMode::DMG:
		default:            base_hz = 4194304.0;             break;
	}

	float mul = impl_->clock_mul;
	if (mul < 0.10f) mul = 0.10f;
	if (mul > 8.00f) mul = 8.00f;

	const double   per_frame = (base_hz / SNES_FPS) * static_cast<double>(mul);
	const int32_t  budget    = static_cast<int32_t>(per_frame);

	RunCycles(budget);
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

void Emulator::GetStatus(char *buf, size_t cap) const
{
	if (!buf || cap == 0) return;
	const CpuState &s = impl_->cpu.State();
	std::snprintf(buf, cap,
	              "SGB PC=%04X SP=%04X A=%02X%s T=%lld ill=%u",
	              s.r.pc, s.r.sp, s.r.a,
	              s.halted ? " HALT" : (s.stopped ? " STOP" : ""),
	              static_cast<long long>(s.t_cycles),
	              static_cast<unsigned>(s.illegal_ops));
}

void Emulator::BlitScreen(uint16_t *dest, uint32_t pitch_pixels)
{
	if (!impl_->has_rom || !dest) return;

	// Stage border into our heap-resident 256 × 224 buffer. Border
	// leaves the centered 20 × 18 tile area untouched — we overwrite
	// that next with palette-resolved GB pixels.
	uint16_t *const staging = impl_->composite;
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

int32_t Emulator::GetAudioSamplesAvailable() const
{
	const uint32_t head = impl_->apu.sample_head;
	const uint32_t tail = impl_->apu.sample_tail;
	const uint32_t frames = (head >= tail)
		? (head - tail)
		: (APU_SAMPLE_BUF_SIZE - tail + head);
	// Each frame is one stereo pair = 2 int16 values.
	return static_cast<int32_t>(frames * 2);
}

void Emulator::SetAudioRate(int32_t rate_hz)
{
	ApuSetOutputRate(impl_->apu, rate_hz);
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

// ===================================================================
// State serialization
//
// Layout:
//   [0..3]   magic "SGB!"
//   [4..7]   version (u32 LE)
//   [8..11]  payload length (u32 LE)
//   [12..]   payload fields in Visit() order
//
// Version 1: initial format.
// ===================================================================

namespace {

constexpr uint32_t SGB_STATE_MAGIC   = 0x21424753u;  // 'S''G''B''!' LE
constexpr uint32_t SGB_STATE_VERSION = 1;

enum class IoMode : uint8_t { Size, Save, Load };

struct IoCtx
{
	uint8_t       *wbuf;
	const uint8_t *rbuf;
	size_t         pos;
	size_t         cap;
	IoMode         mode;
	bool           ok;
};

inline void IoBytes(IoCtx &c, void *data, size_t n)
{
	if (!c.ok) return;
	if (c.mode != IoMode::Size && c.pos + n > c.cap) { c.ok = false; return; }
	if      (c.mode == IoMode::Save) std::memcpy(c.wbuf + c.pos, data, n);
	else if (c.mode == IoMode::Load) std::memcpy(data, c.rbuf + c.pos, n);
	c.pos += n;
}

template <typename T>
inline void IoField(IoCtx &c, T &v)
{
	IoBytes(c, &v, sizeof v);
}

void VisitState(Emulator::Impl &impl, IoCtx &c)
{
	// ----- CPU -----
	IoField(c, impl.cpu.State());

	// ----- Memory (skip pointer fields — they're relinked after load) -----
	IoBytes(c, impl.mem.wram, sizeof impl.mem.wram);
	IoBytes(c, impl.mem.hram, sizeof impl.mem.hram);
	IoField(c, impl.mem.ie);
	IoField(c, impl.mem.if_);
	IoField(c, impl.mem.serial_data);

	// ----- Cart MBC + SRAM (ROM is static, not serialized) -----
	IoField(c, impl.cart.mbc);

	uint32_t sram_size = static_cast<uint32_t>(impl.cart.sram.size());
	IoField(c, sram_size);
	if (c.mode == IoMode::Load) impl.cart.sram.resize(sram_size);
	if (sram_size > 0) IoBytes(c, impl.cart.sram.data(), sram_size);

	// ----- PPU -----
	IoBytes(c, impl.ppu.vram, sizeof impl.ppu.vram);
	IoBytes(c, impl.ppu.oam,  sizeof impl.ppu.oam);
	IoField(c, impl.ppu.lcdc);
	IoField(c, impl.ppu.stat);
	IoField(c, impl.ppu.scy);
	IoField(c, impl.ppu.scx);
	IoField(c, impl.ppu.ly);
	IoField(c, impl.ppu.lyc);
	IoField(c, impl.ppu.bgp);
	IoField(c, impl.ppu.obp0);
	IoField(c, impl.ppu.obp1);
	IoField(c, impl.ppu.wy);
	IoField(c, impl.ppu.wx);
	IoField(c, impl.ppu.mode);
	IoField(c, impl.ppu.mode_clock);
	IoField(c, impl.ppu.window_line);
	IoField(c, impl.ppu.stat_line_high);
	IoBytes(c, impl.ppu.framebuffer,      sizeof impl.ppu.framebuffer);
	IoBytes(c, impl.ppu.scanline_bg_raw,  sizeof impl.ppu.scanline_bg_raw);
	IoField(c, impl.ppu.frame_ready);

	// ----- APU channels + master regs + frame sequencer -----
	// (sample buffer + accumulators are transient and reset on load.)
	IoField(c, impl.apu.ch1);
	IoField(c, impl.apu.ch2);
	IoField(c, impl.apu.ch3);
	IoField(c, impl.apu.ch4);
	IoField(c, impl.apu.nr50);
	IoField(c, impl.apu.nr51);
	IoField(c, impl.apu.master_enabled);
	IoField(c, impl.apu.frame_seq_timer);
	IoField(c, impl.apu.frame_seq_step);

	// ----- Timer / Joypad -----
	IoField(c, impl.timer);
	IoField(c, impl.joypad);

	// ----- SGB command layer -----
	IoField(c, impl.sgb_pkt);
	IoField(c, impl.sgb_state);

	// ----- Emulator-level config -----
	IoField(c, impl.run_mode);
	IoField(c, impl.clock_mul);
}

} // anonymous

size_t Emulator::StateSize() const
{
	IoCtx c{nullptr, nullptr, 0, 0, IoMode::Size, true};
	VisitState(const_cast<Impl &>(*impl_), c);
	// +12 for header: magic + version + payload_size.
	return c.pos + 12;
}

void Emulator::StateSave(uint8_t *buffer) const
{
	if (!buffer) return;

	// Compute payload size first.
	const size_t payload = StateSize() - 12;

	uint32_t magic   = SGB_STATE_MAGIC;
	uint32_t version = SGB_STATE_VERSION;
	uint32_t plen    = static_cast<uint32_t>(payload);
	std::memcpy(buffer + 0, &magic,   4);
	std::memcpy(buffer + 4, &version, 4);
	std::memcpy(buffer + 8, &plen,    4);

	IoCtx c{buffer + 12, nullptr, 0, payload, IoMode::Save, true};
	VisitState(const_cast<Impl &>(*impl_), c);
}

bool Emulator::StateLoad(const uint8_t *buffer, size_t size)
{
	if (!buffer || size < 12) return false;

	uint32_t magic, version, plen;
	std::memcpy(&magic,   buffer + 0, 4);
	std::memcpy(&version, buffer + 4, 4);
	std::memcpy(&plen,    buffer + 8, 4);

	if (magic != SGB_STATE_MAGIC)     return false;
	if (version != SGB_STATE_VERSION) return false;  // future: accept v<=current
	if (size < 12 + plen)             return false;

	IoCtx c{nullptr, buffer + 12, 0, plen, IoMode::Load, true};
	VisitState(*impl_, c);
	if (!c.ok) return false;

	// Relink Memory's pointer fields — they were serialized as garbage.
	impl_->mem.ppu    = &impl_->ppu;
	impl_->mem.apu    = &impl_->apu;
	impl_->mem.timer  = &impl_->timer;
	impl_->mem.joypad = &impl_->joypad;
	impl_->mem.cart   = &impl_->cart;

	// Reset transient audio output state — the ring buffer content is
	// not serialized because it's sub-frame scratch.
	impl_->apu.sample_accum_l   = 0;
	impl_->apu.sample_accum_r   = 0;
	impl_->apu.sample_accum_cnt = 0;
	impl_->apu.sample_head      = 0;
	impl_->apu.sample_tail      = 0;
	impl_->apu.sample_timer     = impl_->apu.cycles_per_sample;

	// Re-point the exposed FrameBuffer at the (now-restored) PPU fb.
	impl_->fb.pixels = impl_->ppu.framebuffer;
	impl_->fb.width  = GB_SCREEN_WIDTH;
	impl_->fb.height = GB_SCREEN_HEIGHT;
	impl_->fb.pitch  = GB_SCREEN_WIDTH;

	return true;
}

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

int32_t S9xSGBGetSampleCount(void)
{
	return SGB::Instance().GetAudioSamplesAvailable();
}

int32_t S9xSGBDrainSamples(int16_t *dest, int32_t count_int16s)
{
	if (!dest || count_int16s <= 0) return 0;
	const int32_t frames = count_int16s / 2;
	const int32_t got    = SGB::Instance().DrainAudio(dest, frames);
	return got * 2;
}

void S9xSGBSetAudioRate(int32_t rate_hz)
{
	SGB::Instance().SetAudioRate(rate_hz);
}

void S9xSGBSetRunMode(uint8_t mode)
{
	SGB::RunMode m;
	switch (mode)
	{
		case 2:  m = SGB::RunMode::SGB2; break;
		case 0:  m = SGB::RunMode::DMG;  break;
		case 1:
		default: m = SGB::RunMode::SGB;  break;
	}
	SGB::Instance().SetRunMode(m);
}

void S9xSGBSetClockMultiplier(float mul)
{
	SGB::Instance().SetClockMultiplier(mul);
}

size_t S9xSGBStateSize(void)
{
	return SGB::Instance().StateSize();
}

void S9xSGBStateSave(uint8_t *buffer)
{
	SGB::Instance().StateSave(buffer);
}

bool S9xSGBStateLoad(const uint8_t *buffer, size_t size)
{
	return SGB::Instance().StateLoad(buffer, size);
}

bool S9xSGBSaveStateToFile(const char *filename)
{
	if (!filename) return false;

	const size_t need = SGB::Instance().StateSize();
	std::vector<uint8_t> buf(need);
	SGB::Instance().StateSave(buf.data());

	FILE *f = fopen(filename, "wb");
	if (!f) return false;
	const size_t w = fwrite(buf.data(), 1, need, f);
	fclose(f);
	return w == need;
}

void S9xSGBGetStatus(char *buf, size_t cap)
{
	SGB::Instance().GetStatus(buf, cap);
}

bool S9xSGBLoadStateFromFile(const char *filename)
{
	if (!filename) return false;

	FILE *f = fopen(filename, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	const long sz = ftell(f);
	if (sz <= 12 || sz > 4 * 1024 * 1024) { fclose(f); return false; }
	fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	const size_t got = fread(buf.data(), 1, static_cast<size_t>(sz), f);
	fclose(f);
	if (got != static_cast<size_t>(sz)) return false;

	return SGB::Instance().StateLoad(buf.data(), buf.size());
}

bool S9xSGBLoadROMBytes(const unsigned char *data, size_t size, const char *path_for_sram)
{
	if (!data || size < 0x150) return false;
	return SGB::Instance().LoadROM(static_cast<const uint8_t *>(data), size, path_for_sram);
}

bool S9xSGBLoadROM(const char *filename)
{
	if (!filename || !*filename) return false;

	FILE *f = fopen(filename, "rb");
	if (!f) return false;

	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	const long sz = ftell(f);
	if (sz <= 0 || sz > 16 * 1024 * 1024)   // 16 MB is the MBC5/MBC6 ceiling
	{ fclose(f); return false; }
	fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	const size_t got = fread(buf.data(), 1, static_cast<size_t>(sz), f);
	fclose(f);
	if (got != static_cast<size_t>(sz)) return false;

	return SGB::Instance().LoadROM(buf.data(), buf.size(), filename);
}
