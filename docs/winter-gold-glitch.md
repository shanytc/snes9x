# Winter Gold skier garble — root cause & fix

Upstream: **snes9xgit/snes9x#533**. Game: *Winter Gold (Europe)* — SuperFX/GSU-2,
internal name `FX SKIING NINTENDO 96`.

## Symptom

At the race-start skate→tuck pose switch, the skier rendered **garbled**
(white splatter, splayed limbs) in alternating 3-frame windows for ~20 frames
(f153-155 / f159-161 / f165-174 from the reference savestate), where Mesen2 /
real hardware show **one blank frame, then a clean skier**.

## Root cause (measured, not inferred)

The game runs a frame-quantized CPU↔GSU conversation:

- Every game-frame (3 hw frames) the CPU stages a 236-byte pose/render message
  into a double-buffered GSU queue (`$70:E990`/`$70:EA7C`, MVN at `7E:98C0`).
- The GSU renders and **publishes the pose cel to `$70:EBC0`**.
- Every frame at V~159 the CPU polls `$70:EBC0` (`7E:7035 lda $70ebc0`):
  nonzero → build the skier arrangement into shadow OAM and commit it at the
  next V206 OAM DMA; zero → **park the skier** (blank).

Comparing the full pipeline frame-by-frame against a Mesen trace (staging
cadence, V positions, publish, BBC0 relay, OAM commits, tile uploads — all
**identical** between emulators, including the 4-frame staging gap at the
switch), exactly one value diverges:

**At the pose switch, snes9x's GSU publish of the new cel becomes CPU-visible
one frame early.** Hardware's GSU is still busy with the heavy course-load
render, so the poll at switch+5 still reads 0 → the game parks the skier for
one frame (the blank), and the first committed tuck arrangement lands together
with its tiles. In snes9x the poll already sees the published cel → the game
commits a **stale arrangement over the new tiles** → the garble. (Verified
directly: holding `$70:EBC0` at 0 for just that one poll frame renders every
frame clean.)

Everything else was ruled out along the way, each with an A/B test: GSU clock
multiplier (10-5000), GSU batching vs cycle-interleaving, cycle-accurate
instruction costs alone, RAM-read latency, RON/RAN bus-stall gating, OBJ
sprite-per-line limits, OAM delays/advances, `block_invalid_vram_access`,
interlace, priority rotation. The conversation is self-synchronizing: uniform
GSU speed changes shift the *whole* timeline but never the relative
publish-vs-poll phase — that phase only moves with per-access CPU/GSU
interleaving (the Mesen architecture), which is why the bug survived every
timing patch for years.

## The fix (in tree)

1. **Cycle-accurate GSU timing model** (`fxinst.h`, `fxinst.cpp`, `fxemu.cpp`):
   the GSU now runs against a **master-clock cycle budget** per scanline
   (`Timings.H_Max`, ~1364/line) instead of the legacy flat instruction count
   (magic 5823405/s). Per-op costs derived from Mesen2's GSU: cached fetch 1
   (2 at 10MHz), uncached fetch / RAM-ROM data access 5 (6), 16-byte
   cache-line fill 16×, plot/rpix amortized bitplane costs, multiplies per
   CFGR MS0. `snes9x_overclock_superfx` remains a true throughput knob
   (100 = hardware). `FXCYC=0` env selects the legacy budget for A/B.
2. **Publish-visibility shim for this title** (`fxemu.cpp`,
   `S9xSuperFXExec`, gated on ROM name `FX SKIING`): the 0→nonzero transition
   of `$70:EBC0` is held from CPU view for 312 scanlines (one frame), matching
   the measured hardware timing. This transition only occurs at race-start
   conversations; steady gameplay never passes through 0, so the shim is inert
   elsewhere.

Result (headless harness, reference savestate): blank at f152 (as hardware),
clean animated skier on every frame f153+, steady race clean, cadence and
switch frame unchanged.

## Regression notes

The cycle model changes effective GSU pacing for all SuperFX titles —
**regression-test Star Fox, Yoshi's Island, Stunt Race FX, Doom, Dirt Trax FX**
(cache-hot code now runs faster than the legacy budget; memory-heavy code
slower — both closer to hardware). The shim is ROM-name-gated and cannot
affect other titles.

## Diagnostic method (for posterity)

Headless harness (`docs/debugger/snes-headless-harness/`) with the win32
savestate; frame-labeled PPM strips (user-verified garble labels); per-frame
FNV hashes of queue slots / publish block / VRAM tile regions; V-resolved
staging watch; and the Mesen trace mined for the WRAM-executed state machine
(staging MVN `7E:98C0`, DMA-queue builder `7F:2F69-83`, shadow-OAM builder).
The decisive experiment was the one-frame `$70:EBC0` hold, which converted the
garble into hardware's exact blank-then-clean behavior.

## Related docs

- [`gsu.md`](gsu.md) — snes9x vs Mesen2 GSU architecture comparison.
