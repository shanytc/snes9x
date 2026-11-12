/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "gtk_compat.h"
#include "barcodereaders.h"

/* Input > Barcodes > Scan Barcode: the code to scan, typed in, made up, picked
 * from the reader's known cards, or read off a picture (picked or dropped on
 * the dialog) or off a webcam held up to the card. Scan hands it to the reader
 * the loaded game's system takes: the Barcode Battler II, the Barcode Boy or
 * Barcode Taisen Bardigun's (barcodereaders.h). The webcam is the one
 * Preferences > Display turns on and picks for the Game Boy Camera; the dialog
 * only reads it. A card's photos show from the
 * reader's folder ("barcodes/battler", "barcodes/barcodeboy" or "barcodes/bardigun") in the config folder or beside
 * the executable (common/desktop/barcode_card_photos.hpp); a code with none is
 * drawn as its barcode. Modeless: Scan leaves it open, the game running on
 * behind it; showing it again brings it forward. */
void S9xShowBarcodeDialog(Gtk::Window &parent, S9xBarcodeReader reader);
void S9xCloseBarcodeDialog();   // a game load: the dialog was for the last one
void S9xBarcodeCameraChanged(); // Preferences applied the camera
void S9xBarcodeConnectChanged(); // Input > Barcodes > Connect was flipped
