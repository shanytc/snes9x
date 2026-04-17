#include "debug_viewer_export.h"

#include <commdlg.h>
#include <stdio.h>
#include <tchar.h>
#include <png.h>

#include "../unzip/zip.h"

namespace {

// libpng IO callback: append to a std::vector<uint8>.
void MemWriteCallback(png_structp png_ptr, png_bytep data, png_size_t len) {
    std::vector<uint8> *buf = (std::vector<uint8> *)png_get_io_ptr(png_ptr);
    buf->insert(buf->end(), data, data + len);
}

void MemFlushCallback(png_structp) { /* nothing */ }

// Write w*h BGRA pixels to `destination` via png_ptr. If `destination` is a
// FILE *, png_init_io has already been set up. If via memory, writer is set
// up with png_set_write_fn.
bool WritePngCommon(png_structp png_ptr, png_infop info_ptr,
                    int w, int h, int stridePixels, const uint32 *bgra) {
    if (setjmp(png_jmpbuf(png_ptr))) return false;

    png_set_IHDR(png_ptr, info_ptr, w, h, 8,
                 PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_bgr(png_ptr); // input is BGRA, convert to RGBA internally.
    png_write_info(png_ptr, info_ptr);

    std::vector<png_bytep> rows((size_t)h);
    for (int y = 0; y < h; ++y) {
        rows[y] = (png_bytep)(bgra + (size_t)y * stridePixels);
    }
    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);
    return true;
}

} // anonymous namespace

bool WritePngFile(const TCHAR *path, int w, int h, int stridePixels,
                  const uint32 *bgra) {
    if (w <= 0 || h <= 0) return false;
    FILE *fp = _tfopen(path, _T("wb"));
    if (!fp) return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) { fclose(fp); return false; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return false;
    }

    bool ok = false;
    if (!setjmp(png_jmpbuf(png_ptr))) {
        png_init_io(png_ptr, fp);
        ok = WritePngCommon(png_ptr, info_ptr, w, h, stridePixels, bgra);
    }
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return ok;
}

bool WritePngToMemory(int w, int h, int stridePixels, const uint32 *bgra,
                      std::vector<uint8> &out) {
    out.clear();
    if (w <= 0 || h <= 0) return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return false;
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return false;
    }

    bool ok = false;
    if (!setjmp(png_jmpbuf(png_ptr))) {
        png_set_write_fn(png_ptr, &out, MemWriteCallback, MemFlushCallback);
        ok = WritePngCommon(png_ptr, info_ptr, w, h, stridePixels, bgra);
    }
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return ok;
}

bool WriteZipFile(const TCHAR *path, const std::vector<ZipBlob> &entries) {
    // minizip's zipOpen takes char*; convert TCHAR -> ANSI.
    char pathA[MAX_PATH];
#ifdef UNICODE
    if (!WideCharToMultiByte(CP_ACP, 0, path, -1, pathA, MAX_PATH, NULL, NULL))
        return false;
#else
    strncpy(pathA, path, MAX_PATH - 1);
    pathA[MAX_PATH - 1] = 0;
#endif

    zipFile zf = zipOpen(pathA, APPEND_STATUS_CREATE);
    if (!zf) return false;

    bool ok = true;
    for (const ZipBlob &e : entries) {
        zip_fileinfo zi = {};
        if (zipOpenNewFileInZip(zf, e.name.c_str(), &zi, NULL, 0, NULL, 0,
                                 NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) {
            ok = false; break;
        }
        if (!e.data.empty()) {
            if (zipWriteInFileInZip(zf, e.data.data(),
                                     (unsigned int)e.data.size()) != ZIP_OK) {
                zipCloseFileInZip(zf);
                ok = false; break;
            }
        }
        if (zipCloseFileInZip(zf) != ZIP_OK) { ok = false; break; }
    }
    zipClose(zf, NULL);
    return ok;
}

bool ShowSaveDialog(HWND parent, TCHAR *outPath, int outPathLen,
                    const TCHAR *defaultName, const TCHAR *filter,
                    const TCHAR *defaultExt) {
    if (defaultName) {
        _tcsncpy(outPath, defaultName, outPathLen - 1);
        outPath[outPathLen - 1] = 0;
    } else {
        outPath[0] = 0;
    }

    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = outPath;
    ofn.nMaxFile    = outPathLen;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                      OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return GetSaveFileName(&ofn) != 0;
}
