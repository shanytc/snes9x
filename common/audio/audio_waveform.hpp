/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

/* Everything the Sound > Show Audio Waveform viewer needs that is not a
 * window, shared by the Qt and GTK ports: the track list, the channel masks
 * with the viewer's mute/solo overlay, the zoom modes, the level meters, the
 * per-track WAV recorder and the diagnostic labels. The windows only draw
 * what this hands them and route clicks back. win32 keeps its own copy of
 * this logic in wsnes9x.cpp, which this mirrors. */
namespace audiowave
{

/* Viewer sources, numbered as the win32 viewer numbers them. */
enum Source
{
    kSPC = 0,        // SPC pre-mix; a group row that expands to the voices
    kGB = 1,         // GB APU pre-mix; a group row that expands to the channels
    kMix = 2,        // final host mix, always the bottom row
    kGBChannel1 = 3, // GB CH1-CH4 are 3..6
    kSPCVoice1 = 7,  // SPC V1-V8 are 7..14
    kSourceCount = 15
};

constexpr int kSnapshotFrames = 4800; // frames of history each lane shows
constexpr int kHeaderWidth = 144;     // the track header strip, in pixels

struct Color
{
    uint8_t r, g, b;
};

struct TrackInfo
{
    const char *name;
    Color color;
};
extern const TrackInfo kTracks[kSourceCount];

Color shade(Color c, int pct); // scale toward black
Color tint(Color c, int pct);  // blend toward white

bool is_group(int src);

/* ---- Channel audibility --------------------------------------------------
 * The user's channel masks (Sound > Channels and the viewer's [M] buttons)
 * plus the viewer's solo overlay. SPC voices ride the 8-bit mask the menu
 * shows; the GB APU's CH1-CH4 have their own 4-bit mask so muting CH2 in the
 * viewer does not silence SPC voice 2. The menu is the blunt tool and
 * re-syncs the GB mask to its low bits. The MIX row is the master mute,
 * which lives in the port's config, so the port supplies hooks for it. */
uint8_t spc_mask();
uint8_t gb_mask();
void set_channel_mask(uint8_t mask); // Sound > Channels: SPC, GB = low bits
void toggle_channel(int channel);    // hotkey: 0-7 toggles one, 8 enables all
void enable_all_channels();          // all on, and drops any engaged solo
void set_master_mute_hooks(std::function<bool()> get, std::function<void(bool)> set);

/* Masks the cores actually get: the user masks composed with the solo set.
 * While any solo is engaged only soloed tracks stay audible; mute still wins
 * over solo on the same track. */
void effective_masks(uint8_t *spc, uint8_t *gb);
void apply(); // push the effective masks to the SPC DSP and the GB APU

bool is_muted(int src);   // the user's own mute on this track
bool is_soloed(int src);
bool is_audible(int src); // reaches the output right now (mute + solo)
void toggle_mute(int src);
void toggle_solo(int src);
void clear_solo();

/* ---- Vertical zoom -------------------------------------------------------
 * Chip output is often a small fraction of full scale, so linear rendering
 * leaves near-flat traces; these modes trade amplitude truth for visibility
 * in different ways. */
enum Scale
{
    kScaleLinear = 0, // true amplitude
    kScaleX2,         // fixed gains, clipped at the lane edge
    kScaleX4,
    kScaleX8,
    kScaleX16,
    kScaleAuto, // normalize each lane to its own visible peak
    kScaleLog,  // dB mapping with a -60 dB floor (Audacity-style)
    kScaleSqrt, // gentler perceptual boost than log
    kScaleCount
};
extern const char *const kScaleLabels[kScaleCount];
constexpr int kDefaultScale = kScaleSqrt;

struct RefreshOption
{
    int ms;
    int hz;
    const char *label;
};
constexpr int kRefreshCount = 6;
extern const RefreshOption kRefreshOptions[kRefreshCount];
constexpr int kDefaultRefresh = 3; // 20 Hz

/* Level meter color bands, as fractions of the -48..0 dB meter width. */
struct MeterSegment
{
    float to;
    Color color;
};
extern const MeterSegment kMeterSegments[3];

/* ---- One-track recorder --------------------------------------------------
 * Raw PCM streams to a temporary file while armed; stopping hands the
 * caller the track so it can prompt for a .wav destination. */
class Recorder
{
  public:
    ~Recorder();
    bool start(int src);
    void pump();  // drain what the ring gained since the last call
    void abort(); // drop everything, e.g. on close
    /* Stops recording and returns the track that was armed, or -1 if none.
     * The data is kept until write_wav or discard. */
    int stop();
    bool write_wav(const std::string &path);
    void discard();
    int source() const { return src; }     // armed track, -1 while idle
    int elapsed_seconds() const;
    static int sample_rate(int src);
    static int channels(int src);

  private:
    int src = -1;
    int stopped_src = -1;
    FILE *file = nullptr;
    uint32_t frames = 0;
    int cursor = -1;
    int64_t start_ms = 0;
};

/* ---- The viewer ---------------------------------------------------------- */
class Viewer
{
  public:
    Viewer();
    ~Viewer();
    void open();  // start capturing; call when the window opens
    void close(); // stop capturing and honor reset_on_close

    /* Top-to-bottom row list for the running core and the expansion state.
     * Dead cores are dropped (no SPC group in BIOS-less GB, no GB group for
     * SNES-only games); MIX always sits at the bottom. Solos on tracks that
     * just left the layout are dropped so they cannot silence the rest. */
    int build_order(int order[kSourceCount]);

    /* Newest frames of a source as interleaved stereo, oldest first; the
     * mono GB channels are expanded in place. */
    int fetch(int src, int16_t *lr, int max_frames);
    int sample_rate(int src) const;

    /* Call around each paint: refreshes the diagnostic rates on a 500 ms
     * cadence and sets up the meter decay for this frame. */
    void begin_paint();
    void end_paint();

    /* Info label at the top-left of a lane: rates, or the counters and
     * fills when nerd_stats is on. Empty for rows with nothing to say. */
    std::string info_text(int src) const;
    /* "%.0f ms" label for x-axis division 1..6 of 7. */
    static std::string axis_label(int division, int frames, int sample_rate);

    /* One vertical min/max segment per pixel column under the current zoom,
     * bridged to the previous column so steep slopes read as a continuous
     * trace. Both vectors hold lane-relative y (0 = lane top). */
    void build_envelope(const int16_t *lr, int frames, int width, int lane_height,
                        std::vector<int> &y_min, std::vector<int> &y_max) const;

    /* Logic-style L/R meters: smoothed level and a 1 s peak hold per side,
     * both 0..1 over -48..0 dB. */
    void meter_levels(int src, const int16_t *lr, int frames, float level[2], float peak[2]);

    int scale = kDefaultScale;
    int refresh = kDefaultRefresh;
    bool spc_open = false;
    bool gb_open = false;
    /* By default closing restores the all-on channel defaults (mutes and
     * solos are listening experiments). Unchecked, the audible state is kept
     * and engaged solos are baked into the channel masks. */
    bool reset_on_close = true;
    bool nerd_stats = false;
    Recorder recorder;

  private:
    struct Stats;
    Stats *stats;
    float meter[kSourceCount][2] = {};
    float meter_peak[kSourceCount][2] = {};
    int64_t meter_peak_ms[kSourceCount][2] = {};
    int64_t meter_ms = 0;
    float meter_decay = 1.0f;
    int64_t now_ms = 0;
    bool opened = false;
};

} // namespace audiowave
