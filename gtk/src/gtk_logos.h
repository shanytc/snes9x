/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

/* The four File->Choose Icon logos from data/logos, embedded as PNG bytes by
 * sourcify at build time (see CMakeLists.txt). Every size the win32 .ico
 * files carry is kept because the small ones are hand-tuned, not scaled. */

#define S9X_DECLARE_LOGO(n, s) \
    extern unsigned char logo##n##_##s[]; \
    extern int logo##n##_##s##_size;
#define S9X_DECLARE_LOGO_SET(n) \
    S9X_DECLARE_LOGO(n, 16) S9X_DECLARE_LOGO(n, 24) S9X_DECLARE_LOGO(n, 32) S9X_DECLARE_LOGO(n, 48) \
    S9X_DECLARE_LOGO(n, 64) S9X_DECLARE_LOGO(n, 128) S9X_DECLARE_LOGO(n, 256)

S9X_DECLARE_LOGO_SET(1)
S9X_DECLARE_LOGO_SET(2)
S9X_DECLARE_LOGO_SET(3)
S9X_DECLARE_LOGO_SET(4)

struct LogoImage
{
    int size;
    const unsigned char *data;
    const int *length;
};

#define S9X_LOGO_ENTRY(n, s) { s, logo##n##_##s, &logo##n##_##s##_size }
#define S9X_LOGO_SET(n) \
    { S9X_LOGO_ENTRY(n, 16), S9X_LOGO_ENTRY(n, 24), S9X_LOGO_ENTRY(n, 32), S9X_LOGO_ENTRY(n, 48), \
      S9X_LOGO_ENTRY(n, 64), S9X_LOGO_ENTRY(n, 128), S9X_LOGO_ENTRY(n, 256) }

static const int NUM_LOGOS = 4;
static const int NUM_LOGO_SIZES = 7;
static const LogoImage logo_images[NUM_LOGOS][NUM_LOGO_SIZES] = {
    S9X_LOGO_SET(1), S9X_LOGO_SET(2), S9X_LOGO_SET(3), S9X_LOGO_SET(4)
};
