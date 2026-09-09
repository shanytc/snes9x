/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

/* Sound > Show Audio Waveform, as on win32: Logic-style track rows for the
 * SPC and GB pre-mixes (each expands to its voices / channels) and the final
 * mix, with record/mute/solo buttons and L/R level meters in each header,
 * and a footer for the reset-on-close, zoom, refresh rate and nerd stats
 * choices. One window at a time; the menu item toggles it. */
void S9xToggleAudioWaveformWindow();
bool S9xAudioWaveformWindowOpen();
void S9xCloseAudioWaveformWindow();
