---
name: snes-headless-harness
description: A no-GUI command-line driver for the full snes9x SNES core, statically linked through the libretro glue. Reproduce and trace SNES video / DMA / HDMA / timing bugs from the shell — scripted input, PPM frame dumps, CGRAM swatch dumps, savestates, per-frame PPU/HDMA state log, and core-option / APU-speedup overrides.
---

# SNES headless diagnostic harness

`harness.cpp` drives the real SNES core (`cpu.cpp`, `ppu.cpp`, `dma.cpp`,
`memmap.cpp`, …) — the exact files the win32 build uses — with no window or
sound device. `libretro/libretro.cpp` serves as the port layer (init order,
`S9xMainLoop` scaffolding, savestates, joypad routing); the harness calls the
`retro_*` entry points directly and pokes core globals (`Registers`,
`Memory.FillRAM`, `PPU`, `DMA[]`) for its logs.

## Build

Two steps so core rebuilds are incremental (cygwin g++, from the repo root):

```sh
# 1. core -> static lib (once, ~1 min; redo per-file after core edits)
mkdir -p /tmp/sn_obj
printf '%s\n' \
  apu/apu.cpp apu/bapu/dsp/sdsp.cpp apu/bapu/smp/smp.cpp apu/bapu/smp/smp_state.cpp \
  bsx.cpp c4.cpp c4emu.cpp cheats.cpp cheats2.cpp clip.cpp conffile.cpp controls.cpp \
  cpu.cpp cpuexec.cpp cpuops.cpp crosshairs.cpp dma.cpp dsp.cpp dsp1.cpp dsp2.cpp dsp3.cpp dsp4.cpp \
  fxinst.cpp fxemu.cpp gfx.cpp globals.cpp memmap.cpp obc1.cpp msu1.cpp ppu.cpp stream.cpp \
  sa1.cpp sa1cpu.cpp screenshot.cpp sdd1.cpp sdd1emu.cpp seta.cpp seta010.cpp seta011.cpp seta018.cpp \
  sgb/sgb.cpp sgb/sgb_packet.cpp sgb/sgb_state.cpp sgb/gb_cpu.cpp sgb/gb_ops.cpp sgb/gb_ops_cb.cpp \
  sgb/gb_memory.cpp sgb/gb_mbc.cpp sgb/gb_ppu.cpp sgb/gb_apu.cpp sgb/gb_timer.cpp sgb/gb_joypad.cpp sgb/gb_cart.cpp \
  snapshot.cpp snes9x.cpp spc7110.cpp srtc.cpp tile.cpp tileimpl-n1x1.cpp tileimpl-n2x1.cpp tileimpl-h2x1.cpp \
  sha256.cpp bml.cpp movie.cpp fscompat.cpp libretro/libretro.cpp \
  hd64180.cpp sfcbox.cpp voicekun.cpp \
  | xargs -P 12 -I{} sh -c 'o=/tmp/sn_obj/$(echo {} | tr "/" "_" | sed "s/\.cpp$/.o/"); \
    g++ -O2 -std=gnu++17 -fno-rtti -w -DRIGHTSHIFT_IS_SAR -D__LIBRETRO__ -DALLOW_CPU_OVERCLOCK \
    -DHAVE_STDINT_H -DHAVE_STRINGS_H -DNDEBUG \
    -Ilibretro -Ilibretro/libretro-common/include -I. -Iapu -Iapu/bapu -c {} -o $o'
gcc -O2 -w -DNDEBUG -I. -c filter/snes_ntsc.c -o /tmp/sn_obj/snes_ntsc.o
ar rcs /tmp/sn_obj/libsnes.a /tmp/sn_obj/*.o

# 2. harness (instant)
g++ -O2 -std=gnu++17 -fno-rtti -w -DRIGHTSHIFT_IS_SAR -D__LIBRETRO__ -DHAVE_STDINT_H -DNDEBUG \
  -Ilibretro -Ilibretro/libretro-common/include -I. -Iapu -Iapu/bapu \
  docs/debugger/snes-headless-harness/harness.cpp /tmp/sn_obj/libsnes.a -o /tmp/snes_harness -lm
```

`-std=gnu++17` (not `c++17`) — strict mode hides `strdup`/`fdopen` on cygwin.
Use explicit `C:/...` paths for ROM/output args: the harness is a cygwin binary
but the invoking shell may be git-bash, and their `/tmp` mappings differ.

### Zip support (required by `snes_regression.py`)

`win32/Roms/` is almost all `.zip`, so the regression suite needs a zip-capable
build. Add `-DUNZIP_SUPPORT -DZLIB -Iwin32/zlib/src -Iunzip` to `CXXFLAGS`, add
`loadzip.cpp` to the core list, and compile the C sources into the same lib:

```sh
for z in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback \
         inffast inflate inftrees trees uncompr zutil; do
  gcc -O2 -w -DNDEBUG -Iwin32/zlib/src -c win32/zlib/src/$z.c -o /tmp/sn_obj/zlib_$z.o
done
# MINIZIP_FOPEN_NO_64: cygwin has fopen/fseeko/ftello, not the *64 spellings
for u in unzip ioapi; do
  gcc -O2 -w -DNDEBUG -DMINIZIP_FOPEN_NO_64 -Iwin32/zlib/src -Iunzip \
      -c unzip/$u.c -o /tmp/sn_obj/unzip_$u.o
done
```

One more link error remains: `undefined reference to _wopen`. `gzguts.h` enables
its `WIDECHAR` path for `__CYGWIN__`, but cygwin provides no `_wopen`. `WIDECHAR`
only gates `gzopen_w()`, which the harness never calls, so link a stub:

```c
#include <wchar.h>
int _wopen(const wchar_t *p, int f, ...) { (void) p; (void) f; return (-1); }
```

Do **not** try `-U__CYGWIN__` instead — cygwin's own `<sys/fcntl.h>` then
redefines `struct flock` and the build fails somewhere unrelated.

## Usage

```
snes_harness <rom.sfc> [frames=N] [log=N] [press=F:BTN[+BTN][:HOLD],...]
             [fb=F:path.ppm,...] [cgram=F:path.ppm,...] [save=F:path,...] [load=path] [loadat=F:path,...]
             [loadrom=F:path,...]
             [poke=F:ADDR:VAL,...] [opt=key:value,...] [apuspd=N] [trace=LO:HI] [reg=LO:HI]
buttons: A B X Y START SELECT U D L R LB RB
```

| arg      | meaning |
|----------|---------|
| `frames` | frames to run (default 600) |
| `log`    | print the state line every N frames (0 = only first/last) |
| `press`  | scripted input: `press=950:A,1550:START` holds each combo 8 frames (or `:HOLD` frames) starting at frame F |
| `fb`     | dump the rendered frame as PPM after frame F |
| `cgram`  | dump CGRAM as a 16×16 color swatch PPM **and** print all 256 entries as hex — first thing to check when graphics render "black" or wrongly colored (tiles usually fine, palette gone) |
| `oam`    | `oam=F,...` print a decoded OAM snapshot after frame F (per on-screen sprite: position, size, tile, palette, priority, flips, plus OBSEL) — the SNES side of the GB harness's OAM dump |
| `save`/`load` | savestate at frame F / restore at startup — skip long boot sequences while iterating (win32 `.000` states work after `gunzip`) |
| `loadat` | restore a savestate at frame F while the machine is running — reproduces rewind-pop / in-session load-state behavior, which `load=` (restore into a fresh boot) does not |
| `loadrom` | load another ROM at frame F through `Memory.LoadROM` — the win32 GUI's File→Open path over a live session. Use it to reproduce cart-swap bugs |
| `rom` | a `.zip` is passed to the core by path so it runs the GUI's FileLoader/unzip route (build the core with `-DUNZIP_SUPPORT -DZLIB` and link `unzip/` + zlib); any other extension is loaded from memory |
| `poke`   | write a WRAM byte at frame F: `poke=1000:746:5` sets $7E:0746=05 (ADDR/VAL hex) — force game-state variables while probing code paths |
| `opt`    | answer libretro core-option queries: `opt=snes9x_block_invalid_vram_access:disabled` |
| `apuspd` | override `Timings.APUSpeedup` after ROM fixes — sweep it to test timing-hack sensitivity |
| `trace`/`reg` | frame ranges for the CGRAM-write / DMA-register traces — need the hooks: `git apply docs/debugger/snes-headless-harness/cgram-trace.patch`, rebuild `ppu.o`+`dma.o`, and build the harness with `-DHARNESS_TRACE_HOOKS` |

The per-frame state line reads PPU registers straight from `Memory.FillRAM`:

```
[f  600] PC=80:B401 mode=1 INIDISP=0F TM=17 TS=00 CGWSEL=30 CGADSUB=00 HDMAEN=1E 256x224
         hdma1 -> $2121 mode=0 dir tbl=89:E1A7 cur=E262 ind=00:FFFF
```

plus one line per armed HDMA channel (B-bus target, transfer mode,
direct/indirect, table base, current table pointer, indirect pointer) — an
instant read on what per-scanline effects a screen uses.

## The trace patch

`cgram-trace.patch` adds gated printf hooks (`g_trace_cgram` / `g_trace_reg`)
to `ppu.cpp` and `dma.cpp`:

* every `$2121` (CGADD) / `$2122` (CGDATA) write with V-counter, H-cycle,
  source (`cpu` / `dma ` / `hdma` / `gdma` = the general-DMA fast path that
  bypasses `S9xSetPPU`), current CGADD and flip state;
* every `$420B` (DMA kick — with channel-0 source/target/length and the live
  `PPU.HDMA` mask) and `$420C` (HDMAEN) write with PC.

Apply it only while diagnosing; the shipped tree stays clean.

## Worked example — the Circuit USA black-menu bug

*Circuit USA (Japan)* (snes9xgit/snes9x#563): main menu renders as black torn
bars; fine in bsnes/Mesen. The harness run showed the glitch immediately
(`fb=600:...`), `cgram=` showed CGRAM almost entirely `$0000` — tiles present,
palette missing. The trace patch then showed why: the game uploads its 512-byte
menu palette in one `$2122` DMA, and with `Timings.APUSpeedup = 3` that upload
fires at V=97 **while the game's CGRAM-gradient HDMA channels are still
armed** — each HBlank the HDMA resets CGADD to 0, folding the whole 256-color
upload into entries $02–$79. The game's own code disables HDMA around the
upload and lands it in VBlank; the APU-speedup hack value decides who wins that
race. `apuspd=` sweep: speedup 2 → upload at V≥225 (VBlank) with `HDMA=00`,
menu pixel-perfect vs bsnes; 0/1/3 → mid-frame, palette destroyed. A
14000-frame `reg=` run confirmed every one of ~100 palette uploads lands clean
at speedup 2. Fix: `memmap.cpp`, hack value 3 → 2 (the original 2017 value;
the 2019 SMP-clock retune to 3 is what broke it — see issue #563).

## Notes / limits

* Core-option defaults apply (no RetroArch config); overscan crop etc. follow
  `libretro.cpp` defaults. Hires/interlace frames dump at their native size.
* No audio output; the APU still runs (games hang without it).
* Temporary tool — build under `/tmp`, don't commit binaries.

## SGB visual regression baselines (`sgb_regression.py`)

Boots every GB/GBC/SGB ROM through the SGB BIOS and keeps one PNG per ROM in
`regression_images/`. The folder doubles as an at-a-glance answer to "which games
boot?", which a framebuffer hash cannot give you — a title that was already broken
hashes as unchanged and reads as a pass.

```sh
python sgb_regression.py --update             # record baselines
python sgb_regression.py                      # check; changed ROMs -> _current/
python sgb_regression.py --update --filter X  # bless an improvement
```

Covers SNES ROMs and SGB-enhanced carts (cart flag `$0146 == 0x03`); plain
GB/GBC carts belong to `docs/debugger/gb-headless-harness` instead. SGB BIOS
mode needs the 65816, which is why the SGB half lives here.

Known noise: the SFC-Box carts (`pss61*`) seed their RTC from the host clock,
so their attract screen is not bit-reproducible and reports CHANGED on most
runs. Compare the image before believing it.
