"""Visual regression baselines for carts booted through the SGB BIOS.

Boots every GB/GBC/SGB ROM in SGB BIOS mode and saves one PNG per ROM under
regression_images/. Those PNGs are the baseline: re-run later and any ROM whose
screen moved is reported, so a fix that quietly breaks another game can't slip
through — and because they're images, the folder itself shows which games boot.

This lives with the SNES harness because SGB BIOS mode runs the real SGB BIOS on
the 65816, so it needs the full SNES core. The BIOS-less GB/GBC core is covered
by docs/debugger/gb-headless-harness instead.

Why images and not hashes: a hash only says a screen *changed*. A game that was
already broken hashes as "unchanged" and reads as a pass.

    python sgb_regression.py --update            # record baselines
    python sgb_regression.py                     # check
    python sgb_regression.py --update --filter X # bless an improvement

Changed ROMs also write their new screen to regression_images/_current/ so the
old and new sit side by side.

Needs snes_harness built with -DUNZIP_SUPPORT (see README) and SGB2.sfc reachable:
--bios-dir, else $SNES9X_SYSTEM_DIR, else ./BIOS.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
IMAGES = os.path.join(HERE, "regression_images")
CURRENT = os.path.join(IMAGES, "_current")


def read_ppm(path):
    """Return (w, h, rgb_bytes) from a binary P6 PPM."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or parts[0] != b"P6":
        return None
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    """Minimal PNG writer so the script needs no image library."""
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


SNES_EXT = (".smc", ".sfc", ".fig", ".swc")
GB_EXT   = (".gb", ".gbc")


def _gb_header(path):
    """First 0x200 bytes of the GB payload, or None if this isn't a GB ROM."""
    try:
        if path.lower().endswith(GB_EXT):
            with open(path, "rb") as f:
                return f.read(0x200)
        if path.lower().endswith(".zip"):
            zf = zipfile.ZipFile(path)
            inner = [n for n in zf.namelist() if n.lower().endswith(GB_EXT)]
            if inner:
                return zf.read(inner[0])[:0x200]
    except Exception:
        pass
    return None


def rom_class(path):
    """'sgb' (SGB-enhanced cart), 'gb' (plain GB/GBC), 'snes', or None.

    $0146 == 0x03 is the cart's own "I support Super Game Boy" flag, so it
    decides which harness owns the ROM.
    """
    hdr = _gb_header(path)
    if hdr is not None and len(hdr) > 0x146:
        return "sgb" if hdr[0x146] == 0x03 else "gb"
    low = path.lower()
    if low.endswith(SNES_EXT):
        return "snes"
    if low.endswith(".zip"):
        try:
            if any(n.lower().endswith(SNES_EXT) for n in zipfile.ZipFile(path).namelist()):
                return "snes"
        except Exception:
            return None
    return None


def content_score(w, h, rgb):
    """How much is actually on screen — a uniform fade/black frame scores 0."""
    dark = light = 0
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            o = row + x * 3
            v = rgb[o] + rgb[o + 1] + rgb[o + 2]
            if v < 240:
                dark += 1
            elif v > 540:
                light += 1
    return min(dark, light)


def capture(harness, rom, frames, workdir, timeout):
    """Boot once, sample several frames, keep the most informative one.

    No single fixed frame suits every ROM: slow intros are still black early,
    and some titles fade through blank frames. Picking the busiest sample stops
    a baseline from recording "blank" for a game that boots fine.
    """
    with tempfile.TemporaryDirectory(dir=workdir) as tmp:
        shots = {f: os.path.join(tmp, f"f{f}.ppm") for f in frames}
        fb = ",".join(f"{f}:{p}" for f, p in shots.items())
        try:
            subprocess.run([harness, rom, f"frames={max(frames) + 20}", "log=1000000",
                            f"fb={fb}"],
                           capture_output=True, cwd=workdir, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        best = None
        for f in frames:
            if not os.path.exists(shots[f]):
                continue
            got = read_ppm(shots[f])
            if got is None:
                continue
            score = content_score(*got)
            if best is None or score > best[0]:
                best = (score, got)
        return best[1] if best else None


def write_summary(images_dir, rows):
    """List every ROM and flag the ones whose capture is a blank screen.

    A uniform white/black PNG looks identical to "nothing was captured", so
    say it in words rather than leaving it to be guessed.
    """
    path = os.path.join(images_dir, "_summary.txt")
    blanks = [n for n, blank in rows if blank]
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Generated by the regression script - do not edit.\n")
        f.write(f"# {len(rows)} ROMs, {len(blanks)} captured a blank screen.\n")
        f.write("# BLANK = one flat colour at every sampled frame: the game\n")
        f.write("# shows nothing in this mode, or hangs.\n\n")
        for n, blank in sorted(rows):
            f.write(f"{'BLANK' if blank else 'ok   '}  {n}\n")
    if blanks:
        print(f"\n{len(blanks)} ROM(s) captured a blank screen - see _summary.txt:")
        for n in blanks:
            print(f"   BLANK  {n}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", default=os.environ.get("SNES_HARNESS", "/tmp/snes_harness"),
                    help="path to the built snes_harness binary")
    ap.add_argument("--rom-dir", default=os.path.join(HERE, "..", "..", "..", "win32", "Roms"))
    ap.add_argument("--bios-dir", default=os.environ.get("SNES9X_SYSTEM_DIR", "BIOS"))
    ap.add_argument("--frames", default="1500,2400,3300,4200",
                    help="comma-separated frames to sample; the busiest one becomes "
                         "the baseline (slow intros need the later samples)")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--filter", default="", help="only ROMs whose name contains this")
    ap.add_argument("--update", action="store_true", help="write/refresh baselines instead of checking")
    args = ap.parse_args()

    if not os.path.exists(args.harness) and os.path.exists(args.harness + ".exe"):
        args.harness += ".exe"          # cygwin/mingw builds land as .exe
    os.makedirs(IMAGES, exist_ok=True)
    bios = os.path.abspath(args.bios_dir)
    if not os.path.isdir(bios):
        print(f"BIOS dir not found: {bios}", file=sys.stderr)
        return 2
    os.environ["SNES9X_SYSTEM_DIR"] = bios
    workdir = os.path.dirname(bios) or "."

    frame_list = [int(v) for v in args.frames.split(",") if v.strip()]
    roms = sorted(os.path.join(args.rom_dir, f) for f in os.listdir(args.rom_dir))
    roms = [r for r in roms if rom_class(r) in ("snes", "sgb")
            and args.filter.lower() in os.path.basename(r).lower()]
    # One baseline per game: the same title often ships as both .smc/.gb and
    # .zip, which would collide on the PNG name and silently overwrite.
    seen, deduped = set(), []
    for r in roms:
        stem = os.path.splitext(os.path.basename(r))[0]
        if stem in seen:
            print(f"SKIP     {stem}  (duplicate of an already-captured container)")
            continue
        seen.add(stem)
        deduped.append(r)
    roms = deduped
    if not roms:
        print("no matching ROMs", file=sys.stderr)
        return 2

    changed, new, same, failed, rows = [], [], 0, [], []
    for rom in roms:
        name = os.path.splitext(os.path.basename(rom))[0]
        shot = capture(args.harness, rom, frame_list, workdir, args.timeout)
        if shot is None:
            failed.append(name)
            print(f"FAILED   {name}")
            sys.stdout.flush()
            continue
        w, h, rgb = shot
        rows.append((name, content_score(w, h, rgb) == 0))
        base = os.path.join(IMAGES, name + ".png")
        if args.update:
            write_png(base, w, h, rgb)
            print(f"{'UPDATED ' if os.path.exists(base) else 'ADDED   '} {name}")
        elif not os.path.exists(base):
            new.append(name)
            os.makedirs(CURRENT, exist_ok=True)
            write_png(os.path.join(CURRENT, name + ".png"), w, h, rgb)
            print(f"NEW      {name}  (no baseline yet)")
        else:
            old = open(base, "rb").read()
            os.makedirs(CURRENT, exist_ok=True)
            tmp_png = os.path.join(CURRENT, name + ".png")
            write_png(tmp_png, w, h, rgb)
            if open(tmp_png, "rb").read() == old:
                os.remove(tmp_png)
                same += 1
            else:
                changed.append(name)
                print(f"CHANGED  {name}  -> regression_images/_current/{name}.png")
        sys.stdout.flush()

    write_summary(IMAGES, rows)

    if args.update:
        print(f"\nbaselines written to {IMAGES}")
        return 0

    print(f"\nunchanged={same} changed={len(changed)} new={len(new)} failed={len(failed)}")
    if changed:
        print("\nCompare each pair, then bless the good ones:")
        print("  regression_images/<rom>.png   (baseline)")
        print("  regression_images/_current/<rom>.png   (now)")
        print("  python gb_regression.py --update --filter <rom>")
    return 1 if (changed or failed) else 0


if __name__ == "__main__":
    sys.exit(main())
