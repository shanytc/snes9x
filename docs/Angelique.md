# Angelique Voice Fantasy — Voicer-kun audio CD (issue #169)

Koei's *Angelique Voice Fantasy* (SFC, 1996, HiROM 3MB + 32KB battery SRAM)
ships with an audio CD of recorded dialogue and the **Voicer-kun**
(ボイサーくん): an IR remote-control transmitter/receiver that plugs into
**controller port 2**. At voiced story moments the game sends learned
CD-player remote codes ("next track", "play", digit keys…) through the
Voicer so the player's own CD player speaks the line. No emulator supports
this today; this document records everything reverse-engineered from the
ROM and captured live in the headless harness, and is the spec for
emulating it: the game always knows the exact CD track number it wants, so
the emulator can play that track straight from the CD image.

Assets used here:

- ROM: `win32/Roms/Angelique - Voice Fantasy (Japan).zip`
- CD image: `win32/Roms/ANGELVOICE.zip` (`ANGELVOICE.BIN` 558,644,688
  bytes raw 2352-byte audio sectors + `ANGELVOICE.CUE`, 90 audio tracks,
  ~52.9 minutes)

## The key fact: track number = voice id + 2

The game's `PlayVoice(id)` routine at **`$CA4114`** does, verbatim:

```
track = id + 2            ; $CA4130: INC $0C / INC $0C
JSL $C60CEE (0, track)    ; hand the CD track number to the playback layer
```

Voice ids 0–88 map to CD tracks 2–90. Track 1 is a 3:45 spoken
announcement (not used by the game). So regardless of which remote-key
strategy the Voicer ends up transmitting, the **absolute track number is
computed first** — an emulator can hook here (or decode the IR key
presses) and always knows what to play.

`PlayVoice` is reached from the dialogue engine (bank C2, call site
`$C27AEA`, gated on scene state `$7E2416 == 3`) and from one bank-C8 site
(`$C8461D`). Per-voice status flags live in a WRAM array at
`$7E21CD + id`; the voice output mode is `$7E241B`
(0 = off, Voicer mode, manual mode — set by the setup wizard at
`$C932BF` / `$C939AC` and by the options code in banks C2/C5).

## Audio CD layout

90 audio tracks. Track 2 (the first story voice) is 43 s; most voice
tracks are padded to a uniform ~23 s, which is how the game can time
dialogue auto-advance without querying the CD player. All tracks have a
2-second pregap (INDEX 00) before INDEX 01.

| CD track | INDEX 01 (MSF) | Start | Length | Voice id |
|---|---|---|---|---|
| 1 | 00:00:00 | 0:00.00 | 224.6 s | n/a (announcement) |
| 2 | 03:46:46 | 3:46.61 | 43.0 s | 0 |
| 3 | 04:31:47 | 4:31.63 | 23.0 s | 1 |
| 4 | 04:56:50 | 4:56.67 | 23.3 s | 2 |
| 5 | 05:21:70 | 5:21.93 | 23.2 s | 3 |
| 6 | 05:47:13 | 5:47.17 | 23.5 s | 4 |
| 7 | 06:12:49 | 6:12.65 | 23.1 s | 5 |
| 8 | 06:37:59 | 6:37.79 | 23.5 s | 6 |
| 9 | 07:03:20 | 7:03.27 | 23.1 s | 7 |
| 10 | 07:28:24 | 7:28.32 | 23.3 s | 8 |
| 11 | 07:53:45 | 7:53.60 | 23.0 s | 9 |
| 12 | 08:18:48 | 8:18.64 | 23.3 s | 10 |
| 13 | 08:43:69 | 8:43.92 | 25.4 s | 11 |
| 14 | 09:11:21 | 9:11.28 | 23.1 s | 12 |
| 15 | 09:36:26 | 9:36.35 | 23.3 s | 13 |
| 16 | 10:01:47 | 10:01.63 | 23.2 s | 14 |
| 17 | 10:26:63 | 10:26.84 | 23.3 s | 15 |
| 18 | 10:52:09 | 10:52.12 | 23.0 s | 16 |
| 19 | 11:17:12 | 11:17.16 | 23.3 s | 17 |
| 20 | 11:42:36 | 11:42.48 | 23.0 s | 18 |
| 21 | 12:07:39 | 12:07.52 | 23.2 s | 19 |
| 22 | 12:32:52 | 12:32.69 | 23.3 s | 20 |
| 23 | 12:57:73 | 12:57.97 | 23.2 s | 21 |
| 24 | 13:23:10 | 13:23.13 | 23.0 s | 22 |
| 25 | 13:48:13 | 13:48.17 | 23.2 s | 23 |
| 26 | 14:13:28 | 14:13.37 | 23.3 s | 24 |
| 27 | 14:38:50 | 14:38.67 | 23.2 s | 25 |
| 28 | 15:03:68 | 15:03.91 | 23.1 s | 26 |
| 29 | 15:28:74 | 15:28.99 | 23.1 s | 27 |
| 30 | 15:54:05 | 15:54.07 | 23.3 s | 28 |
| 31 | 16:19:25 | 16:19.33 | 23.1 s | 29 |
| 32 | 16:44:29 | 16:44.39 | 46.2 s | 30 |
| 33 | 17:32:42 | 17:32.56 | 31.6 s | 31 |
| 34 | 18:06:15 | 18:06.20 | 31.7 s | 32 |
| 35 | 18:39:70 | 18:39.93 | 23.1 s | 33 |
| 36 | 19:05:03 | 19:05.04 | 42.3 s | 34 |
| 37 | 19:49:25 | 19:49.33 | 44.3 s | 35 |
| 38 | 20:35:45 | 20:35.60 | 30.1 s | 36 |
| 39 | 21:07:54 | 21:07.72 | 34.2 s | 37 |
| 40 | 21:43:72 | 21:43.96 | 48.5 s | 38 |
| 41 | 22:34:36 | 22:34.48 | 38.2 s | 39 |
| 42 | 23:14:48 | 23:14.64 | 30.7 s | 40 |
| 43 | 23:47:27 | 23:47.36 | 32.2 s | 41 |
| 44 | 24:21:45 | 24:21.60 | 23.5 s | 42 |
| 45 | 24:47:09 | 24:47.12 | 23.1 s | 43 |
| 46 | 25:12:15 | 25:12.20 | 23.2 s | 44 |
| 47 | 25:37:33 | 25:37.44 | 86.2 s | 45 |
| 48 | 27:05:50 | 27:05.67 | 40.6 s | 46 |
| 49 | 27:48:22 | 27:48.29 | 27.3 s | 47 |
| 50 | 28:17:47 | 28:17.63 | 32.2 s | 48 |
| 51 | 28:51:62 | 28:51.83 | 55.2 s | 49 |
| 52 | 29:49:01 | 29:49.01 | 36.4 s | 50 |
| 53 | 30:27:28 | 30:27.37 | 35.1 s | 51 |
| 54 | 31:04:37 | 31:04.49 | 64.7 s | 52 |
| 55 | 32:11:16 | 32:11.21 | 23.1 s | 53 |
| 56 | 32:36:22 | 32:36.29 | 26.2 s | 54 |
| 57 | 33:04:35 | 33:04.47 | 25.6 s | 55 |
| 58 | 33:32:04 | 33:32.05 | 23.2 s | 56 |
| 59 | 33:57:17 | 33:57.23 | 23.2 s | 57 |
| 60 | 34:22:29 | 34:22.39 | 40.6 s | 58 |
| 61 | 35:04:71 | 35:04.95 | 38.1 s | 59 |
| 62 | 35:45:00 | 35:45.00 | 28.1 s | 60 |
| 63 | 36:15:06 | 36:15.08 | 23.2 s | 61 |
| 64 | 36:40:21 | 36:40.28 | 23.1 s | 62 |
| 65 | 37:05:26 | 37:05.35 | 23.2 s | 63 |
| 66 | 37:30:44 | 37:30.59 | 26.2 s | 64 |
| 67 | 37:58:60 | 37:58.80 | 23.3 s | 65 |
| 68 | 38:24:06 | 38:24.08 | 23.1 s | 66 |
| 69 | 38:49:16 | 38:49.21 | 23.4 s | 67 |
| 70 | 39:14:43 | 39:14.57 | 31.2 s | 68 |
| 71 | 39:47:61 | 39:47.81 | 25.7 s | 69 |
| 72 | 40:15:39 | 40:15.52 | 24.2 s | 70 |
| 73 | 40:41:57 | 40:41.76 | 48.3 s | 71 |
| 74 | 41:32:05 | 41:32.07 | 47.5 s | 72 |
| 75 | 42:21:42 | 42:21.56 | 23.0 s | 73 |
| 76 | 42:46:45 | 42:46.60 | 24.2 s | 74 |
| 77 | 43:12:63 | 43:12.84 | 70.0 s | 75 |
| 78 | 44:24:65 | 44:24.87 | 32.3 s | 76 |
| 79 | 44:59:14 | 44:59.19 | 37.6 s | 77 |
| 80 | 45:38:57 | 45:38.76 | 37.1 s | 78 |
| 81 | 46:17:62 | 46:17.83 | 51.5 s | 79 |
| 82 | 47:11:23 | 47:11.31 | 49.6 s | 80 |
| 83 | 48:02:65 | 48:02.87 | 42.5 s | 81 |
| 84 | 48:47:30 | 48:47.40 | 30.3 s | 82 |
| 85 | 49:19:56 | 49:19.75 | 27.6 s | 83 |
| 86 | 49:49:29 | 49:49.39 | 32.1 s | 84 |
| 87 | 50:23:35 | 50:23.47 | 35.4 s | 85 |
| 88 | 51:00:62 | 51:00.83 | 38.1 s | 86 |
| 89 | 51:40:70 | 51:40.93 | 40.7 s | 87 |
| 90 | 52:23:49 | 52:23.65 | 23.3 s | 88 |

Byte offset of a track in `ANGELVOICE.BIN`:
`offset = MSF_frames(INDEX 01) * 2352` (75 frames/s, 2352 bytes/frame,
16-bit signed stereo LE @ 44.1 kHz — playable raw).

Where each track appears *in the story*: the ids are assigned in
dialogue-script order, so tracks 2–31 cover the prologue / early common
route (the wizard intro with the queen is the first voiced scene) and the
later, longer tracks (32+) are the character/ending lines. The per-scene
attribution is carried in the compiled dialogue scripts (the id is fetched
by the engine right before the `$C27AEA` call site); decoding the script
format to enumerate scene ↔ id statically is future work — at runtime the
Voice Kun hook logs `Voice Kun: play track N` for every line as the story
plays.

## How the game drives the Voicer-kun

### Hardware interface (all games with this device)

- **IR transmit**: bit-banged on **WRIO `$4201` bit 7** (port 2 IOBit).
  The transmit routine at **`$C15788`** takes a 24-bit pointer to an
  RLE-coded envelope: a list of 16-bit durations, level toggling after
  each entry (first entry = high), `$0000` terminator. Each duration unit
  is one iteration of a NOP-padded loop ≈ **428 master cycles ≈ 20 µs**.
  Line idles low; a code is at most 160 entries (322-byte slot).
- **IR receive (learn)**: polled on **JOYSER1 `$4017` bit 1** (port 2
  D1 line) by the recorder at **`$C156D8`** — it waits for the line to go
  high, then samples run-lengths in the same fixed-period loop into a raw
  buffer (the wizard passes `$7F:8000`, max `$2000` samples), with
  timeout/overflow exits. `$CA4656` then compresses the raw capture into
  a 322-byte RLE slot. This is how it "learns" the player's CD remote.
- A manual strobe/serial-read helper exists at `$C15805`
  (`STA $4016` / `LDA $4016,x`) but no caller was found in this ROM.

### Learned-code slots (WRAM, backed by SRAM)

16 slots × 322 bytes at **`$7E6845`** (`slot[n] = $7E6845 + n*322`,
ending at `$7C64`), indexed by remote-key id:

| Slot | Key (per dispatcher usage) |
|---|---|
| 0, 1 | power/stop-class keys (gated by caps bits 0/1) |
| 2 | PLAY |
| 3 | PREV track |
| 4 | NEXT track |
| 5–14 | digits (sent as `$CA4619(n)` = slot n+5) |
| 15 | "+10" key |

Config block: `$7E541C` (init `$FF`), **`$7E541D` capability mask**
(init `$80`; bits gate which keys/strategies exist), **`$7E541E`
inter-key delay** (default `$28` = 40 frames), `$7E541F`–`$7E5422`
further delays/flags. Master enable **`$7E6844`** (1 = Voicer active).
CD state: **`$7E7C66` current track**, `$7E7C67`/`$7E7C68` play state.

### Track selection strategies (bank C9 dispatcher)

`$CA45E0(key)` = "transmit learned code for key" (multiplies key × 322,
sends slot); `$CA4619(n)` = same for digit slots (n+5). On top of these
the dispatcher implements three remote styles, chosen by capability bits:

1. **Relative seek** — `$C98C39`: compares target track to current
   (`$7E7C66`), then taps NEXT (`$C98AB2` loop) or PREV (`$C98B4C`)
   `|target−current|` times, then PLAY.
2. **"+10"-style direct entry** — `$C98CC3`: sends "+10" ⌊track/10⌋
   times, then the remainder digit, then PLAY.
3. **Two-digit direct entry** — `$C98D62`: sends a prefix key, then
   ⌊track/10⌋ and track mod 10 (`$C1523B` udiv / `$C15395` umod), then
   PLAY.

Every key is followed by a `$7E541E`-frame wait (`JSL $C08000` service
`$10`).

## Setup wizard

Fresh boot goes straight into the wizard: KOEI logo (~f300) →
"ネオロマンスゲーム" splash → queen dialogue "…女王候補が、目覚めるようですね…"
→ **"では、ボイサーくんをコントローラコネクタ2に差し込んで下さい"** →
menu **「準備した」/「手動にする」** (ready / manual). Choosing 準備した
with no device loops back to the insert prompt; 手動にする is the
no-hardware path (game displays track numbers for the player to punch in
manually — mode `$7E241B` = manual).

Once per boot, when the insert prompt first types out (~frame 1330), the
game transmits a short synthetic waveform from a **stack-built** buffer
(pointer `$00:08D3`): durations `[10, 5,5,5,5,5,5,5,5]` units ≈ 480 µs
header + 8 × 240 µs alternations — a detect/self-test ping, not a
plausible remote code. How the game *senses* the device is still open: in
all captures it never read `$4016`/`$4017`/`$4213`/`$421E` around the
selection, and mirroring the IR output back onto `$4017.1` did not satisfy
it either. Next candidates if anyone wants the hardware path: trace the
準備した handler (`$C156D8` / `$CA55DE` / `$CA5114`) to find what state it
tests. None of this is needed for CD playback — the manual path below
reaches the same voice requests.

## The device detect (solved)

Both games decide the Voicer-kun is plugged in by reading the **port-2
auto-joypad register `$421A` (JOY2L)** and testing its **low nibble for
`$D`**. On the SNES the auto-read shifts 12 button bits out of a port and
then four device-id bits; a standard pad reports `0` there, so the check
fails and the setup prompt loops forever — which is why both games got
stuck at "connect the Voicer-kun and press A".

EMIT's check, at `$C91341`:

```
PEA $421A            ; JOY2L
PEA $0000
PEA $0001
JSL $C08000          ; kernel service $01 - read that register
LDA $00
AND #$000F           ; device id nibble
CMP #$000D           ; Voicer-kun signature
BNE  -> re-prompt
LDA #$0001           ; accepted
```

`voicekun.cpp` reports that id (`port2_id`) on port 2 whenever a verified
disc is attached, ORed into the word `S9xDoAutoJoypad` writes to `$421A`
(the serial-read path in `S9xReadJOYSERn` is *not* enough on its own — the
auto-joypad path writes `$421A` directly and bypasses it). With that in
place both games accept the device: EMIT sets `$7E22B2 = 1` and moves on,
and Angelique leaves its wizard the same way.

Notes for anyone extending this:

- Nothing about the detect touches the IR lines. `$4017` is read only by
  the learn routine and `$4201` written only by the transmitter, and
  neither runs during the check — earlier attempts to satisfy it by
  echoing IR were looking in the wrong place.
- **The next gate is the IR learn wizard.** Having accepted the device,
  both games ask the player to point a real CD remote at it and press each
  key, recording the waveforms into the 322-byte slots. Getting through
  that needs synthetic IR on `$4017` bit 1. The codes need not be real
  remote codes: nothing downstream transmits to actual hardware, so any
  self-consistent waveform the learner accepts will do.

## Voicer mode

With the device reported and the IR line emulated, the Voicer path
completes and the game drives the CD player itself — the deck UI that
manual mode shows never appears. The mechanism was traced on EMIT and is
written up in full in [emit_1.md](emit_1.md): the mode byte, the five
322-byte transport slots, the two transmit dispatchers, and the fact that
the whole configuration persists to SRAM so the learn wizard is one-time.

Angelique runs the same bank-`$C9` driver behind a friendlier UI. Its
selection screen asks ボイサーくん / 手動 (「彼女のために、声の制御の方法を
決めてくださいね。」), then quizzes the player's remote layout —
「CDのリモコンに ▶ ボタンがありますか?」 with あります / ない — rather than
showing EMIT's deck graphic. Its mode byte is **`$7E6844`** (`1` = Voicer),
the counterpart of EMIT's `$7E22B2`.

Verified in the harness: attaching the disc makes the ボイサーくん option
selectable and `$7E6844` reaches `1`. Angelique's wizard was **not** driven
to completion, so its per-scene behaviour in Voicer mode is untested; EMIT's
was, across 14 tracks.

## The snes9x implementation: Voicer-kun audio tracks (no IR)

We do not emulate the IR hardware. The shipped feature plays the audio
straight from the CD image:

- **Sound → Voice Kun** (win32) appears **only while a supported
  game is loaded** (dynamic show/hide in `CheckMenuStates`, keyed on the
  game list). **Attach Audio CD...** accepts the game's `.cue` (BIN
  streamed from disk) or a `.zip` holding cue+bin (BIN extracted into
  memory). **Eject Audio CD** is grayed until a disc is attached.
- The attach is **verified against the game's disc profile** before it
  is used — track count, BIN size, and a CRC32 fingerprint of the BIN's
  first 64 KB must match (plus structural cue checks: audio tracks only,
  single FILE, consistent offsets). A music CD or any unrelated image is
  rejected with the reason shown to the user.
- Core module `voicekun.cpp/h`: cue/zip parsing, disc validation, track
  table, and a 44.1 kHz PCM streamer pumped from the SPC DSP output hook
  and mixed into the output exactly like MSU-1 audio
  (`namespace voicekun` in `apu/apu.cpp`, clamp-mixed after the MSU-1
  block, silence when idle).
- Game hook: `voicekun_games[]` in `voicekun.cpp` keys on `ROMCRC32` and
  carries the disc profile. For Angelique (`ROM CRC32 EA6AE8A9`, disc =
  90 tracks / 558,644,688 bytes / head CRC `D7978EEB`) it watches two PCs
  in `S9xMainLoop`: **`$C98EAC` = request voice**, whose 16-bit stack
  argument at `S+4` is the **absolute CD track number** (no bias), and
  **`$C98EED` = voice ended**, which stops playback. A new request
  replaces the current track; hard/soft reset stops it; loading a
  different ROM auto-ejects. Adding another Voicer-kun game = one table
  row (crc32, play PC, stop PC, arg offset, bias, disc profile).
- OSD: each request shows `Voice Kun: playing track N/90 (43s)`, so you
  can see playback start even where the voice is quiet.

### Why `$C98EAC` and not `PlayVoice` at `$CA4114`

`$CA4114` is the *Voicer-driver* entry (it derives `track = id + 2` and
feeds the IR sender). It is **never reached in manual mode**, and the
prologue's voiced scenes do not go through it at all — hooking it
produced no playback. The real per-voice entry is `$C98EAC`: it raises
the voice-active flag `$7E38AC`, then branches on `$7E6844` (Voicer
present) between the IR path and the manual on-screen prompt, calling the
HUD renderer `$C9904D` either way. Found by differential JSL tracing in
the headless harness across the frame window where the manual-mode HUD
appears (`$C89987 → $C98EAC → $C9904D`, argument `PEA $0002` matching the
HUD's 「2番目」).

### Auto-confirm (why we synthesize an A press)

In manual mode the game stops at 「N番目 / ポーズ」 and waits for the player
to start their CD deck and press A; only then does it flip to 「プレイ中」
and begin advancing the dialogue on its own timer (verified: one press,
then the text runs through several lines with no further input until
「音声終了」). Since we start the disc ourselves, that press is pure
busywork and would also leave the audio running ahead of the text by
however long the player takes to hit it. So the input layer holds A for
frames 18–48 after a track starts (`S9xVoiceKunAutoConfirm`, applied in
`S9xSetJoypadLatch`) — a ~0.3 s delay so the prompt is up, then a ~0.5 s
hold that reads as one clean press. Gated on the game's manual-mode flag
(`$7E6844 == 0`) so it can never fire while the Voicer driver is active,
and cleared when the voice ends. Result: attach the CD, and voiced scenes
play with the text in sync and no button pressing at all.

### Manual-mode HUD (what the player sees)

In manual mode the game drives a small top-right panel that is the
ground truth for when a voice should sound:

| HUD | Meaning |
|---|---|
| 「N番目」+「ポーズ」 | play CD track N — currently paused |
| 「N番目」+「プレイ中」 | track N now playing |
| 「音声終了」+「ポーズ」 | voice finished |

With a disc attached these transitions are automatic; the panel is still
drawn (it is the game's own CD-operation guidance).
- In-game, answer the setup wizard with **手動にする** (manual mode) —
  the no-hardware path. The game then computes and displays track
  numbers as usual, and the hook plays them automatically at the same
  moment. (The wizard's 準備した path requires the still-unsolved device
  detect and is unnecessary for this feature.)

Not covered (acceptable for voice lines): playback position is not saved
in savestates, and the per-track user volume is fixed at 100%.

Verified headless: `.cue` and `.zip` attach both stream the correct PCM
(RMS and pitch match the BIN source), a tampered cue / wrong zip / wrong
ROM are all rejected, and playing the prologue in manual mode fires
`playing track 2/90` at the game's own request frame followed by
`voice ended` — with the voice audible over the game music in the
captured output.

Pitfall fixed after first bring-up: the CD resampler's rate must be
primed in `UpdatePlaybackRate` **unconditionally**, not only when
`Settings.VoiceKun` is already set. Sound init runs before any attach, so
a mid-session attach otherwise left the stream at the default 1.0 ratio,
which starved the SPC buffer — the symptom was crackling/slow game audio
as soon as a disc was attached, cured by ejecting.

Key ROM routines (HiROM, file offset = addr − `$C00000`):

| Address | Role |
|---|---|
| `$C15788` | IR transmit (RLE envelope → `$4201.7`) |
| `$C156D8` | IR learn/record (← `$4017.1`) |
| `$C15805` | strobe/serial helper (unused?) |
| `$CA45E0` / `$CA4619` | send learned key n / digit n |
| `$CA4656` | raw capture → 322-byte RLE compressor |
| `$C98C39` / `$C98CC3` / `$C98D62` | seek strategies |
| `$CA4114` | PlayVoice(id) → track = id+2 |
| `$C60CEE` | playback layer, receives track number |
| `$C27AEA` | dialogue-engine call site |
