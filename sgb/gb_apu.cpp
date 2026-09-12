/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Game Boy APU. The channel timing/trigger/glitch machinery is a port of
// SameBoy's APU core (Core/apu.c — copyright (c) Lior Halphon, MIT license,
// full text in sgb/SAMEBOY-LICENSE.txt), reduced to the two models this
// core emulates: DMG-B for monochrome and CGB-E for color. That machinery
// is what the blargg dmg_sound/cgb_sound and SameSuite APU tests verify.
//
// Everything runs on the 2 MHz APU clock ("APU ticks", 2 T-cycles each):
//   - square channels step their duty units when sample_countdown expires
//     (reload = (freq ^ $7FF) * 2 + 1 ticks), aligned to 1 MHz via lf_div;
//   - the wave channel steps at the full 2 MHz (reload = freq ^ $7FF);
//   - the noise channel increments a free-running 14-bit counter and steps
//     its LFSR on rising edges of the NR43-selected counter bit;
//   - lengths/envelopes/sweep are driven by the DIV-APU divider events
//     (gb_timer.cpp fires ApuDivEvent on the falling edge of DIV bit 12 —
//     13 in double speed — and ApuDivSecondaryEvent on the rising edge).
//
// The output side (box-filter resampler, DC blocker, reconstruction
// low-pass, host ring buffer) is snes9x-specific and unchanged.

#include "gb_apu.h"

#include <algorithm>
#include <cstring>
#include <cmath>

namespace SGB {

namespace {

constexpr uint8_t DUTY_TABLE[4][8] = {
	{ 0, 0, 0, 0, 0, 0, 0, 1 },   // 12.5%
	{ 1, 0, 0, 0, 0, 0, 0, 1 },   // 25%
	{ 1, 0, 0, 0, 0, 1, 1, 1 },   // 50%
	{ 0, 1, 1, 1, 1, 1, 1, 0 }    // 75%
};

// -------------------------------------------------------------------
// Current digital channel outputs (0..15) — computed on demand for the
// mixer and the CGB PCM registers.
// -------------------------------------------------------------------

inline uint8_t SquareOut(const Apu &a, int idx)
{
	const ApuSquare &c = idx ? a.ch2 : a.ch1;
	if (!a.is_active[idx] || c.sample_surpressed) return 0;
	// A duty write only takes hold when the current sample step completes
	// (samesuite channel_x_duty_delay) — use the step-latched duty.
	return DUTY_TABLE[c.duty_latched][c.current_sample_index & 7] ? c.current_volume : 0;
}

inline uint8_t WaveOut(const Apu &a)
{
	if (!a.is_active[2]) return 0;
	const ApuWave &c = a.ch3;
	const uint8_t nibble = (c.current_sample_index & 1)
		? static_cast<uint8_t>(c.current_sample_byte & 0x0F)
		: static_cast<uint8_t>(c.current_sample_byte >> 4);
	return static_cast<uint8_t>(nibble >> c.shift);
}

inline uint8_t NoiseOut(const Apu &a)
{
	if (!a.is_active[3]) return 0;
	return a.ch4.current_lfsr_sample ? a.ch4.current_volume : 0;
}

inline uint8_t ChannelOut(const Apu &a, int ch)
{
	switch (ch)
	{
		case 0: case 1: return SquareOut(a, ch);
		case 2:         return WaveOut(a);
		default:        return NoiseOut(a);
	}
}

inline bool DacEnabled(const Apu &a, int ch)
{
	switch (ch)
	{
		case 0: return (a.ch1.nrx2 & 0xF8) != 0;
		case 1: return (a.ch2.nrx2 & 0xF8) != 0;
		case 2: return a.ch3.enable;
		default: return (a.ch4.nr42 & 0xF8) != 0;
	}
}

// A powered DAC maps digital 0..15 to a bipolar -15..+15 (non-zero DC
// even at digital 0 — digitized-voice players depend on it). A depowered
// DAC contributes nothing.
inline int32_t DacAnalog(bool dac_enabled, uint8_t digital)
{
	return dac_enabled ? (2 * static_cast<int32_t>(digital) - 15) : 0;
}

// -------------------------------------------------------------------
// Envelope clock (FOSY/FYNO) — see gb_apu.h.
// -------------------------------------------------------------------

inline void SetEnvelopeClock(ApuEnvClock &clock, bool value, bool direction, uint8_t volume)
{
	if (clock.clock == value) return;
	if (value)
	{
		clock.clock       = true;
		clock.should_lock = (volume == 0xF && direction) || (volume == 0x0 && !direction);
	}
	else
	{
		clock.clock   = false;
		clock.locked  = clock.locked || clock.should_lock;
	}
}

void Nrx2GlitchOne(uint8_t &volume, uint8_t value, uint8_t old_value,
                   uint8_t &countdown, ApuEnvClock &lock)
{
	if (lock.clock)
		countdown = value & 7;

	bool should_tick   = (value & 7) && !(old_value & 7) && !lock.locked;
	const bool should_invert = ((value & 8) ^ (old_value & 8)) != 0;

	if ((value & 0xF) == 8 && (old_value & 0xF) == 8 && !lock.locked)
		should_tick = true;

	if (should_invert)
	{
		if (value & 8)
		{
			if (!(old_value & 7) && !lock.locked)
				volume ^= 0xF;
			else
				volume = static_cast<uint8_t>((0xE - volume) & 0xF);
			should_tick = false;
		}
		else
		{
			volume = static_cast<uint8_t>((0x10 - volume) & 0xF);
		}
	}
	if (should_tick)
	{
		if (value & 8) ++volume;
		else           --volume;
		volume &= 0xF;
	}
	else if (!(value & 7) && lock.clock)
	{
		SetEnvelopeClock(lock, false, 0, 0);
	}
}

void Nrx2Glitch(const Apu &a, uint8_t &volume, uint8_t value, uint8_t old_value,
                uint8_t &countdown, ApuEnvClock &lock)
{
	const uint8_t before = volume;
	// Pre-CGB-D revisions pass through an intermediate all-ones value.
	if (!a.cgb)
	{
		Nrx2GlitchOne(volume, 0xFF, old_value, countdown, lock);
		Nrx2GlitchOne(volume, value, 0xFF, countdown, lock);
	}
	else
	{
		Nrx2GlitchOne(volume, value, old_value, countdown, lock);
	}
	// The rewrite can park the channel at full volume until the trigger that
	// follows (~20 us) — a click per note in drivers that never disable it.
	if (a.suppress_nrx2_glitch && volume > before)
		volume = before;
}

void TickSquareEnvelope(Apu &a, int idx)
{
	ApuSquare &c = idx ? a.ch2 : a.ch1;
	SetEnvelopeClock(c.env, false, 0, 0);
	if (c.env.locked) return;
	if (!(c.nrx2 & 7)) return;

	if (c.nrx2 & 8) ++c.current_volume;
	else            --c.current_volume;
	c.current_volume &= 0xF;
}

void TickNoiseEnvelope(Apu &a)
{
	ApuNoise &c = a.ch4;
	SetEnvelopeClock(c.env, false, 0, 0);
	if (c.env.locked) return;
	if (!(c.nr42 & 7)) return;

	if (c.nr42 & 8) ++c.current_volume;
	else            --c.current_volume;
	c.current_volume &= 0xF;
}

// -------------------------------------------------------------------
// CH1 sweep
// -------------------------------------------------------------------

void SweepCalculationDone(Apu &a)
{
	// APU bug: the overflow check happens after adding the delta twice.
	if (a.channel_1_restart_hold == 0)
		a.shadow_sweep_sample_length = a.ch1.sample_length;
	if (a.ch1.nrx0 & 8)
		a.sweep_length_addend ^= 0x7FF;
	if (a.shadow_sweep_sample_length + a.sweep_length_addend > 0x7FF && !(a.ch1.nrx0 & 8))
		a.is_active[0] = false;
	a.channel1_completed_addend = a.sweep_length_addend;
}

void TriggerSweepCalculation(Apu &a)
{
	if ((a.ch1.nrx0 & 0x70) && a.square_sweep_countdown == 7)
	{
		if (a.ch1.nrx0 & 0x07)
		{
			a.ch1.sample_length = static_cast<uint16_t>(
				(a.sweep_length_addend + a.shadow_sweep_sample_length +
				 ((a.ch1.nrx0 & 0x8) ? 1 : 0)) & 0x7FF);
		}
		if (a.channel_1_restart_hold == 0)
		{
			a.sweep_length_addend = a.ch1.sample_length;
			a.sweep_length_addend >>= (a.ch1.nrx0 & 7);
		}

		// Recalculation and overflow check occur after a delay.
		a.square_sweep_calculate_countdown = a.ch1.nrx0 & 0x7;
		a.square_sweep_calculate_countdown_reload_timer = static_cast<uint8_t>(1 + a.lf_div);
		a.unshifted_sweep = !(a.ch1.nrx0 & 0x7);
		a.square_sweep_countdown = static_cast<uint8_t>(((a.ch1.nrx0 >> 4) & 7) ^ 7);
		if (a.square_sweep_calculate_countdown == 0)
			a.square_sweep_instant_calculation_done = true;
	}
}

void Nr10WriteGlitch(Apu &a, uint8_t value)
{
	if (!a.cgb)
	{
		if (a.square_sweep_calculate_countdown_reload_timer == 1 && !a.lf_div)
		{
			// Double-speed-only corruption — unreachable on DMG.
		}
		else if (a.square_sweep_calculate_countdown_reload_timer > 1)
		{
			// Double-speed only.
		}
		else if (a.square_sweep_calculate_countdown)
		{
			bool should_zombie_step = false;
			if (!(a.ch1.nrx0 & 7))
				should_zombie_step = a.lf_div != 0;

			if (should_zombie_step)
			{
				a.square_sweep_calculate_countdown--;
				if (a.square_sweep_calculate_countdown <= 1)
				{
					a.square_sweep_calculate_countdown = 0;
					SweepCalculationDone(a);
				}
			}
		}
	}
	else
	{
		if (a.square_sweep_calculate_countdown_reload_timer == 2)
		{
			// Countdown just reloaded — re-reload it.
			a.square_sweep_calculate_countdown = value & 0x7;
			if (!a.square_sweep_calculate_countdown)
				a.square_sweep_calculate_countdown_reload_timer = 0;
		}
		if ((value & 7) && !(a.ch1.nrx0 & 7) && !a.lf_div &&
		    a.square_sweep_calculate_countdown > 1)
		{
			a.square_sweep_calculate_countdown--;
			if (!a.square_sweep_calculate_countdown)
				SweepCalculationDone(a);
		}
	}
}

// -------------------------------------------------------------------
// Noise channel helpers
// -------------------------------------------------------------------

void UpdateLfsrSample(Apu &a)
{
	a.ch4.current_lfsr_sample = (a.ch4.lfsr & 1) != 0;
}

void StepLfsr(Apu &a)
{
	a.lfsr_bit_7_before_step = (a.ch4.lfsr & 0x80) != 0;
	const uint16_t high_bit_mask = a.ch4.narrow ? 0x4040 : 0x4000;
	const bool new_high_bit = ((a.ch4.lfsr ^ (a.ch4.lfsr >> 1) ^ 1) & 1) != 0;
	a.ch4.lfsr >>= 1;
	if (new_high_bit) a.ch4.lfsr |= high_bit_mask;
	else              a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~high_bit_mask);
	UpdateLfsrSample(a);
	a.lfsr_stepped_in_narrow = a.ch4.narrow;
}

void PrepareNoiseStart(Apu &a)
{
	a.noise_counter_active = (a.ch4.nr42 & 0xF8) != 0;
	const bool was_started_with_dac_disabled = a.noise_started_with_dac_disabled;
	a.noise_started_with_dac_disabled = !a.noise_counter_active;
	unsigned divisor = a.ch4.nr43 & 0x07;
	const bool was_background_counting = a.noise_background_counter_active;
	a.noise_background_counter_active = true;
	bool instant_step = false;
	bool div_1_glitch = false;

	if (divisor > 1 && a.ch4.counter_countdown == 1)
	{
		a.ch4.counter = static_cast<uint16_t>((a.ch4.counter + 1) & 0x3FFF);
	}
	else if (a.ch4.counter_countdown == 2 && (a.ch4.alignment & 3) == 0 && a.is_active[3])
	{
		if (divisor == 0)
		{
			divisor = 8;
		}
		else if (divisor == 1)
		{
			if (!a.ch4.did_step_counter)
				div_1_glitch = true;

			const uint16_t mask = static_cast<uint16_t>(1u << (a.ch4.nr43 >> 4));
			const bool old_bit = (a.ch4.counter & mask) != 0;
			a.ch4.counter = static_cast<uint16_t>((a.ch4.counter + 1) & 0x3FFF);
			const bool new_bit = (a.ch4.counter & mask) != 0;
			if (new_bit && !old_bit)
				instant_step = true;
		}
	}
	a.ch4.counter_countdown = static_cast<uint8_t>(divisor == 0 ? 6 : divisor * 4 + 6);
	if (a.ch4.alignment & 1)
	{
		if (!divisor)
		{
			if (!a.cgb)                        a.ch4.counter_countdown++;
			else if (was_background_counting)  a.ch4.counter_countdown--;
			else                               a.ch4.counter_countdown++;
		}
		else
		{
			if (a.ch4.alignment & 2)
			{
				if (divisor == 1 && !a.is_active[3]) a.ch4.counter_countdown++;
				else                                 a.ch4.counter_countdown -= 3;
			}
			else
			{
				a.ch4.counter_countdown--;
				if (divisor == 1 && a.is_active[3])
					a.ch4.counter_countdown -= 4;
			}
		}
	}
	else
	{
		if (divisor)
		{
			if (a.ch4.alignment & 2)
			{
				a.ch4.counter_countdown -= 2;
			}
			else if (divisor > 1)
			{
				a.ch4.counter_countdown -= 4;
			}
			else if (divisor == 1 && a.is_active[3] && !(a.ch4.nr43 & 0xF0))
			{
				a.ch4.counter_countdown -= 4;
			}
		}
	}

	// Background counting glitches.
	if (divisor > 1)
	{
		if (!a.noise_counter_active && !(a.ch4.alignment & 3))
			a.ch4.counter_countdown += 4;
	}
	else
	{
		if (was_background_counting && !a.is_active[3] && !(a.ch4.alignment & 3))
		{
			if (divisor == 0)
			{
				if (was_started_with_dac_disabled)
					a.ch4.counter_countdown += 28;
			}
			else
			{
				a.ch4.counter_countdown -= 4;
			}
		}
	}
	if (div_1_glitch)
		a.ch4.counter_countdown -= 4;

	if (!divisor && a.is_active[3] && (a.ch4.alignment & 3) == 3)
		a.ch4.lfsr = 0x0055;
	else
		a.ch4.lfsr = 0;
	if (instant_step)
		StepLfsr(a);
}

void Nr43Write(Apu &a, uint8_t val)
{
	const bool old_narrow = a.ch4.narrow;
	a.ch4.narrow = (val & 8) != 0;
	const uint8_t old = a.ch4.nr43;
	a.ch4.nr43 = val;

	if ((old & 0xF0) == (val & 0xF0)) return;

	uint16_t effective_counter = a.ch4.counter;
	if (!a.cgb && a.ch4.countdown_reloaded)
		effective_counter |= static_cast<uint16_t>((effective_counter - 1) & 0x3FFF);
	const bool old_bit = ((effective_counter >> (old >> 4)) & 1) != 0;

	const uint8_t glitch_value = static_cast<uint8_t>((old & 0x7F) | (val & 0x80));
	const bool glitch_bit = ((effective_counter >> (glitch_value >> 4)) & 1) != 0;
	const bool new_bit    = ((effective_counter >> (val >> 4)) & 1) != 0;

	if ((old_bit == new_bit && new_bit != glitch_bit))
	{
		if (new_bit)
		{
			// Category 1 glitches.
			if (a.cgb)
			{
				if (!(val & 0x80))
				{
					StepLfsr(a);
				}
				else
				{
					const uint8_t t1 = (old >> 4) & 7;
					const uint8_t t2 = (val >> 4) & 7;
					if (((t1 ^ 7) + t2 > 7) || ((t1 ^ 7) & t2))
					{
						// Copy bit 8 to bit 7.
						a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~0x80);
						a.ch4.lfsr |= (a.ch4.lfsr >> 1) & 0x80;
						if ((t1 == 0 || t1 == 4) && t2 == 3)
						{
							a.ch4.lfsr &= static_cast<uint16_t>((a.ch4.lfsr >> 1) | 0x545);
							UpdateLfsrSample(a);
						}
						else if (t1 == 2 && t2 == 3)
						{
							uint16_t mask = 0x555;
							if ((a.ch4.lfsr & 0xC) == 0xC)     mask |= 8;
							if ((a.ch4.lfsr & 0xC00) == 0xC00) mask |= 0x800;
							a.ch4.lfsr &= static_cast<uint16_t>((a.ch4.lfsr >> 1) | mask);
							UpdateLfsrSample(a);
						}
						if (!a.ch4.narrow && old_narrow && a.lfsr_stepped_in_narrow)
						{
							if (a.lfsr_bit_7_before_step) a.ch4.lfsr |= 0x40;
							else                          a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~0x40);
						}
						a.ch4.lfsr |= a.ch4.narrow ? 0x4040 : 0x4000;
						a.lfsr_stepped_in_narrow = a.ch4.narrow;
					}
				}
			}
			else
			{
				const bool previous_narrow = a.ch4.narrow;
				a.ch4.narrow = true;
				StepLfsr(a);
				a.ch4.narrow = previous_narrow;
			}
		}
		else
		{
			// Category 2 glitches.
			if (a.cgb)
			{
				static const uint8_t glitch_map[64] = {
					0,0,4,2,2,2,0,0,   // 0->8..D
					0,0,2,4,2,2,0,0,   // 1
					1,2,0,1,5,3,0,0,   // 2
					0,0,0,0,2,2,0,0,   // 3
					0,2,2,2,0,0,0,0,   // 4
					6,0,2,2,0,0,0,0,   // 5
					0,0,0,0,0,0,0,0,
					0,0,0,0,0,0,0,0,
				};
				const unsigned glitch = (val & 0x80)
					? glitch_map[((old & 0x70) >> 1) | ((val & 0x70) >> 4)] : 0;
				switch (glitch)
				{
					case 1:
					case 6:
						StepLfsr(a);
						if (glitch == 6)
						{
							if ((a.ch4.narrow && ((a.ch4.lfsr & 0x71) == 0x20)) ||
							    (a.ch4.lfsr & 0x71) == 0x61)
								a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~0x20);
							if ((a.ch4.lfsr & 0x7001) == 0x2000 ||
							    (a.ch4.lfsr & 0x7001) == 0x6001)
								a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~0x2000);
						}
						if ((a.ch4.lfsr & 0x3) == 2)
							a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~2);
						break;
					case 2:
					{
						const uint16_t prev = a.ch4.lfsr;
						StepLfsr(a);
						a.ch4.lfsr &= static_cast<uint16_t>(prev | 1);
						break;
					}
					case 5:
						if ((a.ch4.lfsr & 0x3) == 2)
							a.ch4.lfsr &= static_cast<uint16_t>(a.ch4.narrow ? ~0x4040 : ~0x4000);
						if ((a.ch4.lfsr & 0x19) == 8)
							a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~8);
						// fallthrough
					case 3:
						a.ch4.lfsr = static_cast<uint16_t>(a.ch4.lfsr & ~1);
						a.ch4.lfsr |= (a.ch4.lfsr >> 1) & 1;
						UpdateLfsrSample(a);
						a.lfsr_stepped_in_narrow = a.ch4.narrow;
						break;
					case 4:
					{
						const uint16_t prev = a.ch4.lfsr;
						StepLfsr(a);
						a.ch4.lfsr &= static_cast<uint16_t>(prev | (a.ch4.narrow ? ~0x2022 : ~0x2002));
						break;
					}
					default:
						StepLfsr(a);
						break;
				}
			}
			else
			{
				StepLfsr(a);
			}
		}
	}
	else if (!old_bit && new_bit)
	{
		if (!a.cgb)
		{
			const bool previous_narrow = a.ch4.narrow;
			a.ch4.narrow = true;
			StepLfsr(a);
			a.ch4.narrow = previous_narrow;
			if ((val & 0xF0) <= 0x20 && glitch_bit && !(effective_counter & 8))
			{
				StepLfsr(a);
				a.ch4.lfsr &= static_cast<uint16_t>(a.ch4.narrow ? ~0x4040 : ~0x4000);
				a.ch4.lfsr |= static_cast<uint16_t>((a.ch4.lfsr & (a.ch4.narrow ? 0x2020 : 0x2000)) << 1);
			}
		}
		else
		{
			StepLfsr(a);
		}
	}
	else if (!a.cgb)
	{
		if ((val & 0xF0) <= 0x20 && !glitch_bit && !new_bit && !old_bit && (effective_counter & 8))
			StepLfsr(a);
	}
}

// -------------------------------------------------------------------
// Mixer — produces int16 stereo from the four channel outputs.
// -------------------------------------------------------------------

uint8_t g_host_channel_mask = 0x0F;

constexpr int CH_CAPTURE_FRAMES = 9600;
bool    g_wave_capture = false;
int32_t g_ch_accum[4]  = {0, 0, 0, 0};
int16_t g_ch_wave[4][CH_CAPTURE_FRAMES];
int     g_ch_wpos = 0;

void Mix(const Apu &a, int32_t &out_l, int32_t &out_r, int32_t *ch_out = nullptr)
{
	int32_t levels[4];
	for (int ch = 0; ch < 4; ++ch)
		levels[ch] = a.master_enabled
			? DacAnalog(DacEnabled(a, ch), ChannelOut(a, ch)) : 0;

	int32_t l = 0, r = 0;
	for (int ch = 0; ch < 4; ++ch)
	{
		if (ch_out)
			ch_out[ch] = (g_host_channel_mask & (1 << ch)) ? levels[ch] : 0;
		if (!(g_host_channel_mask & (1 << ch)))
			continue;
		const int32_t lvl = levels[ch];
		if (a.nr51 & (1 << ch))       r += lvl;
		if (a.nr51 & (1 << (ch + 4))) l += lvl;
	}

	const int32_t vol_r = static_cast<int32_t>(a.nr50 & 0x07) + 1;         // 1..8
	const int32_t vol_l = static_cast<int32_t>((a.nr50 >> 4) & 0x07) + 1;  // 1..8

	// Half the pre-bias gain: DacAnalog doubles each channel's peak-to-peak
	// swing (15 -> 30), so 35 keeps the audible AC level identical to the old
	// unsigned 0..15 mix at GAIN 70.
	constexpr int32_t GAIN = 35;
	out_l = l * vol_l * GAIN;
	out_r = r * vol_r * GAIN;
	if (out_l >  32767) out_l =  32767;
	if (out_l < -32768) out_l = -32768;
	if (out_r >  32767) out_r =  32767;
	if (out_r < -32768) out_r = -32768;
}

// One biquad section, transposed direct-form II.
inline float Biquad(float x, float b0, float b1, float b2, float a1, float a2,
                    float &z1, float &z2)
{
	const float y = b0 * x + z1;
	z1 = b1 * x - a1 * y + z2;
	z2 = b2 * x - a2 * y;
	return y;
}

void PushSample(Apu &a, int16_t l, int16_t r)
{
	const uint32_t size = APU_SAMPLE_BUF_SIZE;
	const uint32_t next = (a.sample_head + 1) % size;
	if (next == a.sample_tail)
	{
		a.dbg_ring_dropped++;
		return;
	}
	a.sample_buf[a.sample_head * 2 + 0] = l;
	a.sample_buf[a.sample_head * 2 + 1] = r;
	a.sample_head = next;
	a.dbg_ring_pushed++;
}

void FlushSample(Apu &a)
{
	int32_t avg_l = 0, avg_r = 0;
	if (a.sample_accum_cnt > 0)
	{
		avg_l = a.sample_accum_l / static_cast<int32_t>(a.sample_accum_cnt);
		avg_r = a.sample_accum_r / static_cast<int32_t>(a.sample_accum_cnt);
	}

	if (g_wave_capture)
	{
		const int32_t cnt = (a.sample_accum_cnt > 0)
			? static_cast<int32_t>(a.sample_accum_cnt) : 1;
		for (int ch = 0; ch < 4; ++ch)
		{
			int32_t v = (g_ch_accum[ch] / cnt) * 2048;
			if (v >  32767) v =  32767;
			if (v < -32768) v = -32768;
			g_ch_wave[ch][g_ch_wpos] = static_cast<int16_t>(v);
			g_ch_accum[ch] = 0;
		}
		g_ch_wpos = (g_ch_wpos + 1) % CH_CAPTURE_FRAMES;
	}

	a.sample_accum_l   = 0;
	a.sample_accum_r   = 0;
	a.sample_accum_cnt = 0;

	// One-pole DC blocker (R = 0.996 → ~30 Hz corner at 48 kHz).
	constexpr float R = 0.996f;
	const float xl = static_cast<float>(avg_l);
	const float yl = xl - a.hp_xprev_l + R * a.hp_yprev_l;
	a.hp_xprev_l = xl; a.hp_yprev_l = yl;
	const float xr = static_cast<float>(avg_r);
	const float yr = xr - a.hp_xprev_r + R * a.hp_yprev_r;
	a.hp_xprev_r = xr; a.hp_yprev_r = yr;

	float fl_f = yl, fr_f = yr;
	for (int s = 0; s < 2; ++s)
	{
		fl_f = Biquad(fl_f, a.lp_b0[s], a.lp_b1[s], a.lp_b2[s], a.lp_a1[s], a.lp_a2[s],
		              a.lp_z1_l[s], a.lp_z2_l[s]);
		fr_f = Biquad(fr_f, a.lp_b0[s], a.lp_b1[s], a.lp_b2[s], a.lp_a1[s], a.lp_a2[s],
		              a.lp_z1_r[s], a.lp_z2_r[s]);
	}

	int32_t fl = static_cast<int32_t>(fl_f);
	int32_t fr = static_cast<int32_t>(fr_f);
	if (fl >  32767) fl =  32767; else if (fl < -32768) fl = -32768;
	if (fr >  32767) fr =  32767; else if (fr < -32768) fr = -32768;
	PushSample(a, static_cast<int16_t>(fl), static_cast<int16_t>(fr));
}

// Derive the output low-pass coefficients from the current output_rate.
static inline void RecomputeLowpass(Apu &a)
{
	const double PI  = 3.14159265358979323846;
	double fs = a.output_rate > 0 ? (double)a.output_rate : 32000.0;
	double fc = 12000.0;
	if (fc > fs * 0.45) fc = fs * 0.45;
	const double Q[2] = { 0.54119610, 1.30656296 };
	for (int s = 0; s < 2; ++s)
	{
		const double w0    = 2.0 * PI * fc / fs;
		const double c     = std::cos(w0);
		const double sn    = std::sin(w0);
		const double alpha = sn / (2.0 * Q[s]);
		const double b0 = (1.0 - c) * 0.5;
		const double b1 = (1.0 - c);
		const double b2 = (1.0 - c) * 0.5;
		const double a0 = 1.0 + alpha;
		const double a1 = -2.0 * c;
		const double a2 = 1.0 - alpha;
		a.lp_b0[s] = (float)(b0 / a0);
		a.lp_b1[s] = (float)(b1 / a0);
		a.lp_b2[s] = (float)(b2 / a0);
		a.lp_a1[s] = (float)(a1 / a0);
		a.lp_a2[s] = (float)(a2 / a0);
	}
}

static inline void RecomputeSampleRate(Apu &a)
{
	if (a.output_rate < 1) a.output_rate = 32000;
	a.cycles_per_sample  = a.clock_hz / a.output_rate;       // floor
	a.cps_remainder_step = a.clock_hz % a.output_rate;
	if (a.cycles_per_sample < 1) a.cycles_per_sample = 1;
	RecomputeLowpass(a);
}

// Power-on init for the divider chain (NR52 off→on edge).
void ApuPowerOnInit(Apu &a, uint16_t div_counter, bool double_speed)
{
	a.lf_div = 1;
	a.ch3.shift = 4;
	a.div_divider = 0;
	a.skip_div_event = 0;
	// APU glitch: turning the APU on while the DIV-APU bit is set skips
	// the first divider event.
	if (div_counter & (double_speed ? 0x2000 : 0x1000))
	{
		a.skip_div_event = 1;
		a.div_divider    = 1;
	}
	a.ch1.sample_countdown = 0xFFFF;
	a.ch2.sample_countdown = 0xFFFF;
}

} // anonymous

// -------------------------------------------------------------------
// DIV-APU events
// -------------------------------------------------------------------

void ApuDivEvent(Apu &a, bool double_speed)
{
	if (!a.master_enabled) return;
	if (a.skip_div_event == 1) { a.skip_div_event = 2; return; }
	if (a.skip_div_event == 2) a.skip_div_event = 0;
	else                       a.div_divider++;

	if ((a.div_divider & 7) == 7)
	{
		if (!a.ch1.env.clock) { a.ch1.volume_countdown--; a.ch1.volume_countdown &= 7; }
		if (!a.ch2.env.clock) { a.ch2.volume_countdown--; a.ch2.volume_countdown &= 7; }
		if (!a.ch4.env.clock) { a.ch4.volume_countdown--; a.ch4.volume_countdown &= 7; }
	}

	if (a.cgb && double_speed)
	{
		// CGB-D/E defers the envelope tick by one M-cycle in double speed;
		// ApuStep consumes pending_envelope_tick at its next entry.
		a.pending_envelope_tick = true;
	}
	else
	{
		if (a.ch1.env.clock) TickSquareEnvelope(a, 0);
		if (a.ch2.env.clock) TickSquareEnvelope(a, 1);
		if (a.ch4.env.clock) TickNoiseEnvelope(a);
	}

	if ((a.div_divider & 1) == 1)
	{
		if (a.ch1.length_enabled && a.ch1.pulse_length)
		{
			if (!--a.ch1.pulse_length) a.is_active[0] = false;
		}
		if (a.ch2.length_enabled && a.ch2.pulse_length)
		{
			if (!--a.ch2.pulse_length) a.is_active[1] = false;
		}
		if (a.ch3.length_enabled && a.ch3.pulse_length)
		{
			if (!--a.ch3.pulse_length)
			{
				a.is_active[2] = false;
				++a.dbg_ch3_len_disable;
			}
		}
		if (a.ch4.length_enabled && a.ch4.pulse_length)
		{
			if (!--a.ch4.pulse_length) a.is_active[3] = false;
		}
	}

	if ((a.div_divider & 3) == 3)
	{
		a.square_sweep_countdown = static_cast<uint8_t>((a.square_sweep_countdown + 1) & 7);
		TriggerSweepCalculation(a);
	}
}

void ApuDivSecondaryEvent(Apu &a)
{
	if (!a.master_enabled) return;
	if (a.is_active[0] && a.ch1.volume_countdown == 0)
		SetEnvelopeClock(a.ch1.env, (a.ch1.volume_countdown = a.ch1.nrx2 & 7) != 0,
		                 a.ch1.nrx2 & 8, a.ch1.current_volume);
	if (a.is_active[1] && a.ch2.volume_countdown == 0)
		SetEnvelopeClock(a.ch2.env, (a.ch2.volume_countdown = a.ch2.nrx2 & 7) != 0,
		                 a.ch2.nrx2 & 8, a.ch2.current_volume);
	if (a.is_active[3] && a.ch4.volume_countdown == 0)
		SetEnvelopeClock(a.ch4.env, (a.ch4.volume_countdown = a.ch4.nr42 & 7) != 0,
		                 a.ch4.nr42 & 8, a.ch4.current_volume);
}

// -------------------------------------------------------------------
// Core stepping
// -------------------------------------------------------------------

void ApuStep(Apu &a, int32_t tcycles)
{
	if (a.pending_envelope_tick)
	{
		a.pending_envelope_tick = false;
		if (a.master_enabled)
		{
			if (a.ch1.env.clock) TickSquareEnvelope(a, 0);
			if (a.ch2.env.clock) TickSquareEnvelope(a, 1);
			if (a.ch4.env.clock) TickNoiseEnvelope(a);
		}
	}

	while (tcycles > 0)
	{
		int32_t chunk = tcycles;
		if (a.sample_timer < chunk) chunk = a.sample_timer;
		if (chunk < 1) chunk = 1;

		// Advance the 2 MHz channel machinery.
		const int32_t acc = a.apu_tick_parity + chunk;
		uint16_t cycles2 = static_cast<uint16_t>(acc >> 1);
		a.apu_tick_parity = static_cast<uint8_t>(acc & 1);

		if (cycles2 > 0 && a.master_enabled)
		{
			// CH4 DMG delayed start.
			bool start_ch4 = false;
			if (a.ch4.dmg_delayed_start)
			{
				if (a.ch4.dmg_delayed_start <= cycles2)
				{
					a.ch4.dmg_delayed_start = 0;
					start_ch4 = true;
				}
				else
				{
					a.ch4.dmg_delayed_start = static_cast<uint8_t>(a.ch4.dmg_delayed_start - cycles2);
				}
			}

			a.lf_div ^= cycles2 & 1;
			a.ch4.alignment = static_cast<uint8_t>(a.ch4.alignment + cycles2);

			unsigned sweep_cycles = cycles2 / 2;
			if ((cycles2 & 1) && !a.lf_div)
				sweep_cycles++;

			if (a.square_sweep_calculate_countdown_reload_timer > sweep_cycles)
			{
				a.square_sweep_calculate_countdown_reload_timer =
					static_cast<uint8_t>(a.square_sweep_calculate_countdown_reload_timer - sweep_cycles);
				sweep_cycles = 0;
			}
			else
			{
				if (a.square_sweep_calculate_countdown_reload_timer &&
				    !a.square_sweep_calculate_countdown &&
				    a.square_sweep_instant_calculation_done)
					SweepCalculationDone(a);
				a.square_sweep_instant_calculation_done = false;
				sweep_cycles -= a.square_sweep_calculate_countdown_reload_timer;
				a.square_sweep_calculate_countdown_reload_timer = 0;
			}

			if (a.square_sweep_calculate_countdown &&
			    ((a.ch1.nrx0 & 7) || a.unshifted_sweep))
			{
				if (a.square_sweep_calculate_countdown > sweep_cycles)
				{
					a.square_sweep_calculate_countdown =
						static_cast<uint8_t>(a.square_sweep_calculate_countdown - sweep_cycles);
				}
				else
				{
					a.square_sweep_calculate_countdown = 0;
					SweepCalculationDone(a);
				}
			}

			if (a.channel_1_restart_hold)
			{
				if (a.channel_1_restart_hold > cycles2)
					a.channel_1_restart_hold = static_cast<uint8_t>(a.channel_1_restart_hold - cycles2);
				else
					a.channel_1_restart_hold = 0;
			}

			for (int i = 0; i < 2; ++i)
			{
				ApuSquare &c = i ? a.ch2 : a.ch1;
				if (!a.is_active[i]) continue;
				uint16_t cycles_left = cycles2;
				if (c.delay)
				{
					if (c.delay < cycles_left) c.delay = 0;
					else                       c.delay = static_cast<uint8_t>(c.delay - cycles_left);
				}
				while (cycles_left > c.sample_countdown)
				{
					cycles_left = static_cast<uint16_t>(cycles_left - (c.sample_countdown + 1));
					c.sample_countdown = static_cast<uint16_t>(((c.sample_length ^ 0x7FF) * 2) + 1);
					c.current_sample_index = static_cast<uint8_t>((c.current_sample_index + 1) & 7);
					c.duty_latched = static_cast<uint8_t>((c.nrx1 >> 6) & 3);
					c.sample_surpressed = false;
					c.did_tick = true;
				}
				c.just_reloaded = cycles_left == 0;
				if (cycles_left)
					c.sample_countdown = static_cast<uint16_t>(c.sample_countdown - cycles_left);
			}

			a.ch3.wave_form_just_read = false;
			if (a.is_active[2])
			{
				uint16_t cycles_left = cycles2;
				while (cycles_left > a.ch3.sample_countdown)
				{
					cycles_left = static_cast<uint16_t>(cycles_left - (a.ch3.sample_countdown + 1));
					a.ch3.sample_countdown = static_cast<uint16_t>(a.ch3.sample_length ^ 0x7FF);
					a.ch3.current_sample_index = static_cast<uint8_t>((a.ch3.current_sample_index + 1) & 0x1F);
					a.ch3.current_sample_byte = a.ch3.ram[a.ch3.current_sample_index >> 1];
					a.ch3.wave_form_just_read = true;
				}
				if (cycles_left)
				{
					a.ch3.sample_countdown = static_cast<uint16_t>(a.ch3.sample_countdown - cycles_left);
					a.ch3.wave_form_just_read = false;
				}
			}
			else if (a.ch3.enable && a.ch3.pulsed)
			{
				// Background countdown keeps running with the DAC on after
				// the channel stops (CGB-E and earlier).
				uint16_t cycles_left = cycles2;
				while (cycles_left > a.ch3.sample_countdown)
				{
					cycles_left = static_cast<uint16_t>(cycles_left - (a.ch3.sample_countdown + 1));
					a.ch3.sample_countdown = static_cast<uint16_t>(a.ch3.sample_length ^ 0x7FF);
					if (cycles_left)
						a.ch3.current_sample_byte = a.ch3.ram[0];
				}
				if (cycles_left)
					a.ch3.sample_countdown = static_cast<uint16_t>(a.ch3.sample_countdown - cycles_left);
			}

			if (a.noise_counter_active || a.noise_background_counter_active)
			{
				uint16_t cycles_left = cycles2;
				unsigned divisor = (a.ch4.nr43 & 0x07) << 2;
				if (!divisor) divisor = 2;
				if (a.ch4.counter_countdown == 0)
					a.ch4.counter_countdown = static_cast<uint8_t>(divisor);
				while (cycles_left >= a.ch4.counter_countdown)
				{
					cycles_left = static_cast<uint16_t>(cycles_left - a.ch4.counter_countdown);
					a.ch4.counter_countdown = static_cast<uint8_t>(divisor);
					const uint16_t mask = static_cast<uint16_t>(1u << (a.ch4.nr43 >> 4));
					const bool old_bit = (a.ch4.counter & mask) != 0;
					a.ch4.counter = static_cast<uint16_t>((a.ch4.counter + 1) & 0x3FFF);
					a.ch4.did_step_counter = true;
					const bool new_bit = (a.ch4.counter & mask) != 0;
					if (new_bit && !old_bit && a.is_active[3])
						StepLfsr(a);
				}
				if (cycles_left)
				{
					a.ch4.counter_countdown = static_cast<uint8_t>(a.ch4.counter_countdown - cycles_left);
					a.ch4.countdown_reloaded = false;
				}
				else
				{
					a.ch4.countdown_reloaded = true;
				}
			}

			if (start_ch4)
				ApuWrite(a, 0xFF23, static_cast<uint8_t>(a.ch4.nr44 | 0x80), a.cgb, 0, false);
		}

		// Integrate the mixer over this chunk.
		{
			int32_t l, r, chlv[4];
			Mix(a, l, r, g_wave_capture ? chlv : nullptr);
			a.sample_accum_l += l * chunk;
			a.sample_accum_r += r * chunk;
			if (g_wave_capture)
				for (int ch = 0; ch < 4; ++ch)
					g_ch_accum[ch] += chlv[ch] * chunk;
		}
		a.sample_accum_cnt += static_cast<uint32_t>(chunk);
		a.sample_timer     -= chunk;

		if (a.sample_timer <= 0)
		{
			a.sample_timer += a.cycles_per_sample;
			a.cps_remainder_acc += a.cps_remainder_step;
			if (a.cps_remainder_acc >= a.output_rate)
			{
				a.cps_remainder_acc -= a.output_rate;
				a.sample_timer += 1;
			}
			FlushSample(a);
		}

		tcycles -= chunk;
	}
}

// ===================================================================
// Public API
// ===================================================================

void ApuReset(Apu &a, bool cgb, bool post_boot)
{
	a.ch1 = ApuSquare{};
	a.ch2 = ApuSquare{};
	{
		uint8_t ram_save[16];
		std::memcpy(ram_save, a.ch3.ram, 16);
		a.ch3 = ApuWave{};
		if (cgb)
		{
			for (int i = 0; i < 16; ++i)
				a.ch3.ram[i] = (i & 1) ? 0xFF : 0x00;
		}
		else
		{
			static const uint8_t kDmgWaveRam[16] = {
				0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C,
				0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2F, 0xEC,
			};
			std::memcpy(a.ch3.ram, kDmgWaveRam, sizeof a.ch3.ram);
		}
		(void)ram_save;
	}
	a.ch4 = ApuNoise{};

	a.cgb  = cgb;
	a.nr50 = 0;
	a.nr51 = 0;
	a.master_enabled = false;
	a.is_active[0] = a.is_active[1] = a.is_active[2] = a.is_active[3] = false;

	a.div_divider = 0;
	a.lf_div      = 1;
	a.skip_div_event = 0;
	a.pending_envelope_tick = false;
	a.apu_tick_parity = 0;

	a.square_sweep_countdown = 0;
	a.square_sweep_calculate_countdown = 0;
	a.square_sweep_calculate_countdown_reload_timer = 0;
	a.sweep_length_addend = 0;
	a.shadow_sweep_sample_length = 0;
	a.unshifted_sweep = false;
	a.square_sweep_instant_calculation_done = false;
	a.channel_1_restart_hold = 0;
	a.channel1_completed_addend = 0;

	a.noise_counter_active = false;
	a.noise_background_counter_active = false;
	a.lfsr_stepped_in_narrow = false;
	a.lfsr_bit_7_before_step = false;
	a.noise_started_with_dac_disabled = false;

	if (post_boot)
	{
		a.master_enabled = true;
		a.nr50 = 0x77;
		a.nr51 = 0xF3;
		// The boot chime leaves CH1 configured and still flagged active in
		// NR52 (duty 2, envelope $F3 decayed to zero, freq $7C1) — mooneye
		// boot_hwio reads NR11=$BF, NR12=$F3, NR52=$F1.
		a.ch1.nrx0           = 0x00;
		a.ch1.nrx1           = 0x80;
		a.ch1.nrx2           = 0xF3;
		a.ch1.nrx3           = 0xC1;
		a.ch1.nrx4           = 0x07;
		a.ch1.sample_length  = 0x7C1;
		a.ch1.sample_countdown = static_cast<uint16_t>(((0x7C1 ^ 0x7FF) * 2) + 1);
		a.ch1.current_volume = 0;
		a.ch1.pulse_length   = 0x40;
		a.is_active[0]       = true;
	}

	a.sample_accum_l   = 0;
	a.sample_accum_r   = 0;
	a.sample_accum_cnt = 0;
	a.sample_timer     = a.cycles_per_sample;
	a.sample_head      = 0;
	a.sample_tail      = 0;

	a.hp_xprev_l = a.hp_yprev_l = 0.0f;
	a.hp_xprev_r = a.hp_yprev_r = 0.0f;

	a.lp_z1_l[0] = a.lp_z2_l[0] = a.lp_z1_l[1] = a.lp_z2_l[1] = 0.0f;
	a.lp_z1_r[0] = a.lp_z2_r[0] = a.lp_z1_r[1] = a.lp_z2_r[1] = 0.0f;
	RecomputeLowpass(a);

	std::memset(a.sample_buf, 0, sizeof a.sample_buf);
}

// -------------------------------------------------------------------
// Register access
// -------------------------------------------------------------------

uint8_t ApuRead(Apu &a, uint16_t addr, bool cgb)
{
	if (addr >= 0xFF30 && addr <= 0xFF3F)
	{
		if (a.is_active[2])
		{
			if (!cgb && !a.ch3.wave_form_just_read)
				return 0xFF;
			return a.ch3.ram[a.ch3.current_sample_index >> 1];
		}
		return a.ch3.ram[addr - 0xFF30];
	}

	switch (addr)
	{
		case 0xFF10: return static_cast<uint8_t>(a.ch1.nrx0 | 0x80);
		case 0xFF11: return static_cast<uint8_t>(a.ch1.nrx1 | 0x3F);
		case 0xFF12: return a.ch1.nrx2;
		case 0xFF13: return 0xFF;
		case 0xFF14: return static_cast<uint8_t>(a.ch1.nrx4 | 0xBF);

		case 0xFF16: return static_cast<uint8_t>(a.ch2.nrx1 | 0x3F);
		case 0xFF17: return a.ch2.nrx2;
		case 0xFF18: return 0xFF;
		case 0xFF19: return static_cast<uint8_t>(a.ch2.nrx4 | 0xBF);

		case 0xFF1A: return static_cast<uint8_t>(a.ch3.nr30 | 0x7F);
		case 0xFF1B: return 0xFF;
		case 0xFF1C: return static_cast<uint8_t>(a.ch3.nr32 | 0x9F);
		case 0xFF1D: return 0xFF;
		case 0xFF1E: return static_cast<uint8_t>(a.ch3.nr34 | 0xBF);

		case 0xFF20: return 0xFF;
		case 0xFF21: return a.ch4.nr42;
		case 0xFF22: return a.ch4.nr43;
		case 0xFF23: return static_cast<uint8_t>(a.ch4.nr44 | 0xBF);

		case 0xFF24: return a.nr50;
		case 0xFF25: return a.nr51;
		case 0xFF26:
		{
			uint8_t status = a.master_enabled ? 0x80 : 0x00;
			status |= 0x70;
			if (a.is_active[0]) status |= 0x01;
			if (a.is_active[1]) status |= 0x02;
			if (a.is_active[2]) status |= 0x04;
			if (a.is_active[3]) status |= 0x08;
			return status;
		}
	}
	return 0xFF;
}

namespace {

// Store the final written value into the backing register field, the
// same way SameBoy stores into io_registers[] after its switch.
void StoreReg(Apu &a, uint16_t addr, uint8_t value)
{
	switch (addr)
	{
		case 0xFF10: a.ch1.nrx0 = value; break;
		case 0xFF11: a.ch1.nrx1 = value; break;
		case 0xFF12: a.ch1.nrx2 = value; break;
		case 0xFF13: a.ch1.nrx3 = value; break;
		case 0xFF14: a.ch1.nrx4 = value; break;
		case 0xFF16: a.ch2.nrx1 = value; break;
		case 0xFF17: a.ch2.nrx2 = value; break;
		case 0xFF18: a.ch2.nrx3 = value; break;
		case 0xFF19: a.ch2.nrx4 = value; break;
		case 0xFF1A: a.ch3.nr30 = value; break;
		case 0xFF1B: a.ch3.nr31 = value; break;
		case 0xFF1C: a.ch3.nr32 = value; break;
		case 0xFF1D: a.ch3.nr33 = value; break;
		case 0xFF1E: a.ch3.nr34 = value; break;
		case 0xFF20: a.ch4.nr41 = value; break;
		case 0xFF21: a.ch4.nr42 = value; break;
		// NR43 is stored by Nr43Write.
		case 0xFF23: a.ch4.nr44 = value; break;
		case 0xFF24: a.nr50 = value; break;
		case 0xFF25: a.nr51 = value; break;
	}
}

} // anonymous

void ApuWrite(Apu &a, uint16_t addr, uint8_t value, bool cgb,
              uint16_t div_counter, bool double_speed)
{
	// While powered off, register writes are ignored — except NR52, wave
	// RAM, and (on DMG) the length loads through NRx1.
	if (!a.master_enabled && addr != 0xFF26 && addr < 0xFF30 &&
	    (cgb || (addr != 0xFF11 && addr != 0xFF16 && addr != 0xFF1B && addr != 0xFF20)))
	{
		return;
	}

	if (addr >= 0xFF30 && addr <= 0xFF3F)
	{
		++a.dbg_wave_ram_writes;
		if (a.is_active[2])
		{
			if (!cgb && !a.ch3.wave_form_just_read)
				return;
			a.ch3.ram[a.ch3.current_sample_index >> 1] = value;
			return;
		}
		a.ch3.ram[addr - 0xFF30] = value;
		return;
	}

	switch (addr)
	{
		case 0xFF24:
			++a.dbg_nr50_writes;
			break;
		case 0xFF25:
			++a.dbg_nr51_writes;
			break;

		case 0xFF26:
		{
			const uint16_t old_pulse_lengths[4] = {
				a.ch1.pulse_length, a.ch2.pulse_length,
				a.ch3.pulse_length, a.ch4.pulse_length
			};
			if ((value & 0x80) && !a.master_enabled)
			{
				ApuPowerOnInit(a, div_counter, double_speed);
				a.master_enabled = true;
			}
			else if (!(value & 0x80) && a.master_enabled)
			{
				// Power down: clear the whole channel machinery + regs.
				// Wave RAM survives.
				uint8_t ram_save[16];
				std::memcpy(ram_save, a.ch3.ram, 16);
				a.ch1 = ApuSquare{};
				a.ch2 = ApuSquare{};
				a.ch3 = ApuWave{};
				a.ch4 = ApuNoise{};
				std::memcpy(a.ch3.ram, ram_save, 16);
				a.nr50 = 0;
				a.nr51 = 0;
				a.is_active[0] = a.is_active[1] = a.is_active[2] = a.is_active[3] = false;
				a.div_divider = 0;
				a.lf_div = 0;
				a.skip_div_event = 0;
				a.pending_envelope_tick = false;
				a.square_sweep_countdown = 0;
				a.square_sweep_calculate_countdown = 0;
				a.square_sweep_calculate_countdown_reload_timer = 0;
				a.sweep_length_addend = 0;
				a.shadow_sweep_sample_length = 0;
				a.unshifted_sweep = false;
				a.square_sweep_instant_calculation_done = false;
				a.channel_1_restart_hold = 0;
				a.channel1_completed_addend = 0;
				a.noise_counter_active = false;
				a.noise_background_counter_active = false;
				a.master_enabled = false;
			}
			if (!cgb && (value & 0x80))
			{
				// DMG: length counters survive the power cycle.
				a.ch1.pulse_length = old_pulse_lengths[0];
				a.ch2.pulse_length = old_pulse_lengths[1];
				a.ch3.pulse_length = old_pulse_lengths[2];
				a.ch4.pulse_length = old_pulse_lengths[3];
			}
			return;
		}

		/* Square channels */
		case 0xFF10:
		{
			if (a.square_sweep_calculate_countdown ||
			    a.square_sweep_calculate_countdown_reload_timer)
				Nr10WriteGlitch(a, value);
			bool old_negate = (a.ch1.nrx0 & 8) != 0;
			a.ch1.nrx0 = value;
			if (!a.cgb)
				old_negate = true;
			if (a.shadow_sweep_sample_length + a.channel1_completed_addend +
			        (old_negate ? 1 : 0) > 0x7FF && !(value & 8))
			{
				a.is_active[0] = false;
			}
			TriggerSweepCalculation(a);
			break;
		}

		case 0xFF11:
		case 0xFF16:
		{
			ApuSquare &c = (addr == 0xFF16) ? a.ch2 : a.ch1;
			c.pulse_length = static_cast<uint16_t>(0x40 - (value & 0x3F));
			if (!a.master_enabled)
				value &= 0x3F;
			break;
		}

		case 0xFF12:
		case 0xFF17:
		{
			const int idx = (addr == 0xFF17) ? 1 : 0;
			ApuSquare &c = idx ? a.ch2 : a.ch1;
			if ((value & 0xF8) == 0)
			{
				c.nrx2 = value;
				a.is_active[idx] = false;
			}
			else if (a.is_active[idx])
			{
				Nrx2Glitch(a, c.current_volume, value, c.nrx2, c.volume_countdown, c.env);
			}
			break;
		}

		case 0xFF13:
		case 0xFF18:
		{
			ApuSquare &c = (addr == 0xFF18) ? a.ch2 : a.ch1;
			c.sample_length = static_cast<uint16_t>((c.sample_length & ~0xFF) | value);
			if (c.just_reloaded)
				c.sample_countdown = static_cast<uint16_t>(((c.sample_length ^ 0x7FF) * 2) + 1);
			break;
		}

		case 0xFF14:
		case 0xFF19:
		{
			const int idx = (addr == 0xFF19) ? 1 : 0;
			ApuSquare &c = idx ? a.ch2 : a.ch1;
			++a.dbg_trigger_count[idx];
			const bool was_active = a.is_active[idx];

			// $7FF→<$700 length rewrite hack (CGB-D/E, see SameBoy).
			if ((value & 0x80) == 0 && a.is_active[idx] &&
			    (c.nrx4 & 0x7) == 7 && (value & 7) != 7)
			{
				if (a.cgb || (c.sample_countdown & 1))
				{
					if (c.did_tick &&
					    (c.sample_countdown >> 1) == (c.sample_length ^ 0x7FF))
					{
						c.current_sample_index = static_cast<uint8_t>((c.current_sample_index - 1) & 7);
						c.sample_surpressed = false;
					}
				}
			}

			const uint16_t old_sample_length = c.sample_length;
			c.sample_length = static_cast<uint16_t>((c.sample_length & 0xFF) | ((value & 7) << 8));
			if (c.just_reloaded)
				c.sample_countdown = static_cast<uint16_t>(((c.sample_length ^ 0x7FF) * 2) + 1);
			if (value & 0x80)
			{
				c.env.locked = false;
				c.env.clock  = false;
				c.did_tick   = false;
				bool force_unsurpressed = false;
				if (!a.is_active[idx])
				{
					if (a.cgb)
					{
						if (!(value & 4) &&
						    !(((c.sample_countdown - c.delay) / 2) & 0x400))
						{
							c.current_sample_index = static_cast<uint8_t>((c.current_sample_index + 1) & 7);
							force_unsurpressed = true;
						}
					}
					c.delay = static_cast<uint8_t>(6 - a.lf_div);
					c.sample_countdown = static_cast<uint16_t>(((c.sample_length ^ 0x7FF) * 2) + c.delay);
				}
				else
				{
					unsigned extra_delay = 0;
					if (a.cgb)
					{
						if (!c.just_reloaded && !(value & 4) &&
						    !(((c.sample_countdown - 1 - c.delay) / 2) & 0x400))
						{
							c.current_sample_index = static_cast<uint8_t>((c.current_sample_index + 1) & 7);
							c.sample_surpressed = false;
						}
						else if (c.sample_length == 0x7FF &&
						         old_sample_length != 0x7FF &&
						         c.sample_surpressed)
						{
							extra_delay += 2;
						}
					}
					// If already active, sound starts 2 (2 MHz) ticks earlier.
					c.delay = static_cast<uint8_t>(4 - a.lf_div + extra_delay);
					c.sample_countdown = static_cast<uint16_t>(((c.sample_length ^ 0x7FF) * 2) + c.delay);
				}
				c.current_volume = c.nrx2 >> 4;
				c.duty_latched = static_cast<uint8_t>((c.nrx1 >> 6) & 3);
				c.volume_countdown = c.nrx2 & 7;

				if ((c.nrx2 & 0xF8) != 0 && !a.is_active[idx])
				{
					a.is_active[idx] = true;
					c.sample_surpressed = !force_unsurpressed;
				}
				if (c.pulse_length == 0)
				{
					c.pulse_length = 0x40;
					c.length_enabled = false;
				}

				if (idx == 0)
				{
					a.square_sweep_instant_calculation_done = false;
					a.shadow_sweep_sample_length = 0;
					a.channel1_completed_addend = 0;
					if (a.ch1.nrx0 & 7)
					{
						// APU bug: nonzero shift → overflow check on trigger.
						a.square_sweep_calculate_countdown = a.ch1.nrx0 & 0x7;
						if ((a.lf_div ^ 1) && !a.cgb)
							a.square_sweep_calculate_countdown_reload_timer = 3;
						else
							a.square_sweep_calculate_countdown_reload_timer = 2;
						a.unshifted_sweep = false;
						if (!was_active)
							a.square_sweep_calculate_countdown_reload_timer++;
						a.sweep_length_addend = a.ch1.sample_length;
						a.sweep_length_addend >>= (a.ch1.nrx0 & 7);
					}
					else
					{
						a.sweep_length_addend = 0;
					}
					a.channel_1_restart_hold = static_cast<uint8_t>(2 - a.lf_div + (a.cgb ? 2 : 0));
					a.square_sweep_countdown = static_cast<uint8_t>(((a.ch1.nrx0 >> 4) & 7) ^ 7);
				}
			}

			// Extra length clocking when enabling in the first half of a
			// length period.
			if ((value & 0x40) && !c.length_enabled && (a.div_divider & 1) &&
			    c.pulse_length)
			{
				if (!--c.pulse_length)
				{
					if (value & 0x80) c.pulse_length = 0x3F;
					else              a.is_active[idx] = false;
				}
			}
			c.length_enabled = (value & 0x40) != 0;
			break;
		}

		/* Wave channel */
		case 0xFF1A:
			a.ch3.enable = (value & 0x80) != 0;
			if (!a.ch3.enable)
			{
				a.ch3.pulsed = false;
				if (a.is_active[2])
				{
					++a.dbg_ch3_dac_disable;
					if (a.ch3.sample_countdown == 0)
						a.ch3.current_sample_byte = a.ch3.ram[0];
					else if (a.ch3.wave_form_just_read && !a.cgb)
						a.ch3.current_sample_byte = a.ch3.ram[0x0A];
				}
				a.is_active[2] = false;
			}
			break;
		case 0xFF1B:
			a.ch3.pulse_length = static_cast<uint16_t>(0x100 - value);
			break;
		case 0xFF1C:
		{
			static const uint8_t kShift[4] = { 4, 0, 1, 2 };
			a.ch3.shift = kShift[(value >> 5) & 3];
			break;
		}
		case 0xFF1D:
			a.ch3.sample_length = static_cast<uint16_t>((a.ch3.sample_length & ~0xFF) | value);
			break;
		case 0xFF1E:
		{
			a.ch3.sample_length = static_cast<uint16_t>((a.ch3.sample_length & 0xFF) | ((value & 7) << 8));
			if (value & 0x80)
			{
				++a.dbg_trigger_count[2];
				a.ch3.pulsed = true;
				// DMG bug: retriggering one cycle before a fetch corrupts
				// wave RAM.
				if (!cgb && a.is_active[2] && a.ch3.sample_countdown == 0)
				{
					const unsigned offset = ((a.ch3.current_sample_index + 1) >> 1) & 0xF;
					if (offset < 4)
					{
						a.ch3.ram[0] = a.ch3.ram[offset];
					}
					else
					{
						std::memcpy(a.ch3.ram, a.ch3.ram + (offset & ~3u), 4);
					}
				}
				a.ch3.current_sample_index = 0;
				if (a.is_active[2] && a.ch3.sample_countdown == 0)
					a.ch3.current_sample_byte = a.ch3.ram[0];
				if (a.ch3.enable)
					a.is_active[2] = true;
				a.ch3.sample_countdown = static_cast<uint16_t>((a.ch3.sample_length ^ 0x7FF) + 3);
				if (a.ch3.pulse_length == 0)
				{
					a.ch3.pulse_length = 0x100;
					a.ch3.length_enabled = false;
				}
			}

			if ((value & 0x40) && !a.ch3.length_enabled && (a.div_divider & 1) &&
			    a.ch3.pulse_length)
			{
				if (!--a.ch3.pulse_length)
				{
					if (value & 0x80) a.ch3.pulse_length = 0xFF;
					else              a.is_active[2] = false;
				}
			}
			a.ch3.length_enabled = (value & 0x40) != 0;
			break;
		}

		/* Noise channel */
		case 0xFF20:
			a.ch4.pulse_length = static_cast<uint16_t>(0x40 - (value & 0x3F));
			break;

		case 0xFF21:
			if ((value & 0xF8) == 0)
			{
				if (a.is_active[3] && (a.ch4.nr43 & 7))
				{
					if (a.ch4.counter_countdown <= 2)
						a.ch4.counter = static_cast<uint16_t>((a.ch4.counter + 1) & 0x3FFF);
					a.noise_background_counter_active = false;
				}
				a.ch4.nr42 = value;
				a.is_active[3] = false;
				a.noise_counter_active = false;
			}
			else if (a.is_active[3])
			{
				Nrx2Glitch(a, a.ch4.current_volume, value, a.ch4.nr42,
				           a.ch4.volume_countdown, a.ch4.env);
			}
			break;

		case 0xFF22:
		{
			if (a.ch4.countdown_reloaded)
			{
				unsigned divisor = (value & 0x07) << 2;
				if (!divisor) divisor = 2;
				if (a.cgb)
				{
					static const uint8_t kOff[4] = { 2, 1, 0, 3 };
					a.ch4.counter_countdown = static_cast<uint8_t>(
						divisor + (divisor == 2 ? 0 : kOff[a.ch4.alignment & 3]));
				}
				else
				{
					static const uint8_t kOff[4] = { 2, 1, 4, 3 };
					a.ch4.counter_countdown = static_cast<uint8_t>(
						divisor + (divisor == 2 ? 0 : kOff[a.ch4.alignment & 3]));
				}
			}
			if (!a.cgb)
			{
				if (a.ch4.countdown_reloaded)
				{
					const bool old_bit = ((a.ch4.counter >> (a.ch4.nr43 >> 4)) & 1) != 0;
					const bool glitch_bit = ((a.ch4.counter >> 7) & 1) != 0;
					const bool new_bit = ((a.ch4.counter >> (value >> 4)) & 1) != 0;
					if (!old_bit && new_bit && glitch_bit)
					{
						const uint16_t prev = static_cast<uint16_t>((a.ch4.counter - 1) & 0x3FFF);
						const bool p_old = ((prev >> (a.ch4.nr43 >> 4)) & 1) != 0;
						const bool p_glitch = ((prev >> 7) & 1) != 0;
						const bool p_new = ((prev >> (value >> 4)) & 1) != 0;
						if (p_old && !p_new && p_glitch)
							StepLfsr(a);
					}
				}
				Nr43Write(a, 0xFF);
			}
			Nr43Write(a, value);
			return;
		}

		case 0xFF23:
		{
			if (value & 0x80)
			{
				++a.dbg_trigger_count[3];
				a.ch4.env.locked = false;
				a.ch4.env.clock  = false;
				if (!cgb && (a.ch4.alignment & 3) != 0)
				{
					a.ch4.dmg_delayed_start = 6;
				}
				else
				{
					a.ch4.lfsr = 0;
					PrepareNoiseStart(a);
					a.ch4.current_volume = a.ch4.nr42 >> 4;
					a.ch4.current_lfsr_sample = false;
					a.ch4.volume_countdown = a.ch4.nr42 & 7;
					a.ch4.did_step_counter = (a.ch4.alignment & 3) == 2;

					if (a.ch4.nr42 & 0xF8)
						a.is_active[3] = true;

					if (a.ch4.pulse_length == 0)
					{
						a.ch4.pulse_length = 0x40;
						a.ch4.length_enabled = false;
					}
				}
			}

			if ((value & 0x40) && !a.ch4.length_enabled && (a.div_divider & 1) &&
			    a.ch4.pulse_length)
			{
				if (!--a.ch4.pulse_length)
				{
					if (value & 0x80) a.ch4.pulse_length = 0x3F;
					else              a.is_active[3] = false;
				}
			}
			a.ch4.length_enabled = (value & 0x40) != 0;
			break;
		}

		default:
			break;
	}
	StoreReg(a, addr, value);
}

// -------------------------------------------------------------------
// PCM taps + host plumbing
// -------------------------------------------------------------------

uint8_t ApuReadPcm12(const Apu &a)
{
	return static_cast<uint8_t>(SquareOut(a, 0) | (SquareOut(a, 1) << 4));
}

uint8_t ApuReadPcm34(const Apu &a)
{
	return static_cast<uint8_t>(WaveOut(a) | (NoiseOut(a) << 4));
}

void ApuSetHostChannelMask(uint8_t mask)
{
	g_host_channel_mask = mask & 0x0F;
}

uint8_t ApuGetHostChannelMask()
{
	return g_host_channel_mask;
}

void ApuSetWaveCaptureEnabled(bool enabled)
{
	if (enabled && !g_wave_capture)
	{
		std::memset(g_ch_wave, 0, sizeof g_ch_wave);
		for (int ch = 0; ch < 4; ++ch) g_ch_accum[ch] = 0;
		g_ch_wpos = 0;
	}
	g_wave_capture = enabled;
}

int ApuGetChannelWaveform(int channel, int16_t *out, int max_samples)
{
	if (channel < 0 || channel > 3 || !out || max_samples <= 0) return 0;
	const int n = (max_samples < CH_CAPTURE_FRAMES) ? max_samples
	                                                : CH_CAPTURE_FRAMES;
	const int start = (g_ch_wpos - n + CH_CAPTURE_FRAMES) % CH_CAPTURE_FRAMES;
	for (int i = 0; i < n; ++i)
		out[i] = g_ch_wave[channel][(start + i) % CH_CAPTURE_FRAMES];
	return n;
}

int ApuReadChannelWaveformNew(int channel, int *cursor, int16_t *out, int max_samples)
{
	if (channel < 0 || channel > 3 || !out || !cursor || max_samples <= 0) return 0;
	const int cap = CH_CAPTURE_FRAMES;
	const int w = g_ch_wpos;
	int c = *cursor;
	if (c < 0 || c >= cap) { *cursor = w; return 0; }
	int avail = (w - c + cap) % cap;
	if (avail > max_samples) avail = max_samples;
	for (int i = 0; i < avail; ++i)
		out[i] = g_ch_wave[channel][(c + i) % cap];
	*cursor = (c + avail) % cap;
	return avail;
}

void ApuSetOutputRate(Apu &a, int32_t rate)
{
	if (rate <= 0) rate = 32000;
	if (rate == a.output_rate) return;  // idempotent — preserve sample_timer
	a.output_rate = rate;
	RecomputeSampleRate(a);
}

void ApuSetClockHz(Apu &a, int32_t hz)
{
	if (hz <= 0)         return;
	if (hz == a.clock_hz) return;  // idempotent
	a.clock_hz = hz;
	RecomputeSampleRate(a);
}

int32_t ApuDrain(Apu &a, int16_t *out, int32_t max_samples)
{
	int32_t got = 0;
	while (got < max_samples && a.sample_tail != a.sample_head)
	{
		out[got * 2 + 0] = a.sample_buf[a.sample_tail * 2 + 0];
		out[got * 2 + 1] = a.sample_buf[a.sample_tail * 2 + 1];
		a.sample_tail    = (a.sample_tail + 1) % APU_SAMPLE_BUF_SIZE;
		++got;
	}
	a.dbg_ring_drained += got;
	return got;
}

} // namespace SGB
