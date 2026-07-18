# SuperFX / GSU timing — snes9x vs Mesen2

Investigation notes for the **Winter Gold** skier-garble bug (upstream snes9x
issue #533) and, more generally, why snes9x's SuperFX emulation drifts out of
coherence where Mesen2 (and real hardware) stay clean.

> **RESOLVED — see [`winter-gold-glitch.md`](winter-gold-glitch.md).** The fix
> (in tree) is a cycle-accurate GSU cost model (fxinst/fxemu, Mesen2-derived
> costs, master-cycle budget per scanline) plus a one-frame publish-visibility
> shim for this title's race-start CPU↔GSU handshake. The historical analysis
> below documents the investigation.
>
> **CORRECTION — GSU timing is NOT the cause of the Winter Gold bug (#533).**
> See [`winter-gold-glitch.md`](winter-gold-glitch.md) for the full root-cause
> investigation. In short: the "root cause" claimed below — snes9x running the GSU
> in one per-scanline lump instead of interleaved — was **implemented and A/B
> tested, and does NOT change the garble.** A cycle-interleaved catch-up (GSU run
> in step with the CPU at H-event / DMA / register-access granularity) produced
> **pixel-identical** output to the legacy per-scanline batch, so GSU *execution
> timing* (batched, interleaved, or even fully cycle-accurate) makes zero
> difference, and Mesen2's cleaner result is **not** due to its GSU interleaving.
>
> The actual mechanism (see [`winter-gold-glitch.md`](winter-gold-glitch.md) for
> the full evidence): the skier is an **OBJ sprite displayed from the GSU-rendered
> `$52E0` framebuffer** (OBJ base word `$4000`; skier tiles `$10D-$159` → VRAM
> `$50D0-$5590`, inside the triple-buffered `$52E0`/`$3C60`/`$6960` framebuffer
> DMA'd from `$70` GSU RAM). The garble is a **wrong / spread skier pose** in that
> framebuffer + OAM, rendered on the cycles where hardware/Mesen render the correct
> pose. The pose is driven by an animation **cel** the game computes from state that
> lands **differently in snes9x than on hardware** because of sub-frame CPU/IRQ/GSU
> timing — so the GSU rasterizes a wrong pose regardless of its own *timing* (which
> is why GSU interleaving/clock changes never help). It's a **timing/pose
> divergence**, not a GSU-execution-granularity issue.
>
> The architectural comparison below remains accurate as *documentation of how
> snes9x and Mesen2 differ in their GSU model*, and is worth implementing for
> general SuperFX accuracy — but it is **not** the fix for #533.

## The symptom

*Winter Gold* (Europe) — SuperFX/GSU-2 ("FX SKIING NINTENDO 96"). At the
demo→race switch (skate pose → tuck pose) in downhill skiing, the player
character renders **garbled** (splayed limbs, detached floating sprite tiles).

- **Mesen2 / real hardware:** the sprite is blank for **~1 frame**, then clean.
- **snes9x:** the sprite is garbled/incoherent for **~20–40 frames** (intermittent
  — each new tuck pose re-garbles until *its* tiles have finished uploading).

Ruled out during the investigation (do not chase these again):
- **RNG / animation cel value** — Mesen lands cel 2 at the switch and still
  renders clean; the garble does not depend on the RNG quarter.
- **Sprite arrangement (OAM) timing** — the cel/arrangement commit lag is
  identical in both emulators (2 game-frames).
- **GSU clock speed** — verified `snes9x_overclock_superfx` = 100 vs 800 is
  *actually applied* (`Settings.SuperFXClockMultiplier`, libretro.cpp:329) yet the
  garble is byte-identical. The game is display-paced, not GSU-throughput-bound;
  clock speed only changes whether the GSU "keeps up," not the coherence.

The garble is a **frame-to-frame coherence drift**: the sprite's OAM references
tuck tiles that are not yet in VRAM, because snes9x's GSU/CPU/DMA/PPU pipeline is
not interleaved finely enough for the tile-generation timing to stay locked.

## snes9x's SuperFX model (the cause)

snes9x runs the GSU in **coarse per-scanline batches**, not interleaved with the
CPU:

- `cpuexec.cpp` `HC_HCOUNTER_MAX_EVENT` (scanline end): if `!SuperFX.oneLineDone`,
  call `S9xSuperFXExec()` — one whole scanline's worth of GSU instructions in a
  single synchronous lump.
- `fxemu.cpp:143 S9xSuperFXExec()` → `FxEmulate(speedPerLine * mult / 100)`.
  `speedPerLine` is a fixed instruction budget (`S9xResetSuperFX`, the "5823405
  magic number"), **not** a cycle count tracked against the master clock.
- `fxemu.cpp` `$3030`(GO)/`$301F` writes also run a full-scanline batch
  synchronously (the only "catch-up on access" snes9x has).

Consequences:
- Between scanline ends the GSU is **frozen** from the CPU's point of view. A CPU
  read/DMA/PPU access mid-scanline sees a **stale** GSU state (RAM contents,
  `SFR` flags, screen buffer) that is up to a full scanline behind.
- The GSU runs a fixed *instruction* budget regardless of the actual sub-scanline
  cycle position, so its progress relative to the CPU is lumpy and phase-wrong.
- No CPU-side bus arbitration, no GSU stall-on-contention, no ROM/RAM access
  latency (see below) — all are absent.

For Winter Gold's tile pipeline (GSU renders tuck tiles → CPU/DMA moves them to
VRAM → PPU displays), this lumpiness means the DMA repeatedly moves
stale/incomplete tiles for ~20–40 frames until everything happens to line up.

## Mesen2's SuperFX model (the reference)

Source: `Core/SNES/Coprocessors/GSU/` in SourMesen/Mesen2.

### 1. Fine-grained interleaving — the key difference

`SnesMemoryManager::Exec()` advances the master clock **2 cycles at a time** and,
on every step, syncs the coprocessors:

```cpp
// SnesMemoryManager.cpp:206
void SnesMemoryManager::Exec() {
    _masterClock += 2;
    ...
    _cart->SyncCoprocessors();   // -> Gsu::Run()
}
```

```cpp
// Gsu.cpp:89
void Gsu::Run() {
    uint64_t targetCycle = _memoryManager->GetMasterClock() * _clockMultiplier;
    while(!_stopped && _state.CycleCount < targetCycle) {
        Exec();                  // one GSU instruction
    }
    if(targetCycle > _state.CycleCount) {
        Step(targetCycle - _state.CycleCount);
    }
}
```

So the GSU is caught up to the **exact current master cycle every 2 CPU cycles**.
The GSU, CPU, DMA and PPU stay in lockstep — at any access the GSU's state is
current. This is what snes9x lacks; it is the root of the Winter Gold coherence
drift.

### 2. CPU-side bus arbitration (open bus when the GSU owns the bus)

When the GSU is running and owns ROM/RAM (`SCMR` bits RON=`GsuRomAccess`,
RAN=`GsuRamAccess`), the SNES CPU does **not** read real cart data:

```cpp
// GsuRamHandler.h — CPU read of Game Pak RAM
uint8_t Read(uint32_t addr) {
    if(!_state->SFR.Running || !_state->GsuRamAccess) return _handler->Read(addr);
    return 0;   // open bus (TODO in Mesen: real open bus)
}
```

```cpp
// GsuRomHandler.h — CPU read of Game Pak ROM
uint8_t Read(uint32_t addr) {
    if(!_state->SFR.Running || !_state->GsuRomAccess) return _romHandler->Read(addr);
    if(addr & 0x01) return 0x01;
    switch(addr & 0x0E) { case 4: return 0x04; case 0x0A: return 0x08;
                          case 0x0E: return 0x0C; default: return 0; }
}
```

snes9x always returns the *real* ROM/RAM data regardless of GSU ownership. (Note:
for Winter Gold's IRQ vector at `$00:FFEE/FFEF` this pattern coincidentally yields
`$010C`, the real vector, so the IRQ path is unaffected — but the general behavior
differs. During the WG race the only CPU accesses to GSU-owned ROM were the IRQ
vector fetches, and there were 0 to GSU-owned RAM, so this arbitration is *not*
the Winter Gold cause — but it is a real accuracy gap.)

### 3. GSU stalls when it does not own the bus

```cpp
// Gsu.cpp:385
void Gsu::WaitForRomAccess() { if(!_state.GsuRomAccess) { _waitForRomAccess = true; _stopped = true; } }
void Gsu::WaitForRamAccess() { if(!_state.GsuRamAccess) { _waitForRamAccess = true; _stopped = true; } }
// UpdateRunningState(): _stopped = !SFR.Running || _waitForRamAccess || _waitForRomAccess;
```

The GSU halts until the SNES CPU hands the bus back (by clearing RON/RAN via
SCMR). snes9x has no GSU-stall concept.

### 4. ROM/RAM access latency

```cpp
// Gsu.cpp:428 Step() — GSU ROM/RAM reads/writes take real cycles
if(_state.RomDelay) { ... RomReadBuffer = ReadGsu(...); }   // RomDelay = 5 or 6 cyc
if(_state.RamDelay) { ... WriteGsu(...); }                   // RamDelay = 5 or 6 cyc
```

The GSU's ROM/RAM operations consume cycles (`ClockSelect ? 5 : 6`), which shapes
how fast it produces tiles. snes9x's fixed instruction budget ignores this.

## What snes9x is missing (summary)

| Behavior | Mesen2 | snes9x |
|---|---|---|
| GSU↔CPU interleave granularity | every 2 master cycles (`SyncCoprocessors` → `Run()`) | once per scanline (batch) |
| GSU advance metric | catch up to master **cycle** count | fixed **instruction** budget/line |
| CPU reads ROM/RAM while GSU owns it | open-bus pattern | real data |
| GSU stalls when CPU owns the bus | yes (`WaitForRom/RamAccess`) | no |
| GSU ROM/RAM access latency | yes (`RomDelay/RamDelay`) | no |

The **Winter Gold garble is caused by #1** (coarse batching → pipeline coherence
drift). #2–#4 are additional accuracy gaps that a proper rework would also close.

## Fix directions

1. **Proper fix — cycle-interleaved GSU (matches Mesen2 / hardware).** Give the
   GSU a master-cycle-based `CycleCount` and a `Run()`-to-current-cycle catch-up,
   invoked frequently (ideally per CPU memory access, like `SyncCoprocessors`, or
   at least far finer than per-scanline). Add CPU-side bus arbitration, GSU
   stall-on-contention, and ROM/RAM access latency. **Large, high-regression-risk**
   (re-test Yoshi's Island, Star Fox, Stunt Race FX, Doom, Dirt Trax FX, etc.) —
   this is why #533 has stayed open.

2. **Targeted partial fix — finer catch-up at sync points.** Keep the batch model
   but add a "run the GSU up to now" catch-up before the events that need
   coherence: CPU/DMA reads of GSU-produced data, and the PPU render point.
   snes9x already catches up on the `$3030`/`$301F` kick; extending catch-up to
   cart-memory accesses would reduce (not necessarily eliminate) the drift.
   Cheaper, lower-risk, but uncertain and less accurate.

3. **Cosmetic (rejected for this game).** Hiding the skier's OBJ band during the
   upload window works mechanically but requires a ~20–40 frame blank, far longer
   and more intrusive than hardware's 1-frame blank — not acceptable.

## References

- snes9x: `fxemu.cpp` (`S9xSuperFXExec`, `FxEmulate`, `S9xResetSuperFX`),
  `cpuexec.cpp` (`HC_HCOUNTER_MAX_EVENT`), `fxinst.cpp`, `getset.h`.
- Mesen2: `Core/SNES/Coprocessors/GSU/{Gsu.cpp,Gsu.h,GsuRamHandler.h,
  GsuRomHandler.h,GsuTypes.h}`, `Core/SNES/SnesMemoryManager.cpp` (`Exec`,
  `SyncCoprocessors`).
- Upstream: snes9xgit/snes9x#533.
