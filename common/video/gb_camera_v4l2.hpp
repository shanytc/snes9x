/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <string>
#include <vector>

/* Host webcam feed for the Game Boy Camera (Pocket Camera) cartridge's image
 * sensor, the Linux counterpart of win32/win32_webcam.{h,cpp}. The core asks
 * for a 128x112 greyscale frame through the callback registered with
 * S9xGBSetCameraCallback() whenever the game triggers a capture; a background
 * thread keeps the latest frame from a Video4Linux2 device ready for it.
 *
 * Device indexes are positions in the list S9xGBCameraEnumerate() returns,
 * which is what Settings.GBVideoCameraIndex stores (as on win32). Cameras that
 * only offer compressed (MJPEG/H.264) streams are listed but produce no image.
 * On non-Linux builds every call is a stub and the list is always empty. */

void S9xGBCameraRegister();
void S9xGBCameraEnumerate(std::vector<std::string> &names);
bool S9xGBCameraStart(int device_index);
void S9xGBCameraStop();
bool S9xGBCameraIsRunning();

/* Start or stop the capture to match Settings.GBVideoCamera and
 * Settings.GBVideoCameraIndex; a no-op when the running device already
 * matches. Mirrors win32's ApplyGBVideoCamera(). */
void S9xGBCameraApply();
