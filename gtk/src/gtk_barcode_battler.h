/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "gtk_compat.h"

/* Input > Barcode Battler II > Scan Barcode: the code to swipe, typed in,
 * made up, read off a picture or off a webcam held up to the card. Scan hands
 * it to the BBII interface on port 2 (barcodebattler.h). The webcam is the
 * one the Game Boy Camera uses; the dialog runs it while open if it isn't
 * already, and puts it back as the settings have it on close. Returns true
 * when a code was scanned. */
bool S9xBarcodeBattlerDialog(Gtk::Window &parent);
