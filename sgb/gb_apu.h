/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_APU_H_
#define _SGB_GB_APU_H_

#include <cstdint>

namespace SGB {

// GB APU — 4 channels, frame sequencer at 512 Hz, stereo output.
// Native sample rate (1 sample per T-cycle) is overkill; we downsample via
// snes9x's existing resampler to match the SNES sound output path.

struct ApuSquare
{
	uint16_t freq      = 0;
	uint8_t  duty      = 0;
	uint8_t  length    = 0;
	uint8_t  envelope  = 0;
	uint8_t  env_dir   = 0;
	uint8_t  env_period = 0;
	uint8_t  sweep_period = 0;
	uint8_t  sweep_dir    = 0;
	uint8_t  sweep_shift  = 0;
	bool     enabled   = false;
	uint16_t timer     = 0;
	uint8_t  duty_pos  = 0;
	uint8_t  volume    = 0;
};

struct ApuWave
{
	uint8_t  ram[16];  // 32 4-bit samples, packed
	uint16_t freq    = 0;
	uint8_t  length  = 0;
	uint8_t  level   = 0;
	bool     enabled = false;
	uint16_t timer   = 0;
	uint8_t  pos     = 0;
};

struct ApuNoise
{
	uint16_t lfsr      = 0x7FFF;
	uint8_t  length    = 0;
	uint8_t  envelope  = 0;
	uint8_t  env_dir   = 0;
	uint8_t  env_period = 0;
	uint8_t  clock_shift = 0;
	uint8_t  clock_div   = 0;
	bool     width_mode  = false;
	bool     enabled   = false;
	uint16_t timer     = 0;
	uint8_t  volume    = 0;
};

struct Apu
{
	ApuSquare ch1, ch2;
	ApuWave   ch3;
	ApuNoise  ch4;

	uint8_t   nr50 = 0, nr51 = 0, nr52 = 0;
	uint8_t   frame_step = 0;
	int32_t   frame_clock = 0;
	int32_t   sample_clock = 0;

	// Ring buffer of emitted stereo samples (L,R interleaved, int16).
	int16_t   sample_buf[4096 * 2];
	uint32_t  sample_head = 0;
	uint32_t  sample_tail = 0;
};

void ApuReset(Apu &a);
void ApuStep(Apu &a, int32_t tcycles);
uint8_t  ApuRead(Apu &a, uint16_t addr);
void     ApuWrite(Apu &a, uint16_t addr, uint8_t value);
int32_t  ApuDrain(Apu &a, int16_t *out, int32_t max_samples);

} // namespace SGB

#endif
