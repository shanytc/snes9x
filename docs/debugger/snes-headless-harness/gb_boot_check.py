#!/usr/bin/env python3
# Boot-sequence garble detector for GB/SGB BIOS-mode regression runs.
#
# Feed it the PPM frames the harness dumped across the boot window; it
# classifies each by ink coverage inside the GB screen, separating a
# half-drawn Nintendo logo (a few stray tiles on cream) from a real one.
#
#   snes_harness game.zip frames=700 "fb=380:f0380.ppm,405:f0405.ppm,..."
#   python gb_boot_check.py "f*.ppm"
#
# FRAGMENTS = light background with almost no ink — the signature of
# boot-logo VRAM writes being dropped. Exits non-zero if any are found.
import sys
import glob

# GB screen inside the 256x224 SGB composite (border occupies the rest).
GB_X0, GB_X1 = 48, 208
GB_Y0, GB_Y1 = 40, 184


def analyze(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    if parts[0] != b'P6':
        return None
    w, h = map(int, parts[1].split())
    px = parts[3]
    x0, x1 = w * GB_X0 // 256, w * GB_X1 // 256
    y0, y1 = h * GB_Y0 // 224, h * GB_Y1 // 224
    dark = light = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            o = row + x * 3
            s = px[o] + px[o + 1] + px[o + 2]
            if s < 240:
                dark += 1
            elif s > 540:
                light += 1
    return dark, light


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else '*.ppm'
    bad = 0
    for path in sorted(glob.glob(pattern)):
        res = analyze(path)
        if not res:
            continue
        dark, light = res
        tag = ''
        # Blank cream frames idle near dark~160; a dropped-tile logo sat at
        # ~460 where a complete one is ~900+.
        if light > dark * 3 and light > 8000:
            if 250 < dark < 700:
                tag = 'FRAGMENTS?'
                bad += 1
            elif dark >= 700:
                tag = 'logo-like'
        print(f'{path}: dark={dark} light={light} {tag}')
    if bad:
        print(f'\n{bad} suspicious frame(s) - inspect before shipping.')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
