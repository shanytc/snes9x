---
name: gb-headless-harness
description: A no-GUI command-line driver for the snes9x `sgb/` GB/GBC/SGB core. Reproduce and trace audio / CPU / timing bugs from the shell — APU register-write trace, per-frame audio energy, hang detector, PC-range watcher, and one-binary run-mode comparison (cgb/sgb2/sgb/dmg).
---

# GB headless diagnostic harness

`harness.cpp` drives the real GB core (`sgb/gb_*.cpp`) directly, with **no Windows
or SNES dependencies**. Use it when a GB/GBC/SGB bug is hard to inspect through
the GUI debugger — it gives you ground truth on the command line, scriptable and
diff-able, and it reproduces exactly what `Emulator::RunCycles` does (interrupts
inside `Cpu::Step`, HDMA inside `PpuStep`, double-speed APU scaling, and the
per-run-mode boot register values from `Emulator::Reset`).

It does **not** link `sgb/sgb.cpp` (that file pulls in `snes9x.h` / `memmap.h` /
`ppu.h`). Instead it re-implements the ~30-line run loop and reset sequence. The
APU `dbg_*` counters it reads are the same ones the Win32 APU panel
(`S9xSGBGetApuState`) surfaces.

## Build

Raw GB/GBC ROM only — unzip first. From the repo root:

```sh
# (optional) extract a ROM from a .zip
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); \
  n=[x for x in z.namelist() if x.lower().endswith(('.gb','.gbc'))][0]; \
  open('/tmp/rom.gbc','wb').write(z.read(n))" "win32/Roms/Your Game.zip"

g++ -O2 -std=c++17 -Isgb \
  docs/debugger/gb-headless-harness/harness.cpp \
  sgb/gb_apu.cpp sgb/gb_cart.cpp sgb/gb_cpu.cpp sgb/gb_joypad.cpp sgb/gb_mbc.cpp \
  sgb/gb_memory.cpp sgb/gb_ops.cpp sgb/gb_ops_cb.cpp sgb/gb_ppu.cpp sgb/gb_timer.cpp \
  -o /tmp/gb_harness
```

(The `fread` / unused-result warnings from the compiler are harmless.)

## Usage

```
gb_harness <rom.gb|.gbc> [frames=1500] [mode=cgb|sgb2|sgb|dmg] [input|noinput] [watchLoHex watchHiHex]
           [press=F:BTN,...] [fb=F:path.ppm,...] [fbl=F:path.pgm,...] [wav=path.wav] [mute=NN..]
           [trig3=LO:HI] [apuw=LO:HI] [hostpace=FPS] [drc]
```

| arg     | meaning |
|---------|---------|
| `rom`   | raw ROM path (unzip first) |
| `frames`| SNES-frames to run (~60/sec of game time) |
| `mode`  | boot register state the cart sees: `cgb` (A=$11, GBC), `sgb2` (A=$FF), `sgb` (A=$01, 4.295 MHz clock), `dmg`. Running the same ROM in two modes isolates mode-specific divergence. |
| `input` | pulse START then A every 120 frames (to walk past logos / "press start"); `noinput` (default) leaves the pad idle — use this to observe automatic title behaviour |
| `watch` | optional PC range; the harness counts instructions executed inside it per frame (e.g. `38F2 3925` to watch a sound routine) |
| `press` | scripted input, overrides `input`/`noinput`: `press=1260:A,1500:START,2700:D+A` holds each combo for 8 frames starting at frame F. Buttons: `A B START SELECT U D L R`. Use it to navigate menus deterministically (e.g. reach a game mode that triggers a bug) |
| `fb`    | screenshot dumps: `fb=3000:/tmp/f3000.ppm,4100:/tmp/f4100.ppm` writes the screen as PPM (CGB color or DMG grayscale) after frame F runs — see which screen/menu the game is on |
| `fbl`   | dump the per-pixel layer map (P5 PGM, 0=BG / 100=window / 200=OBJ) after frame F — pair with `fb=` to classify which layer flickering pixels belong to when picking a frame-blend layer (3D Pocket Pool multiplexes balls on OBJ *and* shadows on BG, so its auto-blend entry needs layer ALL) |
| `wav`   | dump the mixed APU output (48 kHz stereo S16) to a WAV for offline spectral / waveform analysis, or to diff against a reference emulator's capture (SameBoy builds headless with a ~70-line driver) |
| `mute`  | mask channels out of NR51 each CPU step: `mute=124` solos CH3, `mute=3` mutes CH3. Re-applied continuously, so the game's own NR51 writes can't unmute. Pair with `wav=` to isolate which channel carries an artifact |
| `trig3` | print CH3 trigger forensics for frames `[LO,HI)`: trigger count, PC, freq, pre-trigger playback position, sample latch, and the full 16-byte wave RAM. This is the tool for wave-RAM streamers (3D Pocket Pool rewrites all 16 bytes ~7×/frame and retriggers at pos 16) |
| `apuw`  | for frames `[LO,HI)`, log every APU register write tagged with its frame (`APUW f<N> pc=… NRxx old->new`) and force a per-frame CH3 detail line (`enabled/dac/length/nr32/freq/pos/trigcount/peak`). Correlates a button press with the exact register burst it triggers and shows whether an enabled CH3 actually outputs — Altered Space's jump SFX has CH3 enabled+volume-on+freq-swept yet `peak=0` because the game never loads wave RAM and relies on the power-on pattern (fixed in `ApuReset`) |
| `hostpace` | instead of draining the ring fully each frame, drain `48000/FPS` samples — replicates how the Win32 host consumes audio when its frame pacing differs from the GB's 59.7275 Hz. Overproduction pins the ring (PushSample drops), shortfall starves the device. `SUMMARY` reports `ringFullFrames` |
| `drc`   | enable the mirror of `sgb.cpp`'s audio dynamic-rate control (PI loop steering `ApuSetClockHz` to hold the ring at 1/8 fill). The integrator is seeded at the mode's frame-lock steady state (≈ +0.0063 for DMG/SGB2/CGB, ≈ −0.0174 for SGB1's fast clock), mirroring `DrcSteadyStateCorr` in `sgb.cpp`. With `hostpace=60.0988` the corr holds near the seed and `ringFullFrames` stays 0. Under `hostpace` every 60th frame (and any short drain) prints a `PACE fN fillPre want got corr` line — the ring-health trace that exposed the Animaniacs/Mulan LCD-off crackle |

## What the output tells you

Per-frame line (printed for the first frames, every 120th, the last, and any
frame with audible output or watch-range activity):

```
[f  713] pc=0280 instr=7576 DS=0 stop=0 halt=0 ime=0 watch=4089 |
         M=1 audPeak=8465 audAvg=2404 | CH1(en1 f000 v10) CH2(en1 f000 v10)
         CH3(en0) CH4(en0) tg=1627/1627/0/0
```

* `pc / instr` — CPU PC at frame end, instructions run that frame (low + a
  dominant single PC in the histogram ⇒ busy-loop / hang).
* `DS / stop / halt / ime` — double-speed engaged, STOP, HALT, IME.
* `watch=N` — instructions inside the watched PC range this frame.
* `M` — APU master enable. `audPeak / audAvg` — **actual mixed output energy**
  (`ApuDrain`). This is the key diagnostic: a channel can read `en1` with a
  volume yet produce **`audPeak=0`** — i.e. enabled but silent.
* `CHx(en f v)` — enabled, 11-bit freq, env volume. `tg=a/b/c/d` — running
  trigger (NRx4 bit 7) counts.

The `APUW pc=.... NRxx XX->YY` lines (first 400 changes) are the
register-write trace: every APU register change with the PC that caused it.

The `SUMMARY` block gives first-trigger / first-non-zero-freq frames, final
counters, total audio (`framesWithAudio`, `globalPeak`), and the hottest PCs on
the final frame (hang detector).

## Worked example — the Telefang silent-voice bug

*Keitai Denjuu Telefang – Power Version* plays a digitized-voice jingle at the
title (a brief blocking "hang", then the title animates). It was silent in
snes9x but plays in Mesen, in **both** CGB-bios-less and SGB2.

```sh
/tmp/gb_harness /tmp/rom.gbc 900 sgb2 noinput 38F2 3925
```

The trace showed the voice routine at `$38F2` firing ~4100×/frame and writing
real PCM samples to NR12/NR22, the channels reading `en1`, **yet `audPeak`
collapsing to 0** after a couple of frames:

```
[f708] watch=3930 audPeak=8350  CH1(en1 f000 v8)   <- voice audible
[f711] watch=4089 audPeak=0     CH1(en1 f000 v8)   <- SILENT (volume still moving!)
[f713] watch=4089 audPeak=0     CH1(en1 f000 v10)
```

Root cause: the routine plays each 4-bit PCM nibble via NR12 (env volume) then
**re-triggers at freq $000**. At freq $000 the duty period is 8192 T-cycles but
the per-sample loop is ~248, so `duty_pos` is frozen; output =
`DUTY_TABLE[duty][duty_pos] × env_volume` only passes the volume when the frozen
bit is 1 (duty 2 / NR11=$80 → position 0 = 1). `TriggerSquare` wasn't resetting
`duty_pos`, so ISR gaps drifted it onto a 0-bit and it stuck. Fix: reset
`c.duty_pos = 0` on trigger (`sgb/gb_apu.cpp`). After the fix the same frames
sustain `audPeak` ~4000-9000.

The harness makes regressions checkable too — `framesWithAudio` / `globalPeak`
before vs after a change, across `cgb` / `sgb2` / `dmg`, confirm a fix adds the
missing audio without altering existing output.

## Notes / limits

* No SGB packet bus, ICD2 joypad bridge, or SNES-side mixing — it is the **GB
  core in isolation**, which is exactly what isolates GB-core bugs from the SGB
  glue. `mode` only sets boot registers + clock, not the SNES handshake.
* `input` mode mashes START/A blindly; for a specific sequence, edit the
  `JoypadSet` block.
* Temporary tool — build it under `/tmp`, don't commit binaries.
