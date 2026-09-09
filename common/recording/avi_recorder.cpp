/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "avi_recorder.hpp"
#include "avi_writer.hpp"

#include "snes9x.h"
#include "apu/apu.h"
#include "display.h"
#include "sgb/sgb.h"

#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
/* The APU's true output rate, what the resampler is fed at during a recording
 * (apu.cpp's kAPUInputRate). */
constexpr uint32_t kAPUInputRate = 32040;

AVIWriter writer;
std::string avi_filename;
S9xAVIOptions avi_options;

/* The frame as last captured, already in the file's 24-bit bottom-up form. */
std::vector<uint8_t> frame;
std::vector<int16_t> audio;
int avi_width = 0;
int avi_height = 0;
int avi_pitch = 0;
int audio_rate = 0;

/* GFX.Screen pixel -> B, G, R bytes. */
std::vector<uint8_t> pixel_lut;

/* The audio settings forced for the recording (see S9xAVIStart) and what
 * to put back afterwards. */
uint32_t saved_input_rate = 0;
bool saved_dynamic_rate_control = false;

void buildPixelLUT()
{
    if (!pixel_lut.empty())
        return;
    pixel_lut.resize(65536 * 3);
    for (uint32_t pixel = 0; pixel < 65536; pixel++)
    {
        // GFX.Screen is RGB565 (RGB555 on macOS); widen each component to
        // 8 bits by repeating its top bits so white stays white. The format
        // names are not numeric macros, so tell them apart by green's range.
#if MAX_GREEN == 31
        uint32_t r = (pixel >> 10) & 0x1f, g = (pixel >> 5) & 0x1f, b = pixel & 0x1f;
        uint8_t g8 = (uint8_t)((g << 3) | (g >> 2));
#else
        uint32_t r = (pixel >> 11) & 0x1f, g = (pixel >> 5) & 0x3f, b = pixel & 0x1f;
        uint8_t g8 = (uint8_t)((g << 2) | (g >> 4));
#endif
        pixel_lut[pixel * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
        pixel_lut[pixel * 3 + 1] = g8;
        pixel_lut[pixel * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
    }
}

void forceAudioSettings()
{
    /* The resampler must turn the APU's true output rate into exactly
     * SoundPlaybackRate samples per emulated second, or the audio track
     * drifts against the video: a user-tuned input rate or dynamic rate
     * control would bend it by a fraction of a percent. Remember any change
     * made meanwhile so it comes back when the recording stops. */
    if (Settings.SoundInputRate != kAPUInputRate)
        saved_input_rate = Settings.SoundInputRate;
    if (Settings.DynamicRateControl)
        saved_dynamic_rate_control = true;

    if (Settings.SoundInputRate != kAPUInputRate || Settings.DynamicRateControl)
    {
        Settings.SoundInputRate = kAPUInputRate;
        Settings.DynamicRateControl = false;
        S9xUpdateDynamicRate();
    }
}

void restoreAudioSettings()
{
    Settings.SoundInputRate = saved_input_rate;
    Settings.DynamicRateControl = saved_dynamic_rate_control;
    S9xUpdateDynamicRate();
}
} // namespace

bool S9xAVIStart(const std::string &filename, const S9xAVIOptions &options, std::string *error)
{
    S9xAVIStop();

    // Same output size as win32: the visible SNES image, or the Game Boy
    // screen for a BIOS-less Super Game Boy cart, doubled for hi-res.
    int width = SNES_WIDTH;
    int height = options.overscan ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT;
    if (Settings.SuperGameBoy)
    {
        width = SGB_GB_SCREEN_W;
        height = SGB_GB_SCREEN_H;
    }
    if (options.hires)
    {
        width *= 2;
        height *= 2;
    }
    if (height & 1) // most codecs and players want an even height
        height++;

    // The exact console frame rates (master clock / dots per frame), so the
    // audio written per emulated second lines up with the frames.
    uint32_t fps_rate = Settings.PAL ? 21281370 : 21477272;
    uint32_t fps_scale = Settings.PAL ? 1364 * 312 : 1364 * 262;

    audio_rate = (options.include_audio && Settings.SoundPlaybackRate > 0) ? (int)Settings.SoundPlaybackRate : 0;

    if (!writer.open(filename, width, height, fps_rate, fps_scale, audio_rate, 2))
    {
        if (error)
            *error = "Failed to create AVI file.";
        return false;
    }

    avi_filename = filename;
    avi_options = options;
    avi_width = writer.width();
    avi_height = writer.height();
    avi_pitch = writer.pitch();
    frame.assign((size_t)avi_pitch * avi_height, 0);
    audio.clear();
    buildPixelLUT();

    saved_input_rate = Settings.SoundInputRate;
    saved_dynamic_rate_control = Settings.DynamicRateControl;
    forceAudioSettings();

    std::string message = "Recording AVI: " + std::filesystem::path(filename).filename().string();
    S9xSetInfoString(message.c_str());
    return true;
}

void S9xAVIStop(const char *message)
{
    if (!writer.isOpen())
        return;

    writer.close();
    restoreAudioSettings();
    frame.clear();
    audio.clear();

    S9xSetInfoString(message ? message : "AVI recording stopped.");
}

bool S9xAVIRecording()
{
    return writer.isOpen();
}

const std::string &S9xAVIFilename()
{
    return avi_filename;
}

/* Converts the screen into the file's frame. A hi-res (512-wide) or
 * interlaced (448/478-line) frame is averaged down to the normal size, and
 * a normal frame is doubled up to the hi-res size, as win32 does; anything
 * outside the source is black. */
void S9xAVICaptureFrame(const uint16_t *screen, int pitch_bytes, int width, int height)
{
    if (!writer.isOpen() || !screen || width <= 0 || height <= 0)
        return;

    const int src_ppl = pitch_bytes / 2;

    // Axis mapping: -1 = average two source pixels, 0 = one to one,
    // 1 = repeat each source pixel twice.
    auto axis_mode = [](int src, int dst) {
        if (src > dst * 3 / 2)
            return -1;
        if (src * 3 / 2 < dst)
            return 1;
        return 0;
    };
    const int x_mode = axis_mode(width, avi_width);
    const int y_mode = axis_mode(height, avi_height);

    const uint8_t *lut = pixel_lut.data();

    for (int y = 0; y < avi_height; y++)
    {
        // Bottom-up rows
        uint8_t *dst = frame.data() + (size_t)(avi_height - 1 - y) * avi_pitch;

        int sy0, sy1;
        if (y_mode < 0)
        {
            sy0 = y * 2;
            sy1 = y * 2 + 1;
        }
        else if (y_mode > 0)
            sy0 = sy1 = y / 2;
        else
            sy0 = sy1 = y;

        if (sy0 >= height)
        {
            memset(dst, 0, avi_pitch);
            continue;
        }
        if (sy1 >= height)
            sy1 = sy0;

        const uint16_t *row0 = screen + (size_t)sy0 * src_ppl;
        const uint16_t *row1 = screen + (size_t)sy1 * src_ppl;

        if (x_mode == 0 && y_mode == 0)
        {
            int copy_width = width < avi_width ? width : avi_width;
            for (int x = 0; x < copy_width; x++)
            {
                const uint8_t *p = lut + row0[x] * 3;
                dst[0] = p[0];
                dst[1] = p[1];
                dst[2] = p[2];
                dst += 3;
            }
            if (copy_width < avi_width)
                memset(dst, 0, (size_t)(avi_width - copy_width) * 3);
            continue;
        }

        for (int x = 0; x < avi_width; x++)
        {
            int sx0, sx1;
            if (x_mode < 0)
            {
                sx0 = x * 2;
                sx1 = x * 2 + 1;
            }
            else if (x_mode > 0)
                sx0 = sx1 = x / 2;
            else
                sx0 = sx1 = x;

            if (sx0 >= width)
            {
                memset(dst, 0, (size_t)(avi_width - x) * 3);
                break;
            }
            if (sx1 >= width)
                sx1 = sx0;

            const uint8_t *p0 = lut + row0[sx0] * 3;
            const uint8_t *p1 = lut + row0[sx1] * 3;
            const uint8_t *p2 = lut + row1[sx0] * 3;
            const uint8_t *p3 = lut + row1[sx1] * 3;
            dst[0] = (uint8_t)((p0[0] + p1[0] + p2[0] + p3[0] + 2) >> 2);
            dst[1] = (uint8_t)((p0[1] + p1[1] + p2[1] + p3[1] + 2) >> 2);
            dst[2] = (uint8_t)((p0[2] + p1[2] + p2[2] + p3[2] + 2) >> 2);
            dst += 3;
        }
    }
}

void S9xAVIEndFrame()
{
    if (!writer.isOpen())
        return;

    // Like win32, a change of sound format ends the recording rather than
    // corrupting it.
    if (audio_rate && (int)Settings.SoundPlaybackRate != audio_rate)
    {
        S9xAVIStop("AVI recording stopped (configuration settings changed).");
        return;
    }
    forceAudioSettings();

    writer.addVideoFrame(frame.data());

    if (audio_rate && !audio.empty())
        writer.addAudio(audio.data(), (int)(audio.size() / 2));
    audio.clear();

    if (writer.failed())
        S9xAVIStop("AVI recording stopped (write error).");
}

void S9xAVIAddSamples(const int16_t *samples, int count)
{
    if (!writer.isOpen() || !audio_rate || count <= 0)
        return;
    audio.insert(audio.end(), samples, samples + (count & ~1));
}
