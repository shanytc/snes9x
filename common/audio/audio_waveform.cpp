/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "audio_waveform.hpp"

#include "snes9x.h"
#include "apu/apu.h"
#include "memmap.h"
#include "ppu.h"
#include "sgb/sgb.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace audiowave
{

/* Logic-Pro-style track colors: each group (SPC / GB / MIX) and each member
 * (V1-8 / CH1-4) has its own. */
const TrackInfo kTracks[kSourceCount] = {
    { "SPC", { 91, 124, 235 } },
    { "GB", { 166, 168, 60 } },
    { "MIX", { 148, 156, 172 } },
    { "CH1", { 102, 187, 106 } },
    { "CH2", { 38, 166, 154 } },
    { "CH3", { 212, 177, 6 } },
    { "CH4", { 239, 112, 67 } },
    { "V1", { 91, 141, 239 } },
    { "V2", { 79, 195, 247 } },
    { "V3", { 77, 208, 165 } },
    { "V4", { 139, 195, 74 } },
    { "V5", { 212, 196, 65 } },
    { "V6", { 240, 154, 62 } },
    { "V7", { 229, 100, 110 } },
    { "V8", { 176, 106, 212 } },
};

const char *const kScaleLabels[kScaleCount] = {
    "linear", "2x", "4x", "8x", "16x", "auto-fit", "log dB", "sqrt",
};

const RefreshOption kRefreshOptions[kRefreshCount] = {
    { 500, 2, "2 Hz (500 ms)" },
    { 200, 5, "5 Hz (200 ms)" },
    { 100, 10, "10 Hz (100 ms)" },
    { 50, 20, "20 Hz (50 ms)" },
    { 33, 30, "30 Hz (33 ms)" },
    { 16, 60, "60 Hz (16 ms)" },
};

/* Green to -12 dB, yellow to -3 dB, red above. */
const MeterSegment kMeterSegments[3] = {
    { 0.75f, { 80, 200, 90 } },
    { 0.9375f, { 220, 200, 70 } },
    { 1.0f, { 230, 80, 70 } },
};

Color shade(Color c, int pct)
{
    return { (uint8_t)(c.r * pct / 100), (uint8_t)(c.g * pct / 100),
             (uint8_t)(c.b * pct / 100) };
}

Color tint(Color c, int pct)
{
    return { (uint8_t)(c.r + (255 - c.r) * pct / 100),
             (uint8_t)(c.g + (255 - c.g) * pct / 100),
             (uint8_t)(c.b + (255 - c.b) * pct / 100) };
}

bool is_group(int src)
{
    return src == kSPC || src == kGB;
}

static int64_t now_milliseconds()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* ---- Channel audibility ------------------------------------------------ */

namespace
{
uint8_t user_spc = 255;
uint8_t user_gb = 0x0F;
uint8_t solo_spc = 0;      // V1-8 solo bits
uint8_t solo_gb = 0;       // CH1-4 solo bits
bool solo_spc_group = false; // SPC group solo (all voices)
bool solo_gb_group = false;  // GB group solo (all channels)
std::function<bool()> master_mute_get;
std::function<void(bool)> master_mute_set;

bool master_muted()
{
    if (master_mute_get)
        return master_mute_get();
    return (Settings.Mute & 1) != 0;
}
} // namespace

uint8_t spc_mask()
{
    return user_spc;
}

uint8_t gb_mask()
{
    return user_gb;
}

void set_master_mute_hooks(std::function<bool()> get, std::function<void(bool)> set)
{
    master_mute_get = std::move(get);
    master_mute_set = std::move(set);
}

void effective_masks(uint8_t *spc, uint8_t *gb)
{
    const uint8_t spc_solo = solo_spc | (solo_spc_group ? 0xFF : 0);
    const uint8_t gb_solo = (uint8_t)((solo_gb | (solo_gb_group ? 0x0F : 0)) & 0x0F);
    const bool any_solo = (spc_solo | gb_solo) != 0;
    *spc = user_spc & (any_solo ? spc_solo : 0xFF);
    *gb = user_gb & (any_solo ? gb_solo : 0x0F);
}

void apply()
{
    uint8_t spc, gb;
    effective_masks(&spc, &gb);
    S9xSetSoundControl(spc);
    S9xSGBSetSoundChannelMask(gb);
}

void set_channel_mask(uint8_t mask)
{
    user_spc = mask;
    // Channels 1-4 double as the GB APU's CH1-CH4 (pulse A, pulse B, wave,
    // noise) so the menu also works for GB/SGB games. The viewer can
    // retarget the GB mask on its own; the menu re-syncs it.
    user_gb = mask & 0x0F;
    apply();
}

void enable_all_channels()
{
    user_spc = 255;
    user_gb = 0x0F;
    // Enable All means audibly all-on: drop any engaged viewer solo too, or
    // the solo mask would keep everything else silent.
    solo_spc = solo_gb = 0;
    solo_spc_group = solo_gb_group = false;
    apply();
}

void toggle_channel(int channel)
{
    if (channel == 8)
        enable_all_channels();
    else
        set_channel_mask(user_spc ^ (1 << channel));
}

bool is_muted(int src)
{
    switch (src)
    {
    case kSPC:
        return (user_spc & 0xFF) == 0;
    case kGB:
        return (user_gb & 0x0F) == 0;
    case kMix:
        return master_muted();
    default:
        if (src >= kSPCVoice1)
            return !(user_spc & (1 << (src - kSPCVoice1)));
        return !(user_gb & (1 << (src - kGBChannel1)));
    }
}

bool is_soloed(int src)
{
    switch (src)
    {
    case kSPC:
        return solo_spc_group;
    case kGB:
        return solo_gb_group;
    case kMix:
        return false;
    default:
        if (src >= kSPCVoice1)
            return (solo_spc >> (src - kSPCVoice1)) & 1;
        return (solo_gb >> (src - kGBChannel1)) & 1;
    }
}

bool is_audible(int src)
{
    uint8_t spc, gb;
    effective_masks(&spc, &gb);
    switch (src)
    {
    case kSPC:
        return spc != 0;
    case kGB:
        return gb != 0;
    case kMix:
        return !master_muted();
    default:
        if (src >= kSPCVoice1)
            return (spc >> (src - kSPCVoice1)) & 1;
        return (gb >> (src - kGBChannel1)) & 1;
    }
}

void toggle_mute(int src)
{
    static uint8_t saved_spc = 255; // group mute keeps the per-member pattern
    static uint8_t saved_gb = 0x0F;
    switch (src)
    {
    case kSPC:
        if (user_spc & 0xFF)
        {
            saved_spc = user_spc;
            user_spc = 0;
        }
        else
            user_spc = saved_spc ? saved_spc : 255;
        break;
    case kGB:
        if (user_gb & 0x0F)
        {
            saved_gb = user_gb;
            user_gb = 0;
        }
        else
            user_gb = saved_gb ? saved_gb : 0x0F;
        break;
    case kMix:
        if (master_mute_set)
            master_mute_set(!master_muted());
        return;
    default:
        if (src >= kSPCVoice1)
            user_spc ^= 1 << (src - kSPCVoice1);
        else
            user_gb ^= 1 << (src - kGBChannel1);
        break;
    }
    apply();
}

void toggle_solo(int src)
{
    switch (src)
    {
    case kSPC:
        solo_spc_group = !solo_spc_group;
        break;
    case kGB:
        solo_gb_group = !solo_gb_group;
        break;
    case kMix:
        return; // the master bus has nothing to solo against
    default:
        if (src >= kSPCVoice1)
            solo_spc ^= 1 << (src - kSPCVoice1);
        else
            solo_gb ^= 1 << (src - kGBChannel1);
        break;
    }
    apply();
}

void clear_solo()
{
    solo_spc = solo_gb = 0;
    solo_spc_group = solo_gb_group = false;
    apply();
}

/* ---- Recorder ---------------------------------------------------------- */

Recorder::~Recorder()
{
    abort();
    discard();
}

int Recorder::sample_rate(int src)
{
    if (src < kGBChannel1)
        return Settings.SoundPlaybackRate;
    if (src >= kSPCVoice1)
        return 32000;
    return (int)S9xSGBGetAudioRate();
}

int Recorder::channels(int src)
{
    return (src >= kGBChannel1 && src < kSPCVoice1) ? 1 : 2;
}

bool Recorder::start(int track)
{
    if (src >= 0)
        return false;
    discard();
    file = std::tmpfile();
    if (!file)
        return false;
    src = track;
    frames = 0;
    cursor = -1;
    start_ms = now_milliseconds();
    pump(); // latches the cursor
    return true;
}

void Recorder::pump()
{
    if (src < 0 || !file)
        return;
    int16_t buf[4096 * 2];
    for (;;)
    {
        int n;
        if (src >= kGBChannel1 && src < kSPCVoice1)
            n = S9xSGBReadChannelWaveformNew(src - kGBChannel1, &cursor, buf, 4096);
        else
            n = S9xAudioWaveformReadNew(src < kGBChannel1 ? src : src - 4, &cursor, buf, 4096);
        if (n <= 0)
            break;
        fwrite(buf, channels(src) * sizeof(int16_t), n, file);
        frames += n;
        if (n < 4096)
            break;
    }
}

void Recorder::abort()
{
    if (src < 0)
        return;
    if (file)
        fclose(file);
    file = nullptr;
    src = -1;
}

int Recorder::stop()
{
    if (src < 0)
        return -1;
    pump();
    stopped_src = src;
    src = -1;
    return stopped_src;
}

void Recorder::discard()
{
    if (src >= 0)
        return; // still armed
    if (file)
        fclose(file);
    file = nullptr;
    stopped_src = -1;
    frames = 0;
}

bool Recorder::write_wav(const std::string &path)
{
    if (stopped_src < 0 || !file)
        return false;
    const int rate = sample_rate(stopped_src);
    const int chans = channels(stopped_src);

    FILE *out = fopen(path.c_str(), "wb");
    if (!out)
        return false;

    const uint32_t data_bytes = frames * chans * 2;
    const uint32_t byte_rate = (uint32_t)rate * chans * 2;
    const uint16_t block = (uint16_t)(chans * 2);
    const uint16_t bits = 16, fmt = 1, nch = (uint16_t)chans;
    const uint32_t riff_size = 36 + data_bytes, fmt_size = 16;
    const uint32_t rate32 = (uint32_t)rate;
    fwrite("RIFF", 1, 4, out);
    fwrite(&riff_size, 4, 1, out);
    fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out);
    fwrite(&fmt_size, 4, 1, out);
    fwrite(&fmt, 2, 1, out);
    fwrite(&nch, 2, 1, out);
    fwrite(&rate32, 4, 1, out);
    fwrite(&byte_rate, 4, 1, out);
    fwrite(&block, 2, 1, out);
    fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out);
    fwrite(&data_bytes, 4, 1, out);

    rewind(file);
    char copy[16384];
    size_t got;
    while ((got = fread(copy, 1, sizeof(copy), file)) > 0)
        fwrite(copy, 1, got, out);
    const bool ok = ferror(out) == 0;
    fclose(out);
    discard();
    return ok;
}

int Recorder::elapsed_seconds() const
{
    if (src < 0)
        return 0;
    return (int)((now_milliseconds() - start_ms) / 1000);
}

/* ---- Viewer ------------------------------------------------------------ */

/* Ring I/O rates and emulation speed, remeasured every 500 ms for the
 * "stats for nerds" labels. */
struct Viewer::Stats
{
    int64_t last_ms = 0;
    uint32_t last_frames = 0;
    double fps = 0.0;
    uint32_t gb_pushed0 = 0, gb_dropped0 = 0, gb_drained0 = 0;
    unsigned int spc_pushed0 = 0, spc_consumed0 = 0;
    uint64_t cycles_snes0 = 0, cycles_gb0 = 0;
    long lines0 = 0;
    double gb_in = 0, gb_out = 0, gb_drop = 0, spc_in = 0, spc_out = 0;
    double snes_mhz = 0, gb_mhz = 0, lines_k = 0;

    void update(int64_t now)
    {
        if (now - last_ms < 500)
            return;
        const double dt = (now - last_ms) / 1000.0;
        fps = (IPPU.TotalEmulatedFrames - last_frames) * 1000.0 / (double)(now - last_ms);
        uint32_t gp = 0, gd = 0, gr = 0;
        unsigned int sp = 0, sc = 0;
        S9xSGBGetAudioRingStats(&gp, &gd, &gr);
        S9xSpcIoMeters(&sp, &sc);
        gb_in = (gp - gb_pushed0) / dt;
        gb_drop = (gd - gb_dropped0) / dt;
        gb_out = (gr - gb_drained0) / dt;
        spc_in = (sp - spc_pushed0) / 2.0 / dt;
        spc_out = (sc - spc_consumed0) / 2.0 / dt;
        gb_pushed0 = gp;
        gb_dropped0 = gd;
        gb_drained0 = gr;
        spc_pushed0 = sp;
        spc_consumed0 = sc;
        uint64_t csn = 0, cgb = 0;
        S9xSGBGetCycleMeters(&csn, &cgb);
        snes_mhz = (csn - cycles_snes0) / dt / 1e6;
        gb_mhz = (cgb - cycles_gb0) / dt / 1e6;
        cycles_snes0 = csn;
        cycles_gb0 = cgb;
        const long ln = S9xApuScanlineMeter();
        lines_k = (ln - lines0) / dt / 1000.0;
        lines0 = ln;
        last_frames = IPPU.TotalEmulatedFrames;
        last_ms = now;
    }
};

Viewer::Viewer()
    : stats(new Stats)
{
}

Viewer::~Viewer()
{
    close();
    delete stats;
}

void Viewer::open()
{
    if (opened)
        return;
    opened = true;
    S9xAudioWaveformEnable(true);
    // Fresh splice-health counts per viewer session: the diagnostic question
    // is "is it climbing right now", not the lifetime total.
    S9xSGBMixGBPadSamples = 0;
    S9xSGBMixSPCShortSamples = 0;
    memset(meter, 0, sizeof(meter));
    memset(meter_peak, 0, sizeof(meter_peak));
    memset(meter_peak_ms, 0, sizeof(meter_peak_ms));
    meter_ms = 0;
}

void Viewer::close()
{
    if (!opened)
        return;
    opened = false;
    recorder.abort();
    S9xAudioWaveformEnable(false);
    if (reset_on_close)
    {
        // Mutes and solos were listening experiments: restore the all-on
        // state Sound > Channels starts with.
        enable_all_channels();
    }
    else
    {
        // Keep what's audible right now: bake engaged solos into the plain
        // channel masks (there is no UI left to unsolo once the viewer is
        // gone), so Sound > Channels keeps showing them.
        uint8_t spc, gb;
        effective_masks(&spc, &gb);
        user_spc = spc;
        user_gb = gb;
        solo_spc = solo_gb = 0;
        solo_spc_group = solo_gb_group = false;
        apply();
    }
}

int Viewer::build_order(int order[kSourceCount])
{
    const bool gb_core = Settings.SuperGameBoy || Settings.SGB_BIOSModeActive;
    const bool spc_core = !Settings.SuperGameBoy;
    // Solos on tracks that just left the layout (ROM swap while the viewer
    // is open) would silently suppress the remaining core: drop them.
    if (!gb_core && (solo_gb || solo_gb_group))
    {
        solo_gb = 0;
        solo_gb_group = false;
        apply();
    }
    if (!spc_core && (solo_spc || solo_spc_group))
    {
        solo_spc = 0;
        solo_spc_group = false;
        apply();
    }
    int n = 0;
    if (spc_core)
    {
        order[n++] = kSPC;
        if (spc_open)
            for (int v = 0; v < 8; v++)
                order[n++] = kSPCVoice1 + v;
    }
    if (gb_core)
    {
        order[n++] = kGB;
        if (gb_open)
            for (int c = 0; c < 4; c++)
                order[n++] = kGBChannel1 + c;
    }
    order[n++] = kMix;
    return n;
}

int Viewer::fetch(int src, int16_t *lr, int max_frames)
{
    if (src < kGBChannel1)
        return S9xAudioWaveformSnapshot(src, lr, max_frames);
    if (src >= kSPCVoice1)
        return S9xAudioWaveformSnapshot(src - 4, lr, max_frames); // voice rings are streams 3..10
    // The GB channel rings are mono: expand in place to the stereo layout,
    // filling from the back so the copy never overtakes the source.
    const int n = S9xSGBGetChannelWaveform(src - kGBChannel1, lr, max_frames);
    for (int i = n - 1; i >= 0; i--)
    {
        lr[i * 2 + 1] = lr[i];
        lr[i * 2 + 0] = lr[i];
    }
    return n;
}

int Viewer::sample_rate(int src) const
{
    if (src < kGBChannel1)
        return S9xAudioWaveformSampleRate();
    if (src >= kSPCVoice1)
        return 32000;
    return (int)S9xSGBGetAudioRate();
}

void Viewer::begin_paint()
{
    now_ms = now_milliseconds();
    stats->update(now_ms);
    float dt = (now_ms - meter_ms) / 1000.0f;
    if (dt < 0.0f || dt > 1.0f)
        dt = 0.1f;
    meter_decay = (float)pow(0.5, dt / 0.15); // 150 ms half-life
}

void Viewer::end_paint()
{
    meter_ms = now_ms; // shared meter-decay clock
}

std::string Viewer::info_text(int src) const
{
    char text[192];
    switch (src)
    {
    case kSPC:
    {
        const double ratio = S9xSpcGetTimeRatio();
        const double hz = (ratio > 0.0) ? (Settings.SoundInputRate / ratio) : 0.0;
        if (nerd_stats)
            snprintf(text, sizeof(text),
                     "SPC %.0f Hz (ratio %.5f)  emu %.1f fps  shown %u/%d  |  in %.1fk out %.1fk",
                     hz, ratio, stats->fps, IPPU.DisplayedRenderedFrameCount,
                     Memory.ROMFramesPerSecond, stats->spc_in / 1000.0, stats->spc_out / 1000.0);
        else
            snprintf(text, sizeof(text), "SPC %.0f Hz", hz);
        return text;
    }
    case kGB:
        if (nerd_stats)
            snprintf(text, sizeof(text),
                     "GB %d Hz  |  in %.1fk out %.1fk drop %.0f/s  |  snes %.2fM  gb %.2fM  ln %.1fk",
                     S9xSGBGetAudioRate(), stats->gb_in / 1000.0, stats->gb_out / 1000.0,
                     stats->gb_drop, stats->snes_mhz, stats->gb_mhz, stats->lines_k);
        else
            snprintf(text, sizeof(text), "GB %d Hz", S9xSGBGetAudioRate());
        return text;
    case kMix:
        if (!nerd_stats)
            return {};
        snprintf(text, sizeof(text),
                 "gbPad %ld  spcShort %ld  spcDrop %ld  |  gbFill %d  spcFill %d/%d  xS %.3f",
                 S9xSGBMixGBPadSamples, S9xSGBMixSPCShortSamples, S9xSpcDroppedSamples(),
                 S9xGetSampleCount(), S9xSpcSpaceFilled(), S9xSpcResamplerCapacity(),
                 S9xSpcGetDrcScale());
        return text;
    case kGBChannel1:
        return "pulse A (sweep)";
    case kGBChannel1 + 1:
        return "pulse B";
    case kGBChannel1 + 2:
        return "wave";
    case kGBChannel1 + 3:
        return "noise";
    default:
        return {};
    }
}

std::string Viewer::axis_label(int division, int frames, int sample_rate)
{
    const double seconds = sample_rate > 0 ? ((double)frames / sample_rate) : 0.0;
    char text[32];
    snprintf(text, sizeof(text), "%.0f ms", seconds * 1000.0 * division / 7.0);
    return text;
}

/* Map one normalized magnitude (0..1) into lane space under the current
 * mode. Every mode is monotonic, so mapping just the min/max envelope
 * endpoints of each pixel column stays valid. */
static double scale_amplitude(int mode, double a, double peak)
{
    switch (mode)
    {
    default:
    case kScaleLinear:
        return a;
    case kScaleX2:
    case kScaleX4:
    case kScaleX8:
    case kScaleX16:
    {
        const double v = a * (double)(1 << (mode - kScaleLinear));
        return v > 1.0 ? 1.0 : v;
    }
    case kScaleAuto:
        return peak > 0.0 ? a / peak : 0.0;
    case kScaleLog:
    {
        if (a <= 0.0)
            return 0.0;
        const double v = 1.0 + (20.0 * log10(a)) / 60.0;
        return v < 0.0 ? 0.0 : v;
    }
    case kScaleSqrt:
        return sqrt(a);
    }
}

void Viewer::build_envelope(const int16_t *lr, int n, int width, int lane_height,
                            std::vector<int> &y_min, std::vector<int> &y_max) const
{
    y_min.clear();
    y_max.clear();
    if (n <= 1 || width <= 0 || lane_height <= 4)
        return;
    y_min.resize(width);
    y_max.resize(width);

    // auto-fit normalizes to this lane's visible peak; the 1%-FS floor keeps
    // idle noise from being blown up into a full-height smear
    double peak_norm = 1.0;
    if (scale == kScaleAuto)
    {
        int pk = 328;
        for (int s = 0; s < n; s++)
        {
            int v = ((int)lr[s * 2] + (int)lr[s * 2 + 1]) / 2;
            if (v < 0)
                v = -v;
            if (v > pk)
                pk = v;
        }
        peak_norm = pk / 32768.0;
    }

    const int mid = lane_height / 2;
    const int span = lane_height - 4;
    int prev_min = 0, prev_max = 0;
    for (int i = 0; i < width; i++)
    {
        int s0 = (i * n) / width;
        int s1 = ((i + 1) * n) / width;
        if (s1 <= s0)
            s1 = s0 + 1;
        int mn = 32767, mx = -32768;
        for (int s = s0; s < s1 && s < n; s++)
        {
            const int v = ((int)lr[s * 2] + (int)lr[s * 2 + 1]) / 2;
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        const double hi = mx >= 0 ? scale_amplitude(scale, mx / 32768.0, peak_norm)
                                  : -scale_amplitude(scale, -mx / 32768.0, peak_norm);
        const double lo = mn >= 0 ? scale_amplitude(scale, mn / 32768.0, peak_norm)
                                  : -scale_amplitude(scale, -mn / 32768.0, peak_norm);
        int top = mid - (int)(hi * span / 2);
        int bottom = mid - (int)(lo * span / 2);
        if (bottom <= top)
            bottom = top + 1;
        if (i > 0)
        {
            if (top > prev_max)
                top = prev_max;
            if (bottom < prev_min)
                bottom = prev_min;
        }
        prev_min = top;
        prev_max = bottom;
        y_min[i] = top;
        y_max[i] = bottom;
    }
}

void Viewer::meter_levels(int src, const int16_t *lr, int n, float level[2], float peak[2])
{
    int pk[2] = { 0, 0 };
    const int window = (n > 1200) ? 1200 : n;
    for (int i = n - window; i < n; i++)
    {
        for (int ch = 0; ch < 2; ch++)
        {
            int v = lr[i * 2 + ch];
            if (v < 0)
                v = -v;
            if (v > pk[ch])
                pk[ch] = v;
        }
    }
    for (int ch = 0; ch < 2; ch++)
    {
        float lvl = 0.0f;
        if (pk[ch] > 0)
        {
            const float db = 20.0f * (float)log10(pk[ch] / 32768.0);
            lvl = (db + 48.0f) / 48.0f;
            if (lvl < 0.0f)
                lvl = 0.0f;
            if (lvl > 1.0f)
                lvl = 1.0f;
        }
        float &m = meter[src][ch];
        m = (lvl > m) ? lvl : m * meter_decay;
        if (lvl >= meter_peak[src][ch] || now_ms - meter_peak_ms[src][ch] > 1000)
        {
            meter_peak[src][ch] = lvl;
            meter_peak_ms[src][ch] = now_ms;
        }
        level[ch] = m;
        peak[ch] = meter_peak[src][ch];
    }
}

} // namespace audiowave
