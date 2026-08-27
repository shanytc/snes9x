# acid/ — GB Emulator Shootout test suite

Vendored copy of the test ROMs, reference screenshots and test definitions
from the [GB Emulator Shootout](https://tomek.rekawek.eu/GBEmulatorShootout/)
(github.com/trekawek/GBEmulatorShootout, commit
`da23b88682fa422bb5c699055892b7fd3eb8300f`, 2026-07-22). These drive the
**Emulation → Acid Tests** menu item in the win32 port and the headless
`sgb/tests/acid_test` runner.

## Layout

ROMs and screenshots share one set of relative paths but live under separate
roots, so a second opinion is just another folder beside `default/`:

    acid/tests/blargg/halt_bug.gb
    acid/baseline/default/blargg/halt_bug.png
    acid/baseline/supersnes9x/blargg/halt_bug.png

- `manifest.txt` — one test per line: `name|model|runtime|rom|pass_images|fail_images`.
  `rom` is relative to `tests/`, images to `baseline/<name>/`. Image lists are
  `;`-separated, empty pass list = info-only test.
- `tests/` — the ROMs in the upstream suite layout, plus the sources daid
  ships with them.
- `baseline/default/` — the reference screenshots the suite ships. These are
  what decides PASS and FAIL, so the manifest's `pass_images` and
  `fail_images` always resolve here however many other baselines exist.
- `baseline/<anything else>/` — a set of frames to diff against, ours from an
  earlier build or another emulator's. Every folder found here becomes its
  own column; dropping one in is all it takes.
- `defs/` — the upstream Python test definitions the manifest was generated
  from (`make_manifest.py` re-generates it).
- `fetch_baselines.py` — pulls a baseline per emulator off the shootout's
  published table (`--list` to see them, `--only <slug>` to pick).

## Comparison semantics (upstream-compatible)

A test passes when a captured frame matches a pass screenshot: both sides are
grayscale, and no pixel may differ by more than 50/255 (PIL ITU-R 601-2 luma
convert, same as upstream's `test.py`). Each test runs for its per-test
`runtime` plus 1 s startup and a 5 s margin, checking every frame and stopping
early on a match. No joypad input is ever fed. DMG/SGB tests compare the
2-bit shades as `(3-idx)*85`; CGB tests compare the BGR555 output expanded
with `(c<<3)|(c>>2)`.

Total: 264 tests (167 DMG, 94 CGB, 3 SGB). The three tests without pass
screenshots upstream (`which.gb` on DMG+CGB, `rom_and_ram.gb`) are reported
as INFO, matching the shootout.

## Running

- GUI: Emulation → Acid Tests. The filter bar narrows the list by name,
  suite, model and result; Run covers whatever is shown, and Export writes
  that set as `.txt`, `.json` or `.html`.
- Headless: `cd sgb/tests && make acid_test && ./acid_test [acid-dir]
  [options]`. `--dump` writes failing frames as PPM into `_failures/`;
  `results.txt` gets a full per-test report.

## Filtering and reports

The runner takes the same cuts as upstream's `main.py`:

    ./acid_test --suite blargg --suite mooneye     # upstream --test
    ./acid_test --model CGB                        # DMG, CGB or SGB
    ./acid_test --filter halt                      # name substring
    ./acid_test --list                             # what would run
    ./acid_test --suites                           # suite names and counts

Suite names are matched case-insensitively as substrings, so `--suite
mealybug` finds `mealybug-tearoom-tests`. Filters combine: suites and models
are OR-ed within themselves and AND-ed with each other and the name filter.

Reports come in three formats, from `--txt`/`--json`/`--html` on the CLI or
the dialog's Export button:

- **txt** — the per-test table with a per-suite breakdown, the format
  `results.txt` has always used.
- **json** — the test metadata (name, suite, model, runtime, ROM, reference
  images) plus each verdict, covering upstream's `--dump-tests-json`.
- **html** — a standalone report in the spirit of upstream's `build.py`:
  summary cards, per-suite scores and a filterable table with every captured
  frame embedded as a PNG. One file, no external assets; a full 264-test run
  comes to about 640 KB.

## Baselines

`baseline/default/` is one particular emulator's idea of correct. A second
baseline is a second opinion: a folder of captured frames to diff a run
against. Every folder under `baseline/` is picked up automatically and gets
its own column, `default` included, so adding one means creating a folder
and nothing else.

    ./acid_test                                    # diffs against every baseline
    ./acid_test --save-baseline ../../acid/baseline/supersnes9x
    ./acid_test --baseline ../../out/somewhere-else # one more, from anywhere

In the dialog, Save Baseline writes the shown frames as a new one, Rescan
picks up a folder added since, and Diagnosis opens a table of how the shown
tests came out against every baseline — same, differ, missing, n/a, not run
— which is more than a status line can hold once there are twenty of them.
Choosing a row there puts that baseline's frame in the preview. Each
baseline's column reads:

| | |
|---|---|
| `SAME` | matches, within the 50/255 tolerance |
| `DIFF n px` | differs, with the pixel count |
| `MISSING` | this baseline has no image for a test that has a reference |
| `n/a` | the test defines no reference at all, so none is expected |
| `-` | not run yet, so there is no frame of ours to compare |

The preview pane shows one frame at a time — ours, then each baseline that
has one — named on the image and stepped with `<`, `>` or a click on the
frame. They land on the same pixels, so flipping makes a difference blink
out rather than having to be hunted for between two smaller images. The Show
filter grows entries for what differs from or is missing from any baseline.
Comparison uses the same rule as a pass: grayscale, 50/255 per pixel.

### Other emulators

`fetch_baselines.py` fills `baseline/<emulator>/` from the shootout's
published table, which carries a screenshot for every emulator/test pair
inlined as base64 — so the whole set comes down in one page fetch rather
than thousands of requests. 19 emulators, 4439 frames, 4.1 MB.

    python fetch_baselines.py --list            # slugs on offer
    python fetch_baselines.py                   # all of them
    python fetch_baselines.py --only sameboy --only bgb

Frames are filed under the test's own name rather than the manifest's
`pass_images`, because two tests can share one of those — `bully.gb` on DMG
and on GBC both point at `ashiepaws/bully.png` — and one frame would
overwrite the other.

A test an emulator was not run on leaves a bare `<td>No result</td>` in the
table. Those still take a column, so the parser has to count them; matching
only `<td class=...>` drops them and slides every emulator to their right
onto the wrong column. The check that catches it is the published score:
extracted PASS + INFO and the cell total should equal the `(n/m)` in each
header, and they do for 18 of 19. The odd one is No$gmb, whose header says
205 where its own row cells only carry 202 results.

Agreement tracks each emulator's pass count closely, which is what it should
do if we are right — we agree with them on the tests they get right. Coffee
GB 261 of 264, Emulicious 252, docboy 255, SameBoy 249, bgb 202, mGBA 150,
down to ares 23. Where we part from Coffee GB it is twice on informational
ROMs, which have no correct answer, and once on `ppu_scanline_bgp`, which
the manifest gives three acceptable screens — they draw one, we draw
another, both pass.

Saving into a folder that already holds frames asks first, naming how many
would be replaced and who wrote the index it is about to overwrite. Saving
into a folder that has a `manifest.txt` is refused outright: that is the
suite root, not a baseline.

Images are named after the test with a trailing `.gb`/`.gbc` dropped, so
`blargg/halt_bug.gb` is saved as `blargg/halt_bug.png` — the same relative
path `baseline/default/` uses, which is why any foreign dump laid out that
way works unchanged. Thirty tests keep their reference somewhere the name
does not predict (`m3_bgp_change_dmg_blob.png`), so lookup also tries the
images the manifest names. Ours additionally carries a `baseline.txt`
recording the emulator, the timestamp and each verdict, which wins over the
derived path when present.
