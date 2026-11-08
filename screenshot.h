/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SCREENSHOT_H_
#define _SCREENSHOT_H_

bool8 S9xDoScreenshot (int, int);
// Writes GFX.Screen to fname as a PNG, quietly; FALSE if it can't.
bool8 S9xWriteScreenshot (const char *fname, int width, int height);
// One-shot: when set, the next screenshot goes to this front-end target (win32: the clipboard) instead of a file.
extern bool8 (*S9xScreenshotTarget) (int width, int height);

#endif
