# DMG-07 Four Player Adapter — implementation notes and protocol findings

Companion to [link_cable.md](link_cable.md), which describes the link
cable and adapter as they are *used*. This file is the working record
behind the adapter emulation: why it is shaped the way it is, what the
public references leave out, and what it cost to find that out. Read it
before changing anything in the `Dmg07`/`Hub*` section of
`sgb/gb_serial.cpp`.

Branch: `link_cable_support`. Menu path: Emulation → Game Boy Link
Cable → Link Same Game → 2/3/4 Players.

## 1. Architecture

The instance that picks 3 or 4 players hosts an emulated DMG-07 and is
always player 1. Every Game Boy on the session — including the host's
own — runs as an external-clock slave. The host's console is completed
in process; each remote seat is clocked over an ordinary BGB link with
the adapter as permanent master, so spawned instances run the
unmodified two-player code and never learn a hub is on the other end.
In principle any BGB-protocol client can take a seat.

Points that are not obvious from any one file:

- **Seat order is identity.** win32 spawns seats sequentially
  (`GBLinkPumpSpawns`: launch seat k only once k-1 has attached), so
  seat order equals player number equals the Joypad #N binding set
  equals the `_pN` savestate tag. Launch switch:
  `-gblinkpeer=pid,launcherIdx,ownIdx,players`.
- **The adapter never blocks.** Emulators run in frame bursts, so a
  fixed reply window of any length misses; the first attempt ("wait
  briefly for engaged seats") produced no presence and no sync. Every
  `sync1` carries a per-seat sequence number; the peer echoes it; the
  host matches it against a per-seat FIFO of tags frozen at send time
  (`{seq, presence-ack slot, buffer store index}`). Skipped sequences —
  a peer that flushed its queues on a state load — are skipped past
  rather than shifting every later answer onto the wrong byte.
  This is sound because everything that *steers* the adapter comes from
  player 1, which is in process and synchronous; remote answers only
  feed presence bits and the data buffer, and the protocol's own
  one-packet broadcast delay is the latency budget.
- **The slave-side pending byte is a queue**, 32 deep and ordered
  (`Serial.pending_*`), not a single slot: a host frame burst delivers
  many clocks while the peer's game had no cycles to re-arm, and the
  game then drains them one re-arm at a time. Overflow acks the oldest
  away — a slow game misses those bytes on hardware too. Every `sync1`
  gets exactly one answer; that invariant is what keeps the FIFO in
  step.
- **Queue delivery is paced**: at most one passive completion per 4257
  T-cycles (≈1.015 ms, the adapter's fastest byte cadence). Instant
  back-to-back delivery garbled F-1 Race's racer-entry screen on seat
  instances — games use a single `SB` mailbox consumed by the main loop
  between bytes, and some re-arm `SC` before reading `SB`, so a
  zero-gap burst overwrites bytes and slips the game's framing until
  reset. A full drain still fits inside a packet: 16 × 4257 < 71298.
- **Pause/freeze fan-out**: `WM_GBLINK_*` carry the originator pid in
  `lParam` and only the hub host forwards, which is what prevents relay
  loops. `GBLinkKnownPids` is scoped by session shape so pids
  remembered from an earlier session never receive another session's
  relays.

## 2. Protocol

### 2.1 The documented baseline

Pan Docs' [4-Player Adapter](https://gbdev.io/pandocs/Four_Player_Adapter.html)
page has the wire chart; GBE+'s `src/dmg/dmg07.cpp` is the field-tested
reference implementation. Do not guess when these two cover something.

Ping packet is `$FE STAT STAT STAT`; presence is `$88` acks on wire
transfers 2-3; player 1's replies carry RATE and SIZE; an `$AA` train
switches the adapter into a `$CC` sync packet and then transmission,
which runs SIZE×4-byte packets with each player's fresh data landing
one packet behind. A console's byte during transfer N is its answer to
byte N-1.

### 2.2 Interpretation rule: armed replies only

The hub must never latch a protocol-steering value from an idle wire.
Trump Boy II beacons `$AA` once per frame from its title screen as a
cable slave; a single-`$AA` begin-sync rule made the adapter thrash
ping → sync → net → restart every ~170 ms (RATE latched `$FF` from the
idle wire, and idle wires counted toward the restart run), so the game
never saw clean pings, concluded there was no adapter, and fell back to
cable-parent masters.

Settled rules: begin-sync needs three *consecutive armed* `$AA` (idle
resets the run); RATE and SIZE latch from armed replies only; the
restart counts armed `$FF` only, where idle neither counts nor resets.
GBE+ never meets this class of bug because its model only advances on
armed player-1 transfers.

### 2.3 Restart: exactly three

Three consecutive armed `$FF` from player 1, not GBE+'s looser
two-per-packet rule. Jantaku Boy's command blocks are `[cmd,FF,FF,00]`
— the `$FF` bytes are filler — and the all-`$FF` restart packet is that
game's own abort signal (`DI` + jump to reset at `$0408`). Tested both
ways: the two-`$FF` rule made the game bail to reset the moment its
parent console sent a command.

What the `$FF` *pair* means is the subject of the next section.

### 2.4 Undocumented behaviour

The references are complete enough for the games they were derived
from: the ones that use the adapter as a broadcast pipe and stream at a
fixed rate. Jantaku Boy does not. All four consoles run the full mahjong
simulation in lockstep and exchange one-byte signals, so every step is a
rendezvous where all four must observe the same byte in the same window
— and there is no retry, no timeout, no resync. It depends on three
adapter behaviours no public source describes.

These were recovered from the game's own machine code, which is now the
only surviving description of these adapter states. shonumi's hardware
capture does show the wake — the ["some random looking
data"](https://shonumi.github.io/articles/art9.html) that follows the
sync packets — but F-1 Race ignores it, so it was never decoded.

1. **Restart-wake.** A `[cmd,FF,FF,break]` block is a mid-session
   restart request. The adapter answers by re-announcing the
   transmission: one `[FD,FF,FF,FF]` window. Games park with the LCD off
   and only the serial interrupt enabled waiting for it, and no console
   can produce it — the proof is mechanical: at that moment all four
   consoles, the requester included, sit in the same spin, and the ROM's
   only other `$FD` writer belongs to a later game phase. Three or more
   `$FF` remains the documented quit flood. Read at packet size 1 only,
   so a streaming game's payload can never trip it.
2. **Length echo.** The byte after the window is a bulk length, and it
   is the requesting console's own command byte. It rides that
   console's reply slot, so the adapter must place it in slot 0 of the
   announcement and shield it from the replies still in flight (one-shot
   guard — the same index recurs as ordinary data during the bulk).
3. **Bulk relay.** The payload that follows is the requester's staged
   block, relayed contiguously and captured every transfer, not sampled
   once per packet per slot. The interleave that is correct for signal
   windows fills every console's mirror tables with three seats of
   unrelated bytes, which corrupts everything downstream of the deal.

Two consequences for the adapter's bookkeeping. The slot cycle
re-anchors when a bulk ends, because the bulk's length rotates every
console's 4-byte framing. The `$CC` acknowledgment is held for a few
packets before the announcement, because consoles reach their serial
wait at different times and the announcement is one-shot.

One in-process detail belongs here too: the split-screen hub holds a
byte event until every core has re-armed. A dropped byte permanently
rotates that console's reply cycle — real ~1 ms byte gaps never miss,
so nothing on hardware has to tolerate it.

## 3. Case study: Jantaku Boy (Namcot, 1991)

Reference map for future work; addresses are CPU addresses.

**Serial ISR** `$0284`, `$FE`-framed: counter `FF9A`, receive window
`FF9D[4]`, reply block `FFA1[4]`, packet length `FF9B`.

**Stages** (`FF99`): 0 → 1 on the all-`$CC` packet (`$0374`); 1 → 2 on
the wake (`$0395`, which sets `FFA8` and stages `C600`→`C300` with
length `FFA9`); bulk mode `$0345` streams `C300` out and `C200` in,
ending when the counter reaches `C200[0]` and clearing `FFA8` — that
clear is the release every blackout waits on. The only `FFA8` writers in
the ROM are `$03AE` (set, on the wake path) and `$036B` (clear, at bulk
end). The blackout itself is `$03E6`: LCD off, `IE=$08`, spin on
`FFA8`.

**Event system**: `C0B1` → dispatcher `$08C2` (table `$08D1`); event 3
→ macro-step `C0B2` (table `$09A7`), whose steps are the lobby
ceremony: 0 = `$0A08` stations, 1 = `$0AB2` roll call, 2 = `$0AE1`
roster (a 160-byte state sync), 5 = `$176B` in-game turn.

**Turn choreography**: `$0F` announce → `$6F` acks → the drawn tile
held in the slot (it guards the `$1C8C` all-zero barrier against
vacuous passes) → commit (`$2317`, A-edge) → all-zero rendezvous →
claim window → owner sends `$1F` (`$1901`) → `$7F` acks → tile
broadcast.

**Claim beacons are button-driven** (`$1DDC` reads held keys from
`FF95`: A = 01, LEFT = 02, DOWN = 03, RIGHT = 04, B = 06 kan) and they
*persist* until the next phase overwrites `FFA1`. The flickering boxes
on screen during that window are the claim prompt: presses there are
real game input, not taps to unstick a hang.

**Variables**: `C6A0`/`C6A1`/`C6A2` turn cursor, `C0A7` drawn tile,
`C0A5` the spectator's mirrored copy, `C77B` claim table, `FFA6`
barrier mask, `C0C0` mode (3 = lobby, 2 = in-game).

## 4. Method and tooling

Four wire-timing conjectures in a row regressed a working build. Every
fix that held came from reading the game's code instead. The procedure
that works:

> Disassemble the spin the consoles are stuck in. Find every writer of
> the variable it waits on. If no console can write it, the adapter owes
> it.

That single question produced all three undocumented behaviours above.

`SGB_LINK_TRACE=1` writes `sgb_link_trace_<pid>.log` containing adapter
events, per-console state lines, and PC-watch breakpoint hits (the watch
table is near the top of `sgb/gb_serial.cpp`; hooks install through
`Cpu::SetTraceHook`). The log lands in the process working directory,
which the ROM file dialog changes — look in the ROM folder, not next to
the executable.

Input facts worth keeping: GB A maps to the SNES B binding; `FF96` is
per-frame newly-pressed edges (bit 0 = A, bit 3 = Start, high nibble =
d-pad) while `FF95` is held state; the joypad scan lives in the VBlank
handler and stops inside blackouts, so a console parked in one cannot be
freed by pressing anything.

## 5. Do not retry

Each of these regressed a working build:

- **Hardware byte rate** (`6*RATE+512` at packet size 1) — caused
  primary-core byte drops in the split-screen lockstep.
- **Wake persistence gate** (require the request to repeat) — leaves the
  request block on the wire long enough for the game's own matchers to
  re-dispatch it; the log filled with wakes and repeated roll calls.
- **Serving the wake only when every core is armed** — strands the
  requester, which is the last to park.
- **Halving the packet floor** — changed nothing. The owner's lag at the
  end of a turn is CPU-side animation, not wire time.

Working configuration: original transmission pacing (71298-cycle
floor), wake fires on the first completed request frame, length preset
plus one-shot guard, bulk relay, arm grace 64.

Two theories that cost the most time before being disproved: "the game
needs SIZE=4 so its 4-byte blocks stay contiguous" (no — size 1 is
correct; the bulk relay is what makes blocks contiguous) and "the `$FD`
comes from a console" (no — that writer belongs to a later game phase).

## 6. Game notes

F-1 Race's multiplayer is adapter-only: header title `F1RACE`, and with
no adapter it reports "RACERS LINKED 1". Two-player sessions therefore
force-host the hub through the `S9xSGBCartNeedsDmg07` title allowlist in
`sgb/sgb.cpp`, threaded as `force_hub` through `SerialLinkAutoStart` and
`SerialSplitAttach`. Extend that list only with titles read out of real
ROM headers.

From ReyVGM's test sweep (issue #116), the common failure signature was
that player 1 — the in-process host — behaved differently from the
seats. Several of those games' manuals also demand a strict start order
and tell the player to retry, so re-test on a current build before
treating a report as a bug. Manuals confirm F1 Pole Position and Micro
Machines support a plain cable for two players, so neither belongs in
the adapter-only list.

## 7. Limitations accepted

Loading a savestate or hard-resetting the host restarts the adapter from
ping, so games must renegotiate. Pause is not refcounted across four
instances: one seat unpausing clears `PAUSE_LINK_PEER` everywhere.
`$AA`/restart detection watches player 1 only, matching GBE+.

Cosmetic debt: the split-screen snapshot masks LCD-off blackouts. Real
hardware shows blank screens there, which is a progress cue the player
can see and we currently hide.
