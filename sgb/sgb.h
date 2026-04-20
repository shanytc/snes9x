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

	// Consume pending audio samples. Returns the count written
	// (stereo frames — each frame stores L, R into out[2*i], out[2*i+1]).
	int32_t DrainAudio(int16_t *out, int32_t max_samples);
	int32_t GetAudioSampleRate() const;

	// Count of int16 values (NOT stereo frames) currently ready in the
	// ring buffer. Matches the convention S9xGetSampleCount uses —
	// useful when driving snes9x's host audio pull path.
	int32_t GetAudioSamplesAvailable() const;

	// Configure the APU's downsample target in Hz. Takes effect on the
	// next Reset(), or immediately for the next sample emitted.
	void    SetAudioRate(int32_t rate_hz);

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

	// Internal — routes a decoded SGB command into the palette/attribute
	// state. Exposed because the packet callback trampoline needs it.
	void  OnSgbCommandInternal(uint8_t cmd, const uint8_t *data, uint32_t len);

	// Composite the SGB border + GB screen into a 256 × 224 BGR555
	// buffer. `pitch_pixels` is the stride in uint16_t units (use 256
	// for a tightly-packed buffer, or the SNES GFX.Screen's PPL).
	void  BlitScreen(uint16_t *dest, uint32_t pitch_pixels);

	// Opaque implementation struct — forward-declared so file-local
	// helpers in sgb.cpp can reference the full type. External code
	// has no way to obtain one.
	struct Impl;

private:
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

// Load a GB ROM from an in-memory buffer. Callers that already have the
// bytes (e.g. after snes9x's FileLoader has unzipped a .zip/.jma/.7z
// container) use this to skip the stdio re-read.
bool S9xSGBLoadROMBytes(const unsigned char *data, size_t size, const char *path_for_sram);
void S9xSGBRunFrame(void);
void S9xSGBSetJoypad(uint16_t snes_pad_mask);
void S9xSGBOnJoyserWrite(uint8_t value);
bool S9xSGBIsActive(void);

// Composite the current frame (border + GB screen + mask mode) into a
// 256 × 224 BGR555 buffer. `pitch_pixels` is the stride in uint16_t
// units. Call after S9xSGBRunFrame.
void S9xSGBBlitScreen(uint16_t *dest, uint32_t pitch_pixels);

// Audio bridge — match snes9x's S9xGetSampleCount / S9xMixSamples
// contract. `count_int16s` is the number of int16 samples (stereo frame
// × 2), returning the number actually drained.
int32_t S9xSGBGetSampleCount(void);
int32_t S9xSGBDrainSamples(int16_t *dest, int32_t count_int16s);
void    S9xSGBSetAudioRate(int32_t rate_hz);

// Timing knobs — push Settings.GameBoyRunMode and GBClockMultiplier
// into the emulator from snes9x's per-frame dispatch. Both take effect
// from the next RunFrame forward; SetRunMode does NOT implicitly reset
// so applying it mid-game is harmless (but won't rewrite boot-state
// registers until the next Reset).
void    S9xSGBSetRunMode(uint8_t mode /* 0=DMG, 1=SGB1, 2=SGB2 */);
void    S9xSGBSetClockMultiplier(float mul);

// Save-state bridge — snapshot.cpp branches on Settings.SuperGameBoy
// and writes/reads the SGB state directly to/from the save file.
// Format: 4-byte "SGB!" magic + version + payload size + opaque blob.
size_t  S9xSGBStateSize(void);
void    S9xSGBStateSave(uint8_t *buffer);
bool    S9xSGBStateLoad(const uint8_t *buffer, size_t size);

// Convenience file I/O wrappers — write the full blob to `filename`,
// or read it back. Return false on I/O error or format mismatch.
bool    S9xSGBSaveStateToFile(const char *filename);
bool    S9xSGBLoadStateFromFile(const char *filename);

#endif
