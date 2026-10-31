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
 * Barcode Taisen Bardigun's (barcodereaders.h). The webcam is the one the Game
 * Boy Camera uses; the dialog runs it while open if it isn't already, and puts
 * it back as the settings have it on close. A card's photos show from the
 * reader's folder ("battler" or "barcodeboy") in the config folder or beside
 * the executable (common/desktop/barcode_card_photos.hpp); a code with none is
 * drawn as its barcode. Returns true when a code was scanned. */
bool S9xBarcodeDialog(Gtk::Window &parent, S9xBarcodeReader reader);
