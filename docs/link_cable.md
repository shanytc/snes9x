# Game Boy Link Cable

Everything about the link cable support on branch `link_cable_support`
(PR #232, grew out of issue #116). Two to four players, two transports:
separate emulator instances linked over loopback TCP, or split screen —
every player inside one window, one process, no sockets.

Last updated 2026-08-15.

---

## 1. User view

### Menus

Emulation > **Game Boy Link Cable** (visible when a GB game is loaded, or
always on a spawned instance):

- **Link Same Game** >
  - **2 Players** — the direct cable. Launches a second instance with the
    same ROM (or reuses a surviving one) and links to it.
  - **3 Players / 4 Players** — this instance hosts an emulated DMG-07
    Four Player Adapter and seats that many instances, launched one at a
    time.
  - **Split Screen** — a checkable preference. While checked, the
    2/3/4 Players items run that many Game Boy cores inside *this* window
    instead of spawning instances. Persisted as `[SGB] LinkSplitScreen`.
- **Link Other Game** — 2-player direct cable, the launched instance
  starts empty so the user picks the other cartridge (cross-cartridge
  play: Pokémon Red↔Blue↔Yellow and friends).

Clicking the active (checked) item disconnects. While a session is live,
the other shape items are greyed; the split toggle only flips between
sessions.

A **spawned instance** never chooses the session shape. Its menu is
rebuilt into plain **Connect / Disconnect**: Connect dials into whatever
session exists (idempotent — a rejoin request can never unplug it),
Disconnect unplugs only that seat.

### Requirements

- A Game Boy game must be loaded (SGB or BIOS-less path).
- SGB1 BIOS mode is refused — the Super Game Boy 1 has no link port; use
  SGB2 or BIOS-less.
- **Split screen requires BIOS-less GB mode** (Emulation > BIOS > No
  BIOS). The SGB BIOS modes run the Game Boy through the SNES core,
  which is single-instance global state.
- TCP sessions use loopback port `[SGB] LinkPort` (default 8765, BGB's
  default). Loopback-only by design: nothing is exposed, no firewall
  prompt.

### Inputs

Player N plays on the **Joypad #N binding set** — automatically: a
spawned instance knows its player index from its launch switch, a split
seat is fed its joypad slot each frame. Binding sets mix keyboard keys
and specific gamepad buttons; enable Joypad #2..#4 in Input
Configuration. (Same infrastructure as SNES multitap play; two players
on one keyboard is subject to the keyboard's ghosting limits.)

### Saves

- Battery: player 1 `game.sav`, players 2-4 `game.sav2/3/4` — keyed on
  the player index for the whole process lifetime (not on the link being
  up), in both the Saves dir and the ROM-dir migration fallback. No
  player ever falls back onto another player's file; a first-run player
  with no file starts from power-on `$FF` SRAM.
- Savestates (TCP sessions): slots are tagged `_p1`..`_p4` while linked,
  and a slot save/load is relayed to every instance so a linked session
  is one save. The `.oops` dump is per-instance.
- Savestates, rewind and run-ahead are **blocked during split screen**
  (their snapshots carry only the primary core).

### Session behaviors worth knowing

- Pausing any instance holds the whole session (a silent cable would
  read as unplugged). Linked instances force background-input on and
  pause-when-inactive off for the session, restored on unlink.
- The master's players tick follows the live session: close one window
  of a 4-player session and the check slides to 3 Players. When every
  spawned instance has been closed, the session folds itself and the
  menu clears.
- Changing the ROM folds a split session first (seat batteries are saved
  under the outgoing game's names).

---

## 2. Architecture overview

Three layers, all under `sgb/`:

```
gb_link.cpp     transport: loopback TCP, BGB 1.4 packet framing, 1-3 peer seats
gb_serial.cpp   the GB serial port + BGB protocol + DMG-07 adapter + split link
sgb.cpp         core facade; split-screen core manager (S9xSGBSplit*)
win32/wsnes9x.cpp   session choreography: spawning, pids, relays, menus
```

### 2.1 Transport (`gb_link.cpp`)

- Non-blocking TCP on loopback, single-threaded, driven from `LinkPoll()`.
  8-byte BGB packets (4 command bytes + LE u32), per-channel rx/tx rings
  (4 KB each), TCP_NODELAY (an 8-byte sync packet behind Nagle would add
  40 ms per byte), SO_LINGER-0 close so the well-known port never parks
  in TIME_WAIT.
- **Role sensing**: first instance up wins the `bind()` and serves;
  the loser dials in. The kernel arbitrates, so there is only ever one
  host. (Takeover: port taken but nothing answering = a dying instance;
  bind again.)
- **Seats**: a direct cable is one peer on channel 0; a hub serves up to
  three channels, seat k = player k+2. Arrivals take the lowest free
  seat; extras are accepted-and-closed (honest refusal). On a hub a
  dropped seat frees for re-dial; on a direct cable one side leaving
  ends it for both. A per-channel generation counter prevents a
  drop-and-refill inside one poll from inheriting the old occupant's
  handshake.

### 2.2 Serial port + BGB protocol (`gb_serial.cpp`)

The shift register itself (SB/SC at $FF01/$FF02 — the registers stay in
`Memory`, whose fields the savestate serializes individually) plus the
BGB 1.4 protocol (https://bgb.bircd.org/bgblink.html):

- master (SC=$81): send `sync1` → 8 bit periods (512 T-cycles each;
  16 at CGB fast clock) → peer's `sync2` lands in SB; bounded wait
  (500 ms, 20 ms once stalled) so a frozen peer can't hang us.
- slave (SC=$80): arm, wait for `sync1`, answer `sync2`, complete.
- `sync3` doubles as empty-ack (b2=1: nothing armed, wire idles $FF) and
  timestamp carrier (b2=0). Version/status handshake per connection.
- Unlinked, the port keeps the original stub behaviour bit-for-bit.

Two hard-won timing rules (both exist because **emulators run in frame
bursts** — a whole frame of emulated time executes in a couple of
wall-clock milliseconds, then the process sleeps; any design that
assumes wall time ≈ emulated time breaks):

1. **Held clocks are a queue, not a slot.** A burst can deliver many
   master clocks while the slave's game had zero cycles to re-arm; they
   queue in order (32 deep, overflow acks the oldest away — a slow game
   misses those bytes on hardware too) and every clock gets exactly one
   answer.
2. **Queued clocks complete no faster than ~1.015 ms of emulated time**
   (`kExtByteGap` = 4257 T-cycles — the fastest a DMG-07 ever spaces
   bytes: 0.887 ms gap + 0.128 ms shift). Games keep a single SB mailbox
   consumed by the main loop between bytes; zero-gap delivery overwrites
   it and the game's packet framing slips until reset (the garbled
   F-1 Race racer-entry screen).

### 2.3 DMG-07 Four Player Adapter (3-4 player TCP sessions)

Protocol per Pan Docs ("4-Player Adapter") and GBE+'s `dmg07.cpp` — do
not guess at it, the references are exact. Summary:

- The adapter owns the clock; every Game Boy (host's included) is an
  external-clock slave. Wire semantics: a GB's byte during transfer N is
  its answer to byte N-1.
- **Ping phase** (~17 ms/packet, 4 bytes clustered ~1.55 ms apart):
  `$FE STAT STAT STAT`, STAT = presence bits 4-7 | port id in bits 0-2.
  Presence = `$88` acks on wire transfers 2-3. Player 1's replies to
  STAT2/STAT3 set RATE and SIZE (SIZE clamped 1-4).
- **Entering transmission**: player 1 replies `$AA` — at the packet edge
  the adapter emits one packet of `$CC` and switches; the fourth `$AA`
  rides on the first `$CC` transfer, exactly as the Pan Docs chart shows.
- **Transmission**: SIZE×4-byte packets, byte-to-byte
  0.887 + (RATE>>4)·0.106 ms, packet floor (17 + (RATE&15)) ms. Each
  player's fresh data lands in the half of the double buffer not being
  broadcast — a one-packet delay that is part of the protocol. Empty
  ports read as zeroes.
- **Restart**: ≥3 consecutive `$FF` from player 1 → one all-`$FF`
  packet → back to ping, presence cleared.

Emulation shape: the instance that picks 3/4 players hosts the adapter
and is always player 1. Its own GB completes in-process; every remote
seat is driven over the *ordinary BGB wire* with the adapter as
permanent master — so spawned instances run the unmodified two-player
code (and, in principle, any BGB-protocol client can take a seat).

**The adapter never blocks** (frame bursts again — a fixed reply window
of any length misses). Every `sync1` carries a per-seat sequence number;
the peer echoes it in its answer; the host matches it against a FIFO of
tags frozen at send time (`{seq, presence-ack slot, buffer store
index}`). Skipped sequences (a peer flushed its queues on a state load)
are skipped past instead of shifting every later answer onto the wrong
byte; stale answers fall away. This is sound because everything that
*steers* the adapter — RATE, SIZE, the `$AA` switch, the `$FF` restart —
comes from player 1, which is in-process and synchronous; remote answers
only feed presence bits and the data buffer, and the protocol's own
one-packet delay is the latency budget.

### 2.4 Split screen (in-process, no sockets)

- Seats 2-4 are additional `SGB::Emulator` cores (the class was always
  instantiable; only the singleton wiring is global), owned by the
  `S9xSGBSplit*` section of `sgb.cpp`, loaded with the primary's ROM.
- **Lockstep**: `S9xSGBSplitRunFrame` replicates `RunFrame`'s budget +
  first-VBlank frame-lock + audio-DRC tail, but interleaves all cores in
  456-cycle scanline slices — the link always peeks a peer within one
  line. Every timing problem of the socket path structurally cannot
  occur.
- **Local wire** (`SerialSplitAttach`): two seats = symmetric direct
  cable — when a master's countdown completes, it peeks the peer core
  and completes both sides on the spot. Three/four seats = the same
  DMG-07 state machine with `g_hub.local`: `HubSendByte` completes every
  armed seat directly and interprets their bytes one event later (the
  hardware wire semantics), no FIFOs, no pending queues, no pacing —
  those are latency machinery.
- **Video**: per-core `BlitScreenGB(dest + offset, pitch)` tiles
  GFX.Screen — 320×144 side by side, 320×288 as a 2×2 grid (3 players
  leave the fourth quadrant black); both fit the existing 512×478
  buffer. `CalculateDisplayRect` uses the canvas's own square-pixel
  aspect during split (all render backends route through it).
- **Audio v1**: player 1 only; seat APU rings overflow-drop silently
  (the ring drops on full by design). The mixer already sums multiple
  sources with per-source gains (SPC-over-GB, MSU-1, Voicer-kun), so a
  full mix with per-player sliders is a natural follow-up.
- **Multi-core invariants** (violating any looks like emulation bugs):
  - The SGB command callback is process-global and routes to the
    primary → seat packet decoders set `PacketState.mute_commands`
    (survives `PacketReset`) or a seat's game recolors player 1.
  - `Memory.dma_last` / `dma_vram_bypass` were file-scope globals in
    gb_memory.cpp; they are per-console fields now. (Savestates
    serialize `Memory` members individually, so growing the struct is
    safe; `MbcState` is struct-dumped — never grow that one casually.)
  - Seats load the ROM with a **null path** so `CartLoad`'s index-less
    ROM-dir `.sav` self-seed can't fire; their batteries go through the
    split facade (`base .sav path + "2"/"3"/"4"`), hooked into the same
    `LoadSRAM`/`SaveSRAM` sites as everything else.
  - Resets fan out (`S9xSGBReset` / `S9xSGBSoftReset`) so the consoles
    power-cycle together and the local adapter restarts from ping.

### 2.5 Win32 session choreography (`wsnes9x.cpp`)

- Launch switch: `-gblinkpeer=<pid>,<launcherIdx>,<ownIdx>,<players>` —
  the pairing is mutual and a hub seat keeps its player number, which is
  also its Joypad binding set and its save-file index.
- **Sequential seating**: the hub launches the next instance only once
  the previous one is on the port, so transport seat order always equals
  launch order equals player number. Surviving windows are reused via a
  posted **Connect** (idempotent — no toggle race).
- Pause and savestate-slot relays carry the originator's pid; only the
  hub host forwards to the other seats (peers only know the host), which
  is what prevents relay loops. The known-pid list is scoped by session
  shape so instances remembered from an earlier session of the other
  kind never receive another session's savestates.
- The master's tick counts instances actually present (alive pids +
  linked seats + seats not yet brought up); the session auto-folds when
  every seated instance's process is gone (an instance that merely
  unlinked keeps it alive — its window can reconnect).

---

## 3. Design lessons (why it is built this way)

1. **Frame bursts break every fixed reply window.** Emulated time and
   wall time decouple completely inside a frame. Anything that waits a
   fixed wall interval for a cross-process answer will sometimes get
   100% misses. Answers must be matched asynchronously (sequence tags)
   and interpreted positionally, with tolerance budgeted in *emulated*
   time (the DMG-07's one-packet delay).
2. **Delivery pacing is part of hardware fidelity.** Games time their
   serial handlers against the wire's minimum byte spacing; delivering
   queued bytes back-to-back corrupts single-mailbox handlers even
   though no byte is lost.
3. **Hub-with-host-as-master beats mesh.** The original "BGB protocol
   can't carry 4 players" concern dissolved once the adapter sat on one
   side of every wire: each host↔seat link is an ordinary two-party BGB
   session with a permanent master.
4. **Every clock gets exactly one answer** — the invariant that keeps
   the host's per-seat accounting in step across bursts, timeouts,
   overwrites and state loads.
5. **In-process lockstep deletes the whole problem class.** The split
   path needs none of the above machinery: peek the peer, complete both
   sides, done.

---

## 4. Hardware background

- More than 2 players only ever existed through the **DMG-07** (bundled
  with F-1 Race), and it is same-game lockstep — the adapter broadcasts
  one stream all cartridges must interpret identically. Verified
  library: F-1 Race, Faceball 2000, Wave Race (SIZE=1 — exercises the
  negotiation differently than F-1 Race's SIZE=4), Yoshi's Cookie,
  Super R.C. Pro-Am, Top Rank Tennis, Trax; plus less-well-sourced
  JP titles (Kunio-kun Daiundōkai, Super Momotaro Dentetsu, etc.).
- Pokémon (all GB/GBC generations) is strictly 2-player; different
  cartridges of a generation inter-link pairwise — that is Link Other
  Game, never a hub scenario.
- Faceball 2000's legendary 16-player mode is **not** DMG-07 — it used a
  proprietary cable made solely for that game; the dormant code is still
  in the ROM. See `docs/Faceball-16Player.md` for the investigation
  notes.
- The adapter is game-agnostic and so are we: a wrong cartridge dialing
  into a session simply never acks `$88` and reads as an absent player.

---

## 5. Known limitations

TCP sessions:
- Loading a savestate or hard-resetting the host restarts the adapter
  from its ping phase; games renegotiate.
- Pause is not refcounted across 3-4 instances: one seat unpausing
  clears the link-pause everywhere.
- Savestates mid-transmission are best-effort (the 2-player caveat,
  more visible at four).

Split screen:
- Audio is player 1 only (mix mode is a planned follow-up).
- Savestates, rewind and run-ahead are blocked during a session.
- AVI capture records the player-1 viewport only.
- BIOS-less GB mode only.

---

## 6. References

- BGB link protocol: https://bgb.bircd.org/bgblink.html
- Pan Docs, 4-Player Adapter: https://gbdev.io/pandocs/Four_Player_Adapter.html
- GBE+ DMG-07 implementation: https://github.com/shonumi/gbe-plus/blob/master/src/dmg/sio.cpp (`dmg07.cpp`)
- shonumi, "Edge of Emulation: Game Boy 4-Player Adapter": https://shonumi.github.io/articles/art9.html
- Our PR: https://github.com/shanytc/snes9x/pull/232
