---
name: harness
description: Overview of the headless diagnostic harnesses under docs/debugger — which one owns which core, how to build and drive them, how the SGB visual regression suite works, and the traps that produce false results.
---

# Headless harnesses — overview

`docs/debugger/` holds two no-GUI drivers for the emulator cores plus a visual
regression suite. They exist so a bug can be reproduced, traced and diffed from
the shell: scriptable input, frame dumps, savestates, memory pokes and peeks,
with no window and no sound device.

This page is the map. Each harness has its own README with the full flag
reference and worked examples; read that once you know which one you want.

| | [`snes-headless-harness/`](snes-headless-harness/README.md) | [`gb-headless-harness/`](gb-headless-harness/README.md) |
|---|---|---|
| Drives | the full SNES core through `libretro/libretro.cpp` | the `sgb/gb_*.cpp` GB/GBC core directly |
| Covers | SNES games, enhancement chips, **SGB BIOS mode** | GB/GBC and BIOS-less SGB |
| Links | the whole core + libretro glue | ~10 GB files, no `snes9x.h` |
| Build | ~1 min (static lib, then instant relinks) | seconds |
| Reach for it when | video / DMA / HDMA / timing / cart mapping | GB audio / CPU / timing |

The dividing line is the 65816. SGB **BIOS mode** runs the real SGB boot ROM on
the SNES CPU, so it belongs to the SNES harness even though the game is a GB
cart. The BIOS-less GB path is the GB harness's.

## Which core does a ROM belong to

`snes_regression.py`'s `rom_class()` is the authority: `.sfc`/`.smc` and any
zip containing one are `snes`; `.gb`/`.gbc` are `sgb` when run through BIOS
mode. A title shipping as both `.smc` and `.zip` is deduplicated by stem so the
two don't collide on one output name.

## SNES harness — build and drive

Two steps, so core edits relink instantly (full recipe in its README):

```sh
# 1. core -> static lib (redo per-file after core edits)
#    add -DUNZIP_SUPPORT -DZLIB and link unzip/ + win32/zlib/src to load .zip ROMs
ar rcs /tmp/sn_obj/libsnes.a /tmp/sn_obj/*.o

# 2. harness (instant)
g++ -O2 -std=gnu++17 ... docs/debugger/snes-headless-harness/harness.cpp \
    /tmp/sn_obj/libsnes.a -o /tmp/snes_harness -lm
```

```
snes_harness <rom> [frames=N] [log=N] [press=F:BTN[:HOLD],...] [fb=F:out.ppm,...]
             [cgram=F:out.ppm] [save=F:path] [load=path] [loadat=F:path]
             [poke=F:ADDR:VAL] [opt=key:value] [apuspd=N] [trace=LO:HI] [reg=LO:HI]
```

`fb=` is the workhorse: dump a frame, look at it. `cgram=` is the first thing to
check when graphics render black or mis-coloured — tiles are usually fine and
the palette is gone. `load=`/`loadat=` skip long boots; `loadat=` restores mid-run,
which reproduces rewind-pop and in-session load-state bugs that `load=` cannot.

Build notes that cost time if missed:

* `-std=gnu++17`, not `c++17` — strict mode hides `strdup`/`fdopen` on cygwin.
* Use explicit `C:/...` paths. The harness is a cygwin binary but the invoking
  shell may be git-bash, and their `/tmp` mappings differ.
* Zip support needs `-I unzip -I win32/zlib/src` on **both** the core and the
  harness link, plus the `gz*` objects; `zlib.h`/`unzip.h` not found and
  `undefined reference to gzopen` are the two errors you will hit in order.
* Then the bundled zlib and minizip still fail to link under cygwin, because
  both assume Win32/MSVC:
  * `undefined reference to _wopen` — `gzguts.h` turns on its `WIDECHAR` path
    for `__CYGWIN__`, and cygwin has no `_wopen`. Link a stub; `WIDECHAR` only
    gates `gzopen_w()`, which the harness never calls. Do **not** reach for
    `-U__CYGWIN__` — it makes cygwin's own `<sys/fcntl.h>` redefine
    `struct flock` and the build dies further from the cause.
  * `undefined reference to fopen64` / `fseeko64` / `ftello64` — build
    `unzip/ioapi.c` and `unzip/unzip.c` with `-DMINIZIP_FOPEN_NO_64`, which
    maps them to the un-suffixed calls cygwin does provide.

## SGB visual regression suite

`snes_regression.py` boots every SNES/SGB ROM in `win32/Roms/`, samples several
frames, keeps the busiest as the baseline PNG in `regression_images/`, and on a
later run reports every ROM whose screen moved.

```sh
python snes_regression.py --update              # record baselines
python snes_regression.py                       # check
python snes_regression.py --update --filter X   # bless one improvement
```

It stores **images, not hashes**, deliberately: a hash only says a screen
changed, so a game that was already broken hashes as "unchanged" and reads as a
pass. The folder doubles as a visible catalogue of what boots. Changed ROMs
write their new screen to `regression_images/_current/` so old and new sit side
by side. Needs `SGB2.sfc` reachable via `--bios-dir`, `$SNES9X_SYSTEM_DIR`, or
`./BIOS`.

### Reading a result honestly

A `CHANGED` line is a question, not a verdict. Three things produce changes that
have nothing to do with the code under test:

* **A differently-built harness.** Baselines captured from a build with a
  different file set or flags can shift boot timing, so an animated intro is
  sampled at another phase. Both screens are valid; only the moment differs.
* **Stale SRAM.** The harness writes `.srm` next to the ROM. Run build A then
  build B and B loads the save A just wrote, which is a genuine behaviour
  difference caused entirely by run order. Delete the `.srm` between runs of
  different binaries, or the comparison is worthless.
* **Sampling phase.** The busiest-frame heuristic can pick a different frame of
  an attract loop between runs that differ in boot timing by a few frames.

The way to settle it is an **A/B on identical flags**: build the pre-change
commit in a `git worktree`, build the current tree with the *same* command line,
run both over the suspect ROMs and compare the PPMs byte-for-byte. If they match,
the code is exonerated and the baseline is simply stale. Confirm the ROM is
deterministic first by running one binary twice — if that alone differs, nothing
downstream means anything.

**Known non-deterministic: the SFC-Box merged containers** (`pss61+62_merged`,
`pss61+63_merged`, `pss61+64_merged`). Repeating `--filter pss61` on one
unchanged binary reported, in order: `pss61+64`, none, `pss61+62`+`pss61+63`,
none. The delta is a single colour (`4279c6` vs `3961c6`) on scattered pixels
with geometry identical, never a structural difference. Treat a `CHANGED` on
these as noise unless the geometry moved; re-run before investigating.

## Trace hooks

`cgram-trace.patch` adds gated printf hooks to `ppu.cpp` and `dma.cpp` for
CGRAM writes (with V-counter, H-cycle and source: `cpu`/`dma`/`hdma`/`gdma`) and
DMA kicks. Apply it only while diagnosing — the shipped tree stays clean:

```sh
git apply docs/debugger/snes-headless-harness/cgram-trace.patch
# rebuild ppu.o + dma.o, build the harness with -DHARNESS_TRACE_HOOKS
```

The same discipline applies to any temporary instrumentation: gate it behind an
env var, and take it back out before committing.

## Related

* [`snes-agent.md`](snes-agent.md), [`gb-agent.md`](gb-agent.md),
  [`debugger-agent.md`](debugger-agent.md) — task-oriented playbooks.
* [`mapper.md`](mapper.md) — cart mapper notes.
* `gb_boot_check.py` — quick boot check across many GB ROMs.
