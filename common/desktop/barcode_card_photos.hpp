/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct S9xBarcodeCard;

/* Photos of the known Barcode Battler cards (barcodebattler.h): JPEG XL files
 * <image>-front.jxl, -back.jxl and -fluorescent.jxl in a "battler" folder.
 * Decoding needs libjxl (HAVE_LIBJXL); without it there are no photos. */
struct BarcodeCardPhoto
{
    std::vector<uint8_t> rgba;    // 8-bit RGBA, top-down, width * 4 bytes a row
    int width = 0;
    int height = 0;
};

// The first of the given folders' "battler" subfolders that exists, or "".
std::string S9xBarcodePhotoDir(const std::vector<std::string> &folders);

// The card's front, back and fluorescent photos that dir holds, in that order.
std::vector<BarcodeCardPhoto> S9xBarcodeCardPhotos(const std::string &dir, const S9xBarcodeCard &card);
