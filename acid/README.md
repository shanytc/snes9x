# acid/ — GB Emulator Shootout test suite

Vendored copy of the test ROMs, reference screenshots and test definitions
from the [GB Emulator Shootout](https://tomek.rekawek.eu/GBEmulatorShootout/)
(github.com/trekawek/GBEmulatorShootout, commit
`da23b88682fa422bb5c699055892b7fd3eb8300f`, 2026-07-22). These drive the
**Emulation → Acid Tests** menu item in the win32 port and the headless
`sgb/tests/acid_test` runner.

## Layout

- `manifest.txt` — one test per line: `name|model|runtime|rom|pass_images|fail_images`.
  Image lists are `;`-separated, empty pass list = info-only test.
- `acid/`, `blargg/`, `daid/`, `ax6/`, `mooneye/`, `samesuite/`, `ashiepaws/`,
  `cpp/`, `mealybug/` — ROMs plus their reference PNGs, kept in the upstream
  suite layout.
- `defs/` — the upstream Python test definitions the manifest was generated
  from (`make_manifest.py` re-generates it).

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

The manifest's reference PNGs are one particular emulator's idea of correct.
A baseline is a second opinion: a folder of captured frames to diff a run
against, either ours from an earlier build or another emulator's dump.

    ./acid_test --save-baseline ../../out/base     # write our frames there
    ./acid_test --baseline ../../out/base          # diff this run against them
    ./acid_test --baseline ../../acid              # or against the references

In the dialog the Baseline button saves the shown frames, loads a folder to
compare against, or clears the comparison. A Baseline column then reads
SAME, DIFF *n* px, MISSING or `-`, the preview pane stacks our frame over
the baseline's, and the Show filter grows entries for what differs or is
missing. Comparison uses the same rule as a pass: grayscale, 50/255 per
pixel.

Images are named after the test with a trailing `.gb`/`.gbc` dropped, so
`blargg/halt_bug.gb` is saved as `blargg/halt_bug.png` — the layout the
suite's own reference images already use. That means `acid/` itself works as
a baseline with no index, and so does any foreign dump laid out the same
way. Ours also carries a `baseline.txt` index recording the emulator, the
timestamp and each test's verdict, which wins over the derived path when it
is present.
