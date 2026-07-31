# EMIT Vol. 1: Toki no Maigo — Voicer-kun audio CD

**Status:** implemented and working in snes9x. The two audio CDs bundled
with the SFC release have been dumped (98 tracks each, one BIN per track),
the play/stop hooks were confirmed in the headless harness, and voiced
scenes play from the disc in manual mode. See `docs/Angelique.md` for the
Voicer-kun background and the shared implementation.

Koei's *EMIT Vol. 1: Toki no Maigo* (SFC, 1995, HiROM FastROM 3MB, 8KB
battery SRAM, internal title `EMIT Vol.1`, ROM CRC32 **`D1AACC2D`**) is
the game the **Voicer-kun** (ボイサーくん) was originally sold with — one
year before *Angelique Voice Fantasy* reused the device. The story
narration ships on two audio CDs; the ROM drives the player's CD deck
through learned IR remote codes, or in manual mode tells the player which
track to cue.

Assets:

- ROM: `win32/Roms/Emit Vol. 1 - Toki no Maigo (Japan).zip`
- CDs: `win32/Roms/Emit Vol. 1 - Toki no Maigo - CD1.zip` and `- CD2.zip`
  ("SFC Game Bundle" dumps: a cue sheet plus 98 per-track BIN files).

## The discs

Each disc holds **98 audio tracks**; the ROM's chapter table (below) uses
36 of CD 1 and 38 of CD 2 for narration, the rest being unused or BGM.
The chapter table's `pos` byte turned out to be the chapter's **first CD
track number**, not a time — chapter 4 runs to track 92 and chapter 10 to
track 88, both inside a 98-track disc, which is what confirmed these dumps
are the right ones.

Disc profiles used for attach verification (`voicekun.cpp`):

| Disc | tracks | total bytes | fingerprint |
|---|---|---|---|
| CD 1 | 98 | 394,863,168 | `57FD0BE3` |
| CD 2 | 98 | 508,817,568 | `4DAC56EF` |

The fingerprint is a CRC32 of 64 KB taken from the **middle** of the disc.
The first sectors of track 1 are digital silence on every Voicer-kun disc
seen so far — Angelique's and both EMIT discs share the same leading-64 KB
CRC — so hashing the start of the disc matches everything and is useless.

An earlier candidate dump (`EMIT1_CD.zip`, the PC version's game disc:
one MODE1 data track plus 26 audio tracks) is **not** either SFC disc and
is rejected by the profile check.

## Shared IR driver library (byte-identical with Angelique)

The low-level driver in EMIT is the exact code Koei later shipped in
Angelique, relocated. 48-byte pattern matches, unique in both ROMs
(HiROM: file offset = address − `$C00000`):

| Routine | Angelique | EMIT Vol. 1 |
|---|---|---|
| IR transmit (RLE envelope → `$4201.7`, port 2 IOBit) | `$C15788` | `$C1F484` |
| IR learn/record (poll `$4017.1`, port 2 D1) | `$C156D8` | `$C1F3D4` |
| strobe/serial helper | `$C15805` | `$C1F501` |
| raw capture → 322-byte RLE slot compressor | `$CA4656` | `$C9121B` |

Same IR protocol, same 322-byte learned-code slot format, same timing
(≈20 µs units). Everything **above** this layer was rewritten between
the games — no byte-level matches — so EMIT's game code was located
independently (below).

## Game-level driver (EMIT-specific)

### Learned-code slots, flags, config

- Slot base **`$7E22B3`**, 322 (`$0142`) bytes per slot; slot address
  computed by helper `$C1ED6A` as `$7E22B3 + n*322`. Slot map matches
  Angelique: digits occupy slots 5–14 (the digit sender adds 5).
  16 slots × 322 = 5,152 bytes — fits the cart's 8KB SRAM, consistent
  with learned codes being battery-backed.
- **`$7E22B2`** — Voicer/CD master flag; all senders and prompt
  routines gate on `== 1` (Angelique's equivalent: `$7E6844`).
- `$7E2158` — secondary gate read by the prompt trio (manual-mode
  sub-flag; exact semantics unconfirmed). Cleared by the dispatcher.
- `$7E2159` — dispatcher mode selector (1 = bypass; 2 = consult SRAM
  config bit).
- `$7E2152`, `$7E214E` (low 12 bits hold a current absolute track/state;
  high nibble is a UI state, `$5000` = prompt phase), `$7E22AA` (track
  base added to the computed in-chapter index), `$7E223B` — CD/UI state.
- **`$30:6002` / `$30:6005`** — battery-SRAM config/capability bytes
  consulted by the dispatcher (≙ Angelique's capability mask
  `$7E541D`); bit 0 of `$30:6005` selects a second strategy.

### Key send layer (bank C9)

- **`$C90D2B` — send learned key n** (≙ Angelique `$CA45E0`): arg at
  `S+4`; gated on `$7E22B2 == 1`; slot pointer via `$C1ED6A`, then
  `JSL $C1F484` to transmit.
- **`$C90D64` — send digit n** (≙ `$CA4619`): identical but sends slot
  **n+5**.
- Third transmitter call site `$C9147D`: candidate for the boot-time
  device self-test ping (Angelique has the same pattern); unconfirmed.
- IR learner called from `$C91DD7` and `$C92715` (learn wizard).
- Flag writes (`STA $7E22B2`) at `$C90EBE`, `$C915EF`, `$C91670` — the
  setup/options code that arms the Voicer.

### Strategy / dispatcher layer (bank C1)

- **`$C15D02`** — CD-operation dispatcher entry: clears `$7E2158`,
  early-outs unless `$7E22B2 == 1`, picks strategy 1 or 2 from
  `$7E2159` and `$30:6005` bit 0, masks with `$30:6002`.
- Send-key call sites cluster in `$C15D8E`–`$C16F60` and digit sites in
  `$C16652`–`$C1672C` (plus `$C92C80`, `$C93AED`) — the EMIT
  counterparts of Angelique's remote-style strategies (NEXT/PREV
  relative seek, digit direct entry). Individual strategy entry points
  not labeled.
- `$C16310` — computes the in-chapter track index (returns in `$00`):
  reads the chapter table (below) and subtracts the chapter's base from
  the absolute id in `$7E214E & $0FFF`.

### Chapter → CD track table at `$C4:03D7`

Ten 3-byte records `(disc, pos, count)` followed by a cumulative-base
byte array at **`$C4:03FB`**:

| Chapter | disc | pos (raw) | tracks | base (`$C403FB[]`) |
|---|---|---|---|---|
| 1 | 0 (CD 1) | `$03` | 11 | 0 |
| 2 | 0 | `$21` | 7 | 11 |
| 3 | 0 | `$34` | 14 | 18 |
| 4 | 0 | `$59` | 4 | 32 |
| 5 | 1 (CD 2) | `$03` | 11 | 36 |
| 6 | 1 | `$1E` | 8 | 47 |
| 7 | 1 | `$31` | 3 | 55 |
| 8 | 1 | `$39` | 4 | 58 |
| 9 | 1 | `$44` | 6 | 62 |
| 10 | 1 | `$53` | 6 | 68 |

Totals: CD 1 = 36 narration tracks, CD 2 = 38, 74 overall (the bases end
at 74). `pos` is the chapter's **first CD track number** — chapter 4 ends
at track 92 and chapter 10 at 88, both inside the 98-track discs. The
per-request argument chain is `track = $C16310() + $7E22AA`, displayed
as `track + 1` — i.e. internal ids are 0-based, CD tracks 1-based.

### Manual-mode prompt trio (bank C1)

All three share the gate "`$7E22B2 == 1` and `$7E2158`-condition", and
all format Shift-JIS templates via `$C1C6ED`:

- **`$C1388E`** — "seek" prompt: takes a 16-bit track argument at `S+4`
  (`S+$36` after its `$32`-byte frame move), increments it, and prints
  **「%02d曲目の先頭位置でポーズ状態にしてください。」** ("pause at the
  start of track %02d", template `$C1B9DD`). Called with the final
  track id from all request paths — **best play-hook candidate**, arg
  at `S+4`, bias +1.
- **`$C139F7`** — countdown prompt: 「%d秒後にＣＤのＰＬＡＹボタンを
  押してください。」 (template `$C1BA0C`), counts 3 → 0; the player
  starts the CD on cue (EMIT's sync mechanism — unlike Angelique's
  single A-press confirm).
- **`$C13B08`** — end-of-voice/stop display (template just after the
  others; body parallels `$C1388E`) — **best stop-hook candidate**.
  A fourth sibling reads the flag at `$C13B9C` (likely the
  「ＣＤをポーズ状態にし、１曲分戻してください。」 "pause and go back one
  track" prompt at `$C1BA3C`).

Request-path call sites observed statically: `$C17BE1`, `$C17C96`,
`$C8010C` (scene engine; computes `$00 + $7E22AA` → arg), and wrapper
**`$C101B2`** (forwards its `S+4` arg; called from `$C71820`,
`$C80C1B`). Typical sequence: compute id (`$C16310`) → seek prompt
(`$C1388E`) → countdown (`$C139F7`) → IR dispatcher (`$C15D02`).

### Confirmed in the harness (what the shipped hook uses)

Booting the ROM headless, choosing 手動で操作する and playing into
chapter 1 shows one request per voice, all issued from a single routine
around `$C17C9x`:

```
$C16310   compute the in-chapter index
$C1388E   "pause at the start of track NN"   arg at S+4, displayed = arg+1
$C139F7   countdown, ~3 s later
$C15D02   dispatcher — the game acting on PLAY, ~3 s after that
```

The `S+4` argument was `0x0003` then `0x0004` while the on-screen HUD read
**♪04** then **♪05**, so **CD track = arg + 1** as predicted.

**The hook is split across the sequence.** `$C1388E` says 「…の先頭位置で
ポーズ状態にしてください」 — *cue the disc and pause there* — so it names the
track without playing it, and it also **stops** whatever was playing, since
the deck is being re-cued. `$C15D02`, after the 3..1 countdown, is the game
acting on PLAY, and that is where the track starts. Playing at the cue
prompt instead starts the audio while the game is still saying "pause
here", which is wrong by about the length of the countdown.

The sequence runs on its own once a scene changes — the countdown is not
gated on input — so a scene change plays its track automatically.

`$C13B08` is wired as the stop hook but has never been observed firing,
and neither has its sibling `$C13B9C`, so there is no explicit end-of-voice
signal to hook. Playback therefore ends when the track runs out, when the
next scene re-cues the deck, or on reset.

**The prompt repeats about every 10 s while a voice is current** (the game
is nagging a player who hasn't worked the deck). Acting on every repeat
restarts the audio, so a request for the track already selected is ignored
— while it plays, and for `VOICEKUN_REPLAY_GUARD` frames (25 s) after it
ends, which covers the repeats that follow a finished track.

Past that window the same track **does** play again, which matters: the
game's index (目次) lets the player re-enter a scene, and the re-entered
scene asks for the track it used the first time. Holding the last track
number indefinitely made those revisits silent.

## Other setup-wizard strings (context)

Around `$C1BA3C`–`$C1BBCC`: 「ＣＤをポーズ状態にし、１曲分戻して
ください。」, 「ＣＤをポーズ状態にしてください。」, 「ＣＤ１をセットして
ください。」, 「ＣＤ１がセットしてあることを確認してください。」,
「ＣＤ２をセットしてください。」, 「ＣＤプレイヤーに総曲数と総再生時間が
表示されていることを確認してください。」 — the game manages disc swaps
and verifies the correct disc via the player's own deck display.

## The examined PC-version disc (for reference)

`emit1.cue`/`emit1.bin`: 276,487 sectors, BIN head CRC32 (first 64 KB,
MODE1 data) **`3DC2F9CE`**. Track 1 MODE1/2352 (94.7 s of data), then
26 audio tracks from 01:34:55 with lengths 7.8 s – 664.5 s. Not the
SFC discs; kept only as the PC port's disc image.

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

## Voicer mode (solved, verified)

The setup screen states the point outright:
「ボイサーくん」があれば、CDプレイヤーの操作は自動的に行えます — *with the
Voicer-kun, CD player operation is performed automatically*. Manual mode
is the fallback for players without the peripheral, not the normal path.

### Setup flow

`$C90DD2`–`$C90E04` is a state machine driven by `$7E214E & $0FFF`:

```
JSR $148D            ; the "use Voicer-kun / operate manually" menu,
LDA $00              ;   returns the chosen mode
CMP #$0001
BEQ  -> state loop   ; Voicer: run the learn wizard (state 1 = JSR $1B52)
BRL $C90EAD          ; manual: exit setup immediately
```

The exit at `$C90EAD` reads **SRAM `$306002`**: non-zero means a saved
configuration exists and `$7E22B2` is left alone; zero forces manual.

### The learned-code table

`$7E22B2` is the mode byte (`1` = Voicer). Immediately after it sit five
322-byte RLE slots, `$22B3`–`$28FC`, in transport order:

| slot | key | | slot | key |
|---|---|---|---|---|
| 0 | ■ stop | | 3 | ⏮ prev |
| 1 | ▶ play | | 4 | ⏭ next |
| 2 | ⏸ pause | | 5+ | 0–9 keypad (optional page) |

Two dispatchers transmit from it, both gated on `$22B2 == 1`:

- `$C90D2B(n)` — slot `n`, i.e. the five transport keys.
- `$C90D64(n)` — slot `n + 5`, the numeric keypad registered by the
  optional 「曲番号ボタンの登録」 page for decks that take absolute track
  numbers. Declining that page makes the game seek with ⏭ instead.

Both compute the slot address as `$7E22B3 + n * $0142` (322) and hand it
to the transmitter at `$C1F484`. **We never read the slot contents** —
hooking these two PCs yields the deck command directly.

### The config persists in SRAM

The whole configuration is mirrored to SRAM from offset `$0A` (byte-for-byte
identical to the WRAM slots), with the "configured" marker at `$02`. So the
wizard is genuinely one-time: on a later boot the game offers
「−リモコンの再登録−」 (re-register buttons / re-register functions / clear
data / **終了**) instead of the wizard, and 終了 goes straight to the story.
Nothing needs to be injected — snes9x's ordinary `.srm` handling carries it.

### Observed command stream

Traced from a boot with a saved config, playing hands-free (only story-text
advances pressed):

```
STOP                      ; deck reset
PLAY / PAUSE / STOP       ; disc verification track
NEXT x4                   ; seek to the scene's track
PLAY                      ; voice starts   -> track 4
PAUSE
PLAY / PAUSE ...          ; paced against the on-screen text
```

A 40,000-frame run played 14 distinct tracks (1, 4–14, 34, 35) with no
deck UI on screen, the `♪NN` HUD counter advancing on its own. Track 5
played **twice** in that run, so a revisited scene re-requests correctly.

The existing `$C1388E` arm / `$C15D02` play hooks fire in Voicer mode too
and reported the correct track throughout, so no IR decoding is needed for
playback; the dispatcher PCs above are documented as the more faithful
alternative should the prompt hooks ever prove unreliable.

### Player-facing procedure

1. Sound > Voicer-kun > Attach Audio CD… (CD 1, and CD 2 if wanted).
2. At 「このソフトは「ボイサーくん」に対応しています」 choose
   「ボイサーくん」を使う, then A at the connect prompt.
3. Teach the five transport keys: **A** registers the highlighted key
   (the emulated IR answers instantly), **A** again dismisses the message,
   **←/→/↓** move. Then **B** ends registration.
4. 「曲番号ボタンの登録」 — 行わない is fine; the game then seeks with ⏭.
5. On the 「−リモコンの機能を登録−」 list pick 終了 (the deck-quirks
   defaults are unused by us).
6. Save; from then on the game offers 終了 at the re-registration menu.

Angelique's flow is the same driver with a friendlier UI: it asks
ボイサーくん / 手動, then quizzes the remote's layout (「CDのリモコンに ▶
ボタンがありますか?」) instead of showing a deck graphic. Its mode byte is
`$7E6844`. Only the mode selection was verified in the harness; its wizard
was not driven to completion.

## How it works in snes9x

Sound > Voicer-kun > Attach Audio CD... takes either CD's `.zip` (or its
`.cue` unpacked on disk). EMIT's dumps are **one BIN per track**, so the
cue parser handles multi-FILE sheets: each track records which file it
lives in plus its offsets, and zip members are inflated one file at a time
when a track starts rather than holding the whole disc in memory.

Both discs can be attached at once — Attach stays enabled until every disc
in the profile is in. The disc a track is taken from is the one attached
last, matching the physical act of swapping discs when the game prompts
「ＣＤ２をセットしてください。」 at chapter 5.

### Why there is no auto-press for EMIT

Angelique waits on a single A press once its voice starts, so the input
layer supplies it (`docs/Angelique.md`). EMIT is different: a scene change
walks a multi-step CD-deck UI — show the deck, act on PLAY, then start the
animation — and each step consumes a keypress. Injecting those presses was
tried and backed out, because the game shares the same buttons with story
advancement:

| attempt | result |
|---|---|
| no injection (shipped) | tracks 1, 4, 5, 6, 7, 8, 9 — every voice plays |
| burst of 4 A presses after the request | track 6 never requested |
| one press per prompt PC | only 2 voices reached; the prompt PCs repeat |
| L instead of A (dialogue ignores L) | track 5 lost — same skipping |

Measured with identical player input each run, so the losses are caused by
the injection, not the pacing. None of it is needed: hooking the scene
change itself (`$C1388E`) starts the voice without any synthetic input,
and the deck UI is then just the game's own interface, harmless to leave
to the player.

The clean fix is the **Voicer path**, where the game drives the deck over
IR and never prompts at all. Selecting 「ボイサーくん」を使う stops at
「ボイサーくんをコントローラコネクタ2に接続し、Aボタンを押してください」
and waits for the device to answer — the same unsolved detect that blocks
Angelique's 準備した path. Solving that would make both games hands-free
and delete the auto-press machinery entirely.

### Known gaps

- **Automatic disc switching.** When both discs are attached the game's
  own idea of which disc is loaded isn't read, so the last-attached disc
  wins. Deriving it would mean locating the current-chapter variable and
  mapping it through the `$C403D7` table (the fields the static analysis
  guessed for this — `$7E214E`, `$7E22AA` — did not hold the expected
  values at request time and need a fresh look).
- **The setup track keeps playing.** The disc-confirmation step cues
  track 1, a ~4-minute announcement / total-time track, and the game
  expects it to actually play — so it does. It stops when the next scene
  re-cues the deck, but if the player opens the index menu first it keeps
  running underneath, because no end-of-voice signal exists to hook.
  (`first_voice` in the profile can suppress low tracks, but suppressing
  track 1 leaves the countdown silent, which is worse.)
- **Sync.** Playback starts at the dispatcher, which is where a player
  would have pressed PLAY. The game's own on-screen timer starts a few
  seconds later; whether that timer is wall-clock or something else was
  not pinned down, so exact frame-level sync is unverified.

Key EMIT ROM addresses (HiROM, file offset = addr − `$C00000`):

| Address | Role |
|---|---|
| `$C1F484` | IR transmit (shared lib; ≙ Angelique `$C15788`) |
| `$C1F3D4` | IR learn/record (≙ `$C156D8`) |
| `$C1F501` | strobe/serial helper (≙ `$C15805`) |
| `$C9121B` | RLE compressor (≙ `$CA4656`) |
| `$C90D2B` / `$C90D64` | send learned key n / digit n (slots at `$7E22B3`) |
| `$C15D02` | CD-operation dispatcher |
| `$C16310` | in-chapter track index compute (table `$C403D7`) |
| `$C1388E` | "pause at track %02d" prompt — play-hook candidate |
| `$C139F7` | "press PLAY in %d s" countdown |
| `$C13B08` | end-of-voice display — stop-hook candidate |
| `$C101B2` | request wrapper (callers `$C71820`, `$C80C1B`) |
| `$C4:03D7` / `$C4:03FB` | chapter records / cumulative track bases |
