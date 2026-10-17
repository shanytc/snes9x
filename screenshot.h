/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SCREENSHOT_H_
#define _SCREENSHOT_H_

bool8 S9xDoScreenshot (int, int);
// One-shot: when set, the next screenshot goes to this front-end target (win32: the clipboard) instead of a file.
extern bool8 (*S9xScreenshotTarget) (int width, int height);
// One picture as a PNG, Settings.StretchScreenshots applied; square = its pixels already are (a Game Boy screen).
bool8 S9xWriteScreenshotPNG (const char *fname, const uint16 *screen, int pitch, int width, int height, bool8 square);
// A front end's own pictures beside each screenshot (win32: the link session's player windows); returns how many.
extern int (*S9xScreenshotExtras) (const char *fname);

#endif
