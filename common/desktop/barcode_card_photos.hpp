/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* Photos of a barcode reader's known cards (barcodereaders.h): JPEG XL files
 * <image><side> in the reader's folder ("battler" for the Barcode Battler II,
 * "barcodeboy" for the Game Boy readers). Decoding needs libjxl (HAVE_LIBJXL);
 * without it there are no photos. */
struct BarcodeCardPhoto
{
    std::vector<uint8_t> rgba;    // 8-bit RGBA, top-down, width * 4 bytes a row
    int width = 0;
    int height = 0;
};

// The first of the given folders' subfolders by that name that exists, or "".
std::string S9xBarcodePhotoDir(const std::vector<std::string> &folders, const char *subfolder);

// The photos of a card's stem that dir holds, one per side the list (ending in
// NULL) names, in its order.
std::vector<BarcodeCardPhoto> S9xBarcodeCardPhotos(const std::string &dir, const char *image, const char * const *sides);
