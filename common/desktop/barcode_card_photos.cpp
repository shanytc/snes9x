/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "barcode_card_photos.hpp"
#include "barcodebattler.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef HAVE_LIBJXL
#include <jxl/decode.h>
#endif

namespace fs = std::filesystem;

std::string S9xBarcodePhotoDir(const std::vector<std::string> &folders)
{
    std::error_code ec;
    for (auto &folder : folders)
    {
        if (folder.empty())
            continue;
        const fs::path dir = fs::path(folder) / "battler";
        if (fs::is_directory(dir, ec))
            return dir.string();
    }
    return {};
}

#ifdef HAVE_LIBJXL
static bool load_jxl(const fs::path &path, BarcodeCardPhoto &photo)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    JxlDecoder *dec = JxlDecoderCreate(nullptr);
    const JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    bool ok = false;
    if (dec && JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS &&
        JxlDecoderSetInput(dec, data.data(), data.size()) == JXL_DEC_SUCCESS)
    {
        JxlDecoderCloseInput(dec);
        for (;;)
        {
            const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
            if (status == JXL_DEC_BASIC_INFO)
            {
                JxlBasicInfo info;
                if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS)
                    break;
                photo.width = (int)info.xsize;
                photo.height = (int)info.ysize;
            }
            else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
            {
                size_t size = 0;
                if (JxlDecoderImageOutBufferSize(dec, &format, &size) != JXL_DEC_SUCCESS ||
                    size != (size_t)photo.width * photo.height * 4)
                    break;
                photo.rgba.resize(size);
                if (JxlDecoderSetImageOutBuffer(dec, &format, photo.rgba.data(), size) != JXL_DEC_SUCCESS)
                    break;
            }
            else if (status != JXL_DEC_FULL_IMAGE)
            {
                ok = status == JXL_DEC_SUCCESS && !photo.rgba.empty();
                break;
            }
        }
    }
    JxlDecoderDestroy(dec);
    return ok;
}
#endif

std::vector<BarcodeCardPhoto> S9xBarcodeCardPhotos(const std::string &dir, const S9xBarcodeCard &card)
{
    std::vector<BarcodeCardPhoto> photos;
#ifdef HAVE_LIBJXL
    if (dir.empty() || !*card.image)
        return photos;

    for (const char *side : { "-front.jxl", "-back.jxl", "-fluorescent.jxl" })
    {
        BarcodeCardPhoto photo;
        if (load_jxl(fs::path(dir) / (std::string(card.image) + side), photo))
            photos.push_back(std::move(photo));
    }
#endif
    return photos;
}
