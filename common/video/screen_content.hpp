/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include "snes9x.h"
#include "sgb/sgb.h"

/* What the loaded content actually draws, for the parts of a port that have to
 * ask: how big the unscaled picture is, and which console screen the Color
 * Correction dialog is speaking for. The win32 counterparts are
 * WinGetContentSize() and ColorCorrectionSystem() in win32/win32_display.cpp
 * and win32/wsnes9x.cpp.
 */

/* Whether the Game Boy core is drawing the picture itself. Without the Super
 * Game Boy BIOS it renders its own 160x144 frame into the buffer; with it
 * (Settings.SGB_BIOSModeActive) the SNES draws the border and the screen is an
 * ordinary SNES one. The two flags are mutually exclusive.
 */
static inline bool S9xContentIsGameBoy()
{
    return Settings.SuperGameBoy != FALSE;
}

/* The unscaled picture for whatever is loaded: what a "size the window to Nx"
 * menu multiplies, and the grid an aspect correction is applied to. `overscan`
 * is the port's own "show the overscan area" setting, which only the SNES has.
 */
static inline void S9xGetContentSize(bool overscan, int *width, int *height)
{
    const bool gb = S9xContentIsGameBoy();

    if (width)
        *width = gb ? (int)SGB_GB_SCREEN_W : SNES_WIDTH;
    if (height)
        *height = gb ? (int)SGB_GB_SCREEN_H
                     : (overscan ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT);
}

/* Which console screen colour correction models, as a name for the Color
 * Correction dialog's checkbox: a Game Boy game rendering in colour gets the
 * Game Boy Color LCD curve (applied in the GB blit), everything else the SNES
 * one, SGB sessions included, since there the SNES draws the picture. Null when
 * there is no screen to model -- nothing loaded, or a Game Boy picture in plain
 * shades. `game_loaded` is the port's own notion of a cart being in.
 */
static inline const char *S9xColorCorrectionSystem(bool game_loaded)
{
    if (!game_loaded)
        return nullptr;
    if (S9xContentIsGameBoy())
        return S9xSGBIsCgbRender() ? "GBC" : nullptr;
    return "SNES";
}
