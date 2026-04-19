/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_H_
#define _SGB_H_

#include <cstdint>
#include <cstddef>

namespace SGB {

struct FrameBuffer
{
	const uint8_t *pixels;  // 160x144 color-indexed (4 shades / SGB palette idx)
	uint32_t       width;
	uint32_t       height;
	uint32_t       pitch;
};

enum class RunMode : uint8_t
{
	DMG  = 0,  // Plain Game Boy, monochrome
	SGB  = 1,  // SGB1 — 4.295 MHz GB CPU, SGB features enabled
	SGB2 = 2   // SGB2 — exact 4.194 MHz GB CPU
};

struct CpuState;
struct Memory;
struct Ppu;
struct Apu;
struct Timer;
struct Joypad;
struct Cart;
struct SgbState;

class Emulator
{
public:
	Emulator();
	~Emulator();

	Emulator(const Emulator &) = delete;
	Emulator &operator=(const Emulator &) = delete;

	bool Init();
	void Deinit();
	void Reset();

	bool LoadROM(const uint8_t *data, size_t size, const char *path = nullptr);
	void UnloadROM();
	bool HasROM() const;

	void SetRunMode(RunMode m);
	RunMode GetRunMode() const;

	// Advance the GB core by one whole video frame (ends at VBlank entry).
	void RunFrame();

	// Advance by N T-cycles. Used when snes9x drives the master clock directly.
	void RunCycles(int32_t tcycles);

	const FrameBuffer &GetFrameBuffer() const;

	// Consume pending audio samples. Returns the count written.
	int32_t DrainAudio(int16_t *out, int32_t max_samples);
	int32_t GetAudioSampleRate() const;

	// Mapped from a SNES joypad bitmask — see sgb.cpp for the mapping table.
	void SetJoypad(uint16_t snes_pad_mask);

	// Called by the SNES $4016 write path; feeds the SGB command-packet sniffer.
	void OnJoyserWrite(uint8_t value);

	// Snapshot format — versioned block appended to snes9x's .state.
	size_t StateSize() const;
	void   StateSave(uint8_t *buffer) const;
	bool   StateLoad(const uint8_t *buffer, size_t size);

	// 1.0 = real hardware speed. <1 underclocks (ARM targets), >1 overclocks.
	void  SetClockMultiplier(float m);
	float GetClockMultiplier() const;

private:
	struct Impl;
	Impl *impl_;
};

// Global singleton — snes9x keeps one active SGB core.
Emulator &Instance();

} // namespace SGB

// C-style facade used by snes9x integration code (matches S9x* naming).
bool S9xSGBInit(void);
void S9xSGBDeinit(void);
void S9xSGBReset(void);
bool S9xSGBLoadROM(const char *filename);
void S9xSGBRunFrame(void);
void S9xSGBSetJoypad(uint16_t snes_pad_mask);
void S9xSGBOnJoyserWrite(uint8_t value);
bool S9xSGBIsActive(void);

#endif
