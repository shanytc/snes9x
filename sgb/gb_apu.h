/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_APU_H_
#define _SGB_GB_APU_H_

#include <cstdint>

namespace SGB {

// GB APU — 4 channels driven by the DIV-APU divider chain. The channel
// timing/trigger machinery is a port of SameBoy's APU core (Core/apu.c,
// (c) Lior Halphon, MIT — see sgb/SAMEBOY-LICENSE.txt), emulating DMG-B
// for monochrome models and CGB-E for color, which is what the SameSuite
// and blargg sound tests validate against. The output side (box-filter
// resampler, DC blocker, reconstruction low-pass, host ring buffer) is
// snes9x-specific.

constexpr uint32_t APU_SAMPLE_BUF_SIZE = 16384;

// FOSY/FYNO-style envelope clock line (see SameBoy) — the envelope tick
// is armed by the secondary DIV event and consumed by the primary one;
// reaching volume $F/$0 in the active direction locks it until retrigger.
struct ApuEnvClock
{
	bool clock       = false;
	bool locked      = false;
	bool should_lock = false;
};

// CH1/CH2 — square wave with envelope; CH1 also has frequency sweep.
struct ApuSquare
{
	// Raw registers — returned by ApuRead with the correct read masks.
	uint8_t  nrx0 = 0;   // CH1 sweep; unused for CH2
	uint8_t  nrx1 = 0;   // duty + length
	uint8_t  duty_latched = 0; // duty as sampled at the last duty-step advance
	uint8_t  nrx2 = 0;   // envelope
	uint8_t  nrx3 = 0;   // freq low
	uint8_t  nrx4 = 0;   // trigger + length enable + freq high

	uint16_t pulse_length     = 0;    // length counter, 256 Hz ticks
	uint8_t  current_volume   = 0;
	uint8_t  volume_countdown = 0;
	uint8_t  current_sample_index = 0;
	bool     sample_surpressed    = false;  // fresh start emits 0 until first tick

	uint16_t sample_countdown = 0;    // in 2 MHz APU ticks
	uint16_t sample_length    = 0;    // 11-bit frequency (NRx3/NRx4)
	bool     length_enabled   = false;
	ApuEnvClock env;
	uint8_t  delay        = 0;        // trigger start delay bookkeeping
	bool     did_tick     = false;
	bool     just_reloaded = false;   // countdown reloaded on the last APU tick
};

// CH3 — wave table from 32 4-bit samples in wave RAM ($FF30-$FF3F).
struct ApuWave
{
	uint8_t  nr30 = 0;
	uint8_t  nr31 = 0;
	uint8_t  nr32 = 0;
	uint8_t  nr33 = 0;
	uint8_t  nr34 = 0;

	uint8_t  ram[16] = {0};   // 32 packed 4-bit samples

	bool     enable       = false;  // NR30 bit 7 (the DAC)
	uint16_t pulse_length = 0;
	uint8_t  shift        = 4;      // NR32 volume shift (4 = mute)
	uint16_t sample_length = 0;
	bool     length_enabled = false;

	uint16_t sample_countdown = 0;  // in 2 MHz APU ticks
	uint8_t  current_sample_index = 0;   // 0..31
	uint8_t  current_sample_byte  = 0;
	bool     wave_form_just_read  = false;
	bool     pulsed               = false;
	uint8_t  bugged_read_countdown = 0;
};

// CH4 — LFSR noise clocked from a free-running 14-bit counter.
struct ApuNoise
{
	uint8_t  nr41 = 0;
	uint8_t  nr42 = 0;
	uint8_t  nr43 = 0;
	uint8_t  nr44 = 0;

	uint16_t pulse_length     = 0;
	uint8_t  current_volume   = 0;
	uint8_t  volume_countdown = 0;
	uint16_t lfsr             = 0;
	bool     narrow           = false;

	uint8_t  counter_countdown = 0;  // 2 MHz ticks until the counter steps
	uint16_t counter           = 0;  // 14-bit; a selected bit's rising edge steps the LFSR
	bool     length_enabled    = false;

	uint8_t  alignment            = 0;  // 2 MHz phase tracking for start alignment
	bool     current_lfsr_sample  = false;
	bool     did_step_counter     = false;
	bool     countdown_reloaded   = false;
	uint8_t  dmg_delayed_start    = 0;
	ApuEnvClock env;
};

struct Apu
{
	ApuSquare ch1, ch2;
	ApuWave   ch3;
	ApuNoise  ch4;

	uint8_t   nr50 = 0;          // master L/R volume + VIN enables
	uint8_t   nr51 = 0;          // per-channel panning
	bool      master_enabled = false;  // NR52 bit 7

	bool      is_active[4] = { false, false, false, false };
	bool      cgb = false;       // model at last reset (DMG-B vs CGB-E)
	// Host policy (not serialized): an NRx2 rewrite on a live channel may
	// never raise its volume — see Nrx2Glitch.
	bool      suppress_nrx2_glitch = false;

	// DIV-APU divider chain state.
	uint8_t   div_divider    = 0;
	uint8_t   lf_div         = 0;   // 1 MHz phase bit of the 2 MHz APU clock
	uint8_t   skip_div_event = 0;   // 0 inactive, 1 skip, 2 skipped
	bool      pending_envelope_tick = false;
	uint8_t   apu_tick_parity = 0;  // odd-T carry for the 2 MHz conversion

	// CH1 sweep machinery.
	uint8_t   square_sweep_countdown = 0;                    // 128 Hz
	uint8_t   square_sweep_calculate_countdown = 0;          // 1 MHz
	uint8_t   square_sweep_calculate_countdown_reload_timer = 0;
	uint16_t  sweep_length_addend = 0;
	uint16_t  shadow_sweep_sample_length = 0;
	bool      unshifted_sweep = false;
	bool      square_sweep_instant_calculation_done = false;
	uint8_t   channel_1_restart_hold = 0;
	uint16_t  channel1_completed_addend = 0;

	// Noise background-counter machinery.
	bool      noise_counter_active = false;
	bool      noise_background_counter_active = false;
	bool      lfsr_stepped_in_narrow = false;
	bool      lfsr_bit_7_before_step = false;
	bool      noise_started_with_dac_disabled = false;

	// Box-filter resampling state.
	int32_t   sample_accum_l    = 0;
	int32_t   sample_accum_r    = 0;
	uint32_t  sample_accum_cnt  = 0;
	int32_t   sample_timer      = 0;
	int32_t   cycles_per_sample = 131;  // ~4.194 MHz / 32000 Hz (floor)
	int32_t   output_rate       = 32000;
	// Actual GB clock rate the host is driving us at (SGB1 = 4.295 MHz).
	int32_t   clock_hz          = 4194304;
	// Bresenham fractional accumulator so long-run pitch is exact.
	int32_t   cps_remainder_step = 0;       // = clock_hz % output_rate
	int32_t   cps_remainder_acc  = 0;       // [0, output_rate)

	// Ring buffer of mixed stereo int16 samples (interleaved L,R).
	int16_t   sample_buf[APU_SAMPLE_BUF_SIZE * 2];
	uint32_t  sample_head = 0;
	uint32_t  sample_tail = 0;

	// One-pole DC blocker per output channel (strips the DAC-bias DC).
	float     hp_xprev_l = 0.0f, hp_yprev_l = 0.0f;
	float     hp_xprev_r = 0.0f, hp_yprev_r = 0.0f;

	// Analog-output reconstruction low-pass: 4th-order Butterworth (two
	// cascaded biquads, ~12 kHz), coefficients derived from output_rate.
	float     lp_b0[2] = {0,0}, lp_b1[2] = {0,0}, lp_b2[2] = {0,0};
	float     lp_a1[2] = {0,0}, lp_a2[2] = {0,0};
	float     lp_z1_l[2] = {0,0}, lp_z2_l[2] = {0,0};
	float     lp_z1_r[2] = {0,0}, lp_z2_r[2] = {0,0};

	// --- Debugger instrumentation (NOT serialized in savestates) ---
	uint32_t  dbg_trigger_count[4] = {0, 0, 0, 0}; // CH1..CH4 NRx4 bit-7 kicks
	uint32_t  dbg_wave_ram_writes  = 0;            // writes to $FF30-$FF3F
	uint32_t  dbg_ch3_len_disable  = 0;            // CH3 off via length underflow
	uint32_t  dbg_ch3_dac_disable  = 0;            // CH3 off via DAC clear (NR30 b7)
	uint32_t  dbg_nr50_writes      = 0;            // writes to $FF24
	uint32_t  dbg_nr51_writes      = 0;            // writes to $FF25 (panning)
	uint32_t  dbg_pcm_silent       = 0;
	// Host-ring I/O meters, in stereo frames.
	uint32_t  dbg_ring_pushed  = 0;
	uint32_t  dbg_ring_dropped = 0;
	uint32_t  dbg_ring_drained = 0;
};

void ApuReset(Apu &a, bool cgb, bool post_boot = false);
void ApuStep(Apu &a, int32_t tcycles);

uint8_t ApuRead(Apu &a, uint16_t addr, bool cgb);
// div_counter is the timer's internal 16-bit counter at write time — the
// NR52 power-on glitch samples the DIV-APU bit to decide whether the
// first divider event is skipped.
void    ApuWrite(Apu &a, uint16_t addr, uint8_t value, bool cgb,
                 uint16_t div_counter, bool double_speed);

int32_t ApuDrain(Apu &a, int16_t *out, int32_t max_samples);

// Configure the downsample target. Idempotent.
void ApuSetOutputRate(Apu &a, int32_t rate);

// Tell the APU the actual GB clock rate it's being stepped at. Idempotent.
void ApuSetClockHz(Apu &a, int32_t hz);

// Host-side channel enable mask, bits 0..3 = CH1..CH4. UI policy only.
void    ApuSetHostChannelMask(uint8_t mask);
uint8_t ApuGetHostChannelMask();

// Per-channel waveform capture for the host's audio-waveform viewer.
void    ApuSetWaveCaptureEnabled(bool enabled);
int     ApuGetChannelWaveform(int channel, int16_t *out, int max_samples);
int     ApuReadChannelWaveformNew(int channel, int *cursor, int16_t *out, int max_samples);

// CGB-only PCM amplitude registers ($FF76/$FF77).
uint8_t ApuReadPcm12(const Apu &a);
uint8_t ApuReadPcm34(const Apu &a);

// DIV-APU events: the primary event fires on a falling edge of DIV bit
// 12 (13 in double speed) — lengths, envelopes, sweep; the secondary
// event fires on the rising edge and arms the envelope clocks.
// gb_timer.cpp drives both, including edges caused by DIV resets.
void ApuDivEvent(Apu &a, bool double_speed);
void ApuDivSecondaryEvent(Apu &a);

} // namespace SGB

#endif
