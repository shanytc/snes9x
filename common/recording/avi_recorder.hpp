/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <string>

/* AVI recording for the Qt and GTK ports: the counterpart of the win32
 * port's File->AVI Recording (DoAVIOpen/DoAVIVideoFrame in win32.cpp), built
 * on the portable AVIWriter instead of Video for Windows.
 *
 * The port feeds it from three places:
 *   - S9xAVICaptureFrame() from S9xDeinitUpdate(), with the screen exactly
 *     as it is about to be displayed (after the overscan offset, before any
 *     hi-res effect or software filter changes it);
 *   - S9xAVIEndFrame() once per emulated frame from the main loop, which
 *     writes the last captured frame (again, if the frame was skipped, so the
 *     file never drifts from the audio) and the audio gathered since;
 *   - S9xAVIAddSamples() from the samples-available handler with every mixed
 *     sample, before the sound device gets to drop any.
 *
 * All calls belong on the thread that runs the emulation. */
struct S9xAVIOptions
{
    bool hires = false;        // 2x output (512x448) like win32's "Hi-Res AVI Recording"
    bool overscan = false;     // 239 visible lines instead of 224
    bool include_audio = true; // win32 records a silent movie while muted
};

bool S9xAVIStart(const std::string &filename, const S9xAVIOptions &options, std::string *error = nullptr);
void S9xAVIStop(const char *message = nullptr);
bool S9xAVIRecording();
const std::string &S9xAVIFilename();

void S9xAVICaptureFrame(const uint16_t *screen, int pitch_bytes, int width, int height);
void S9xAVIEndFrame();
void S9xAVIAddSamples(const int16_t *samples, int count);
