/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "gtk_compat.h"

/* Input > Barcodes > Scan Barcode: the code to swipe, typed in,
 * made up, picked from the known cards, or read off a picture (picked or
 * dropped on the dialog) or off a webcam held up to the card. Scan hands it
 * to the BBII interface on port 2 (barcodebattler.h). The webcam is the one
 * Preferences > Display turns on and picks for the Game Boy Camera; the
 * dialog only reads it. A card's photos show from a "battler" folder in the
 * config folder or beside the executable
 * (common/desktop/barcode_card_photos.hpp). Modeless: Scan leaves it open,
 * the game running on behind it; showing it again brings it forward. */
void S9xShowBarcodeBattlerDialog(Gtk::Window &parent);
void S9xCloseBarcodeBattlerDialog();    // a game load: the dialog was for the last one
void S9xBarcodeBattlerCameraChanged();  // Preferences applied the camera
void S9xBarcodeBattlerConnectChanged(); // Input > Barcodes > Connect was flipped
