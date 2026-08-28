#!/usr/bin/env python3
"""Turn the GB Emulator Shootout results table into one baseline per emulator.

The published table at https://tomek.rekawek.eu/GBEmulatorShootout/ carries a
screenshot for every emulator/test pair, inlined as base64 PNGs - so the whole
set arrives in a single page fetch rather than thousands of requests.

Each emulator becomes acid/baseline/<slug>/, laid out exactly like
baseline/default/, which is all it takes for the runner to pick it up and
compare against it. Images are filed under the path the manifest gives the
test, and a baseline.txt records the emulator, the table's date and each
verdict.

    python fetch_baselines.py                 # download and extract
    python fetch_baselines.py --html page.html   # from a saved copy
    python fetch_baselines.py --only sameboy --only bgb
    python fetch_baselines.py --list          # just say what is on offer

Screenshots are the shootout's own output, MIT licensed like the project.
"""

import argparse
import base64
import html as htmllib
import os
import re
import sys
import urllib.request

URL = "https://tomek.rekawek.eu/GBEmulatorShootout/"
ACID_ROOT = os.path.dirname(os.path.abspath(__file__))
BASELINE_ROOT = os.path.join(ACID_ROOT, "baseline")


def slugify(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower())
    return s.strip("_")


def manifest_images():
    """Test name -> the relative path a baseline files its frame under.

    The test name with a trailing .gb/.gbc dropped and .png added, which is
    what the runner writes when it saves a baseline of its own. Not the
    manifest's pass_images: two tests can share one of those - bully.gb on
    DMG and on GBC both point at ashiepaws/bully.png - and one frame would
    then overwrite the other.
    """
    out = {}
    path = os.path.join(ACID_ROOT, "manifest.txt")
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            name = line.rstrip("\n").split("|")[0]
            out[name] = re.sub(r"\.(gb|gbc)$", "", name) + ".png"
    return out


def png_size(data):
    """(width, height) from the IHDR, or None if it is not a PNG."""
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return (int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big"))


def parse(page):
    """(updated, emulators, rows) with rows as (test, [(status, png|None)])."""
    updated = ""
    m = re.search(r"Updated On<br>([^<]*)", page)
    if m:
        updated = m.group(1).strip()

    emulators = []
    for th in re.findall(r"<th class='emulator'>(.*?)</th>", page, re.S):
        name = htmllib.unescape(re.sub("<[^>]+>", "", th)).strip()
        emulators.append(re.sub(r"\s*\(\d+/\d+\)$", "", name))

    rows = []
    for tr in re.findall(r"<tr>(?!<th style)(.*?)</tr>", page, re.S):
        th = re.search(r"<th class='test'>(.*?)</th>", tr, re.S)
        if not th:
            continue
        name = re.sub(r'<span class="tooltiptext">.*?</span>', "", th.group(1), flags=re.S)
        name = htmllib.unescape(re.sub("<[^>]+>", "", name)).replace("​", "").strip()

        # Every <td>, including the bare ones a not-run test leaves behind.
        # Matching only <td class='...'> drops those and shifts every
        # column to their right onto the wrong emulator.
        cells = []
        for attrs, body in re.findall(r"<td([^>]*)>(.*?)</td>", tr, re.S):
            cls = re.search(r"class='([^']*)'", attrs)
            img = re.search(r"src='data:image/png;base64,([A-Za-z0-9+/=]+)'", body)
            cells.append((cls.group(1) if cls else "NONE",
                          base64.b64decode(img.group(1)) if img else None))
        if len(cells) != len(emulators):
            raise SystemExit("row %r has %d cells for %d emulators - the table "
                             "layout has changed" % (name, len(cells), len(emulators)))
        rows.append((name, cells))
    return updated, emulators, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", help="read a saved copy instead of downloading")
    ap.add_argument("--only", action="append", default=[],
                    help="slug to extract (repeatable); default is all")
    ap.add_argument("--list", action="store_true", help="list emulators and exit")
    args = ap.parse_args()

    if args.html:
        with open(args.html, encoding="utf-8", errors="replace") as f:
            page = f.read()
    else:
        sys.stderr.write("fetching %s\n" % URL)
        with urllib.request.urlopen(URL) as r:
            page = r.read().decode("utf-8", "replace")

    updated, emulators, rows = parse(page)
    slugs = [slugify(e) for e in emulators]
    if args.list:
        for e, s in zip(emulators, slugs):
            print("%-24s %s" % (s, e))
        return 0

    images = manifest_images()
    unknown = [t for t, _ in rows if t not in images]
    if unknown:
        sys.stderr.write("not in the manifest, skipping: %s\n" % ", ".join(unknown[:5]))

    wanted = set(args.only)
    total_files = total_bytes = 0
    for col, (emulator, slug) in enumerate(zip(emulators, slugs)):
        if wanted and slug not in wanted:
            continue
        root = os.path.join(BASELINE_ROOT, slug)
        index = ["# Acid Tests baseline - captured GB frames, 160x144 PNG.",
                 "# emulator: %s" % emulator,
                 "# generated: %s" % updated,
                 "# source: %s" % URL,
                 "# name|status|frames|image"]
        written = odd = 0
        for test, cells in rows:
            if test not in images or col >= len(cells):
                continue
            status, data = cells[col]
            if not data:
                continue
            if png_size(data) != (160, 144):
                odd += 1
                continue
            rel = images[test]
            path = os.path.join(root, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            index.append("%s|%s|0|%s" % (test, status, rel))
            written += 1
            total_bytes += len(data)
        if not written:
            continue
        os.makedirs(root, exist_ok=True)
        with open(os.path.join(root, "baseline.txt"), "w", newline="\n",
                  encoding="utf-8") as f:
            f.write("\n".join(index) + "\n")
        total_files += written
        print("%-24s %3d frames%s" % (slug, written,
                                      "  (%d not 160x144, skipped)" % odd if odd else ""))

    print("\n%d frames, %.1f MB" % (total_files, total_bytes / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
