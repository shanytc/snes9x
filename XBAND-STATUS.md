# XBAND emulation status

This branch (`xband_support`) adds preliminary XBAND modem peripheral
emulation to snes9x. XBAND was a pass-through cartridge by Catapult
Entertainment (1994-1997) containing its own 1MB firmware ROM, 64KB
SRAM, a Rockwell RC2324DP 2400-baud modem, and a custom "Fred" chip
that applied per-game binary patches at runtime to enable online
multiplayer.

## What works

- **BIOS load and detection.** Loading the released `X-Band Modem
  BIOS (U).smc` (1MB HiROM, internal name `XBAND VIDEOGAME`,
  CRC32 `a8b868a0`) triggers XBAND mode automatically. Detection
  scans the ROM for `CATAPULT` / `X-Band` / `XBAND` signatures.

- **HiROM memory mapping** with the correct SRAM mirror window:
  - `$00-$3F:$8000-$FFFF`, `$80-$BF:$8000-$FFFF`, `$C0-$DF:$0000-$FFFF`
    map to firmware ROM (HiROM-style)
  - `$E0-$FA:$0000-$FFFF`, `$FB:$0000-$BFFF`, `$FC-$FF:$0000-$FFFF`
    are XBAND SRAM mirrors (matches bsnes-plus xband_support)
  - `$FB:$C000-$FFFF` is the Fred + modem MMIO window

- **Fred + Rockwell modem register dispatch** at 2-byte stride.
  Includes the magic constants from bsnes-plus that the BIOS power-on
  expects: Fred reg `$7D` -> `$80`, reg `$B4` -> `$7F` (kLEDData),
  modem reg `$0F` -> RLSD+CTS, reg `$0E` -> 2400 baud, reg `$0D` ->
  U1DET, reg `$1E` -> last_written | TDBE, reg `$19` reset to `$46`.
  Plus smart-card-present (`kSStatus = $01`) hack from older
  bsnes-plus dead code.

- **`kreadmstatus2` consecutive-reads cap.** bsnes-plus's "fixes
  PUVBLCallback overflow" trick is implemented: limits "RX data
  ready" reads to 127 in a row, then forces a 0 to break tight
  polling loops.

- **SRAM dump loader.** On reset, `S9xResetXBand` looks in
  `S9xGetDirectory(BIOS_DIR)` for one of: `XBAND.srm`, `xband.srm`,
  `XBAND.bin`, `xband.bin`, `Benner.1.SRM`, `XBand_luke2.srm`,
  `SF2DXB.S04.srm`. The first 64KB file found is loaded into
  `XBand.sram[]`. We use `BIOS_DIR` rather than `SRAM_DIR` because
  snes9x never writes to `BIOS_DIR`, so the dump survives across
  runs. `Memory::SaveSRAM` is a no-op in XBAND mode for the same
  reason (don't clobber the dump with half-initialized garbage).

  The Cinghialotto/xband repo includes three preserved SNES XBAND
  SRAM dumps -- drop one in `win32/BIOS/` to use it.

- **SRAM dump selection menu.** `Netplay -> XBAND: SRAM ...` lets
  the user pick which preserved dump to load. Each dump contains a
  different historical 1990s user profile. The menu reloads the SRAM
  immediately and triggers a SNES reset so the BIOS re-reads the new
  contents.

- **Save state coverage.** `SnapXBand` in `snapshot.cpp` serializes
  the full XBAND state (regs, modem regs, kill/control, rxbuf/txbuf,
  net step, sram). Snapshot version bumped to 13.

- **TCP socket bridging.** `S9xXBandConnect`, `S9xXBandDisconnect`,
  and `S9xXBandPoll` provide cross-platform (Winsock / POSIX) TCP
  bridging between the modem RX/TX FIFOs and a remote server.
  `S9xXBandPoll` is wired into `cpuexec.cpp` so it runs once per
  frame.

- **Multi-cart loader path.** `File -> Open Multicart...` accepts
  the XBAND BIOS in slot A (with or without a game cart in slot B).
  When `is_XBand_BIOS` matches the slot-A image, `Multi.cartType`
  is set to a new value `6`, `LoadXBandMultiCart` runs, and
  `Map_XBandMultiCartHiROMMap` lays out the same HiROM + SRAM
  mirror + MMIO window as the standalone path. The `stbios.bin
  not found!` warning in the dialog only matters for Sufami Turbo
  loads -- you can ignore it for XBAND.

- **Live network connection to the public retrocomputing.network
  XBAND server.** As of this session, the BIOS reaches the
  "Connected to XBAND!" screen against the live community-revival
  server (`xbserver.retrocomputing.network:56969`) with the
  Benner.1.SRM dump loaded. Bidirectional ADSP frames flow with
  validated CCITT-16 CRCs in both directions. See "How the protocol
  actually works" below.

## How the protocol actually works

This is the result of multiple sessions of trial and error,
disassembly, packet capture, and cross-referencing with the dreampi
project (Kazade/dreampi netlink.py). It's the canonical reference
for what we know about the XBAND wire protocol.

### Server endpoint

`xbserver.retrocomputing.network:56969` is the canonical hostname per
dreampi. As of this session, the following resolve to the same
backend IP (`51.79.10.145`) so any of them work, but `xbserver.*` is
the documented one:

- `xbserver.retrocomputing.network`  ← canonical
- `16bit.retrocomputing.network`
- `xband.retrocomputing.network`
- `retrocomputing.network`

The `Netplay` menu has explicit Connect items for all four; the
default `Connect to Server...` uses `xbserver.*`.

### Identity handshake

On TCP connect, we immediately send the bsnes-plus identity prefix
exactly once:

```
"///////EMU-Waj04qaASNfmaRNw\n"   (28 bytes, last byte is \x0a)
```

That's it for the handshake -- no echo, no banner exchange, no
waiting for a server greeting. dreampi sends `///////PI-` plus the
Pi's CPU serial in the same shape; the server appears to dispatch
per-prefix to different protocol handlers (`PI-` for Dreamcast bridge,
`EMU-` for SNES emulator).

The 16-byte body is supposed to be a unique per-box hardware ID. We
use the bsnes-plus literal value because it apparently still gets
treated as a "valid emulator client" by the server. The catapult
gameplay variant (`0123456789abcde2`) gets explicitly rejected with
`sayonara...` and EOF, so the server clearly has some kind of
allow/deny list.

### TX framing (BIOS -> server)

dreampi-style: BIOS bytes accumulate in `XBand.txbuf` until we see
the ADSP frame end marker `\x10\x03`. Then the entire packet is sent
as one `send()` call. Bytes after the last `\x10\x03` stay buffered
for the next frame.

Safety valve: if the buffer fills past `XBAND_TXBUF_SIZE / 2` without
an end marker, fall back to a raw flush so unframed pre-ADSP `\r`
sequences from modem auto-baud don't accumulate forever.

### RX framing (server -> BIOS)

Pure passthrough. Server bytes go straight into `XBand.rxbuf` without
any inspection or filtering. The BIOS does its own deframing and
validation internally.

(Earlier in development we had a "HELO\n filter" that stripped the
server's HELO probes from the RX stream, but it turned out to be
unnecessary -- dreampi doesn't filter and the BIOS handles them fine.
The filter is still present as a runtime toggle for A/B testing
(`Netplay -> XBAND: Toggle HELO Filter`) but defaults to OFF.)

### ADSP frame format

Per `xband_post.txt` and confirmed against live wire bytes:

```
\x00 + escape(payload + crc16) + \x10\x03
```

- **`\x00`** = ADSP encapsulation byte (frame start)
- **payload** = arbitrary data, byte 0 is the ADSP descriptor
  byte (NOT a ServerTalk opcode despite that being a tempting
  interpretation -- byte 0 commonly seen as `$05` for server-to-box
  and `$04` for box-to-server, which are ADSP control/data
  descriptor values)
- **crc16** = 2-byte big-endian CCITT-16 CRC, see below
- **escape()** = doubles `\x10` to `\x10\x10` in the payload + CRC
  region (NOT including the leading `\x00` or trailing `\x10\x03`)
- **`\x10\x03`** = frame end marker (always unescaped)

### CRC formula (canonical)

**CCITT-16-FALSE** (poly `$1021`, init `$FFFF`, final XOR `$FFFF`)
computed over **the body INCLUDING the leading `\x00` encapsulation
byte**. This contradicts the PHP framer in `xband_post.txt` which
computes CRC *before* encapsulation, but the live server's wire
bytes are authoritative -- the canonical formula was identified by
running 7 CRC variants in parallel against captured frames and
seeing which one matched (`xband.cpp::xband_crc_false` plus a
synthetic leading `\x00`).

Tiny `<4` byte frames (e.g. the 2-byte `$1E0F` ADSP probe) have no
CRC field at all and are classified as "control" not "bad CRC".

### Network test loop summary

End-to-end, a typical "good" connect attempt with Benner.1.SRM looks
like this (per the `Netplay -> XBAND: Show Kill/Ctrl Trace` dump):

```
ADSP handshake state:
  net_step          = 2 (CONNECTED)
  identity sent     = 1
  socket            = open
  socket RX bytes   = ~600 from server
  socket TX bytes   = ~700 to server
  HELO filter       = OFF (passthrough)
  BIOS hangups      = 0

ADSP frame detector:
  RX frames good CRC = 22  (msServerMiscControl heartbeats)
  RX frames control  = 1   (the $1E0F probe)
  TX frames sent     = 32  (BIOS-generated ADSP frames)
  RX bytes consumed  = 624 (BIOS popped from rxbuf via fred $94)
```

The 32 TX frames include a 125-byte profile-transmission packet
containing the user's box configuration straight from SRAM
(username, hometown, phone, address book). For Benner.1.SRM that's
"0mEgA       DeAtH" / "Fort Wayne, IN" / "485-9149".

## What the BIOS UI does in a connect attempt

End-to-end visual sequence with Benner.1.SRM loaded against the live
server:

1. Smart-card warning ("Hey! The card you inserted is not an XBAND
   Card. Would you still like to connect to XBAND?") -- click Yes.
2. Registration confirmation ("You are dialing from 737-7601. Are
   you sure you want to register with XBAND?") -- click Yes.
3. Dialing animation ("Dialing XBAND...")
4. **"Connected to XBAND!"** -- the breakthrough screen.
5. Plain wallpaper for ~10 seconds while the BIOS waits for
   server-side user data that never arrives.
6. **"Sorry, the connection to XBAND has been lost. Please try
   again soon."** -- the BIOS times out waiting for user
   validation messages from the server.

The BIOS reaches step 4 cleanly and the protocol layer is fully
working at every level -- TCP, identity, framing, CRC. The blocker
is purely **server-side user recognition**: the live community
server doesn't have the historical 1990s users (0mEgA DeAtH and
co.) in its database, so it never sends the post-login messages
(`msNewNGPList`, `msSetCurrentUserNumber`, `msReceiveValidationToken`,
etc.) that the BIOS needs to proceed past the connect screen.

## "Translation problem" mystery solved

For most of this session we were trying to fix a screen that said
"Translation problem. Dialing XBAND again..." Initially it looked
like a network protocol failure. Multiple iterations of TCP-layer
work eventually got past it.

But during SRAM-selection testing we discovered that the problem is
actually **NOT a network failure** -- it's the BIOS validating its
own SRAM config and aborting *before* any network I/O.

Concrete evidence: with `XBand_luke2.srm` or `SF2DXB.S04.srm`
loaded, the BIOS shows "Translation problem" and the trace counters
show `socket TX bytes = 0` -- the BIOS literally never wrote a
single byte to fred reg `$90` even though the TCP socket was open
and receiving HELO probes (`socket RX bytes = 351`, `RX bytes
consumed = 351`). The server eventually times out and closes
(`server EOF seen = yes`).

With `Benner.1.SRM` loaded, the BIOS sends 32 TX frames including
the user profile, and reaches "Connected to XBAND!" before its
later user-validation timeout.

The relevant difference between the dumps is in the stored phone /
dial config data. Benner has a plain-digit phone format
(`485-9149`); luke2 has a vanity number (`1 909 PIMP`) with letters
that probably can't be looked up in the BIOS's DTMF translation
table; SF2DXB likely has a similar issue. Whatever the BIOS's
dial-setup validation function checks, only Benner's data passes.

**Lesson learned:** the "Translation problem" screen in the XBAND
BIOS is a *client-side* config-validation failure, not a network
problem. The proper protocol layer work was still useful (we
eventually reached "Connected to XBAND!" with Benner) but the
original failure mode was client-side, not server-side.

## What still doesn't work

- **Server-side user validation.** The live retrocomputing.network
  server doesn't have any of the preserved 1990s users in its
  database. After "Connected to XBAND!", the BIOS waits for
  follow-up messages (`msNewNGPList`, `msSetCurrentUserNumber`,
  `msReceiveValidationToken`, etc.) that never arrive, then times
  out with "Sorry, the connection to XBAND has been lost." This
  is the next milestone blocker.

- **Game cart pass-through / Fred bank-mux.** Confirmed in the
  earlier session that the BIOS does NOT read the inserted game
  cart at boot or during connection. Cart detection happens *after*
  successful user validation, so this work is gated on the same
  blocker above.

- **Per-game patch chip emulation.** Same -- gated on user
  validation.

- **Two of the three preserved SRAM dumps fail with "Translation
  problem"** because the BIOS rejects something in their config
  before networking starts. Workable but limits us to Benner.1.SRM.

## Diagnostic infrastructure

A lot of debug tooling was added to make the protocol work
debuggable. Most of it is in `xband.cpp` and surfaces via the
`Netplay -> XBAND: Show Kill/Ctrl Trace` menu item:

- **Kill/control register access trace** for the four candidate
  Fred kill/control register addresses (`$FB:FE01`, `$FB:FE03`,
  `$61:7000`, `$61:7001`). Used to confirm none of those are
  actually used by the SNES BIOS (they're Genesis-specific). The
  trap is still wired so it would fire if anything ever did use
  them.

- **Silent-write trap** -- catches any write to a `MAP_NONE` HiROM
  address. The BIOS never writes to ROM space, so any hit here is
  an unknown MMIO register. Used (unsuccessfully) to find a Fred
  bank-mux register.

- **Cross-bank read trap** -- logs reads where the program bank
  is in BIOS code (`$D0-$DF` or `$50-$5F` mirror) but the target
  is cart-side HiROM (`$00-$3F:$8000+`, `$40-$7D`, `$80-$BF:$8000+`,
  `$C0-$CF`). Used to confirm the BIOS never reads the cart at
  boot or during connect.

- **Per-Fred-register write counter** -- 256-bucket histogram of
  every Fred general / modem register write. Lets us identify
  which registers the BIOS actually pokes during a connect attempt.

- **Cart-header read counter** ($XX:$FFB0-$FFDF) -- broken down
  by program bank. Used to confirm the BIOS never inspects the
  SNES cart header.

- **ADSP frame detector** -- per-direction state machine that
  scans the raw byte stream for `\x00...\x10\x03` boundaries,
  unescapes `\x10\x10`, and validates CCITT-16 CRCs. Counts
  good/bad/control/aborted frames. Captures the first 4 RX frames
  and first 4 TX frames into the dump for hand inspection.

- **ServerTalk opcode lookup** -- maps the byte-0 of each captured
  frame to a name from the documented enum
  (`msServerMiscControl`, `msExecuteCode`, etc.). NOTE: byte 0 is
  actually the ADSP descriptor byte, not the ServerTalk opcode --
  the labels are sometimes misleading. The hex dump itself is
  authoritative.

- **CRC variant comparator** -- the dump shows 7 CRC computations
  (CCITT-FALSE, FALSE-noxor, XMODEM, AUG, Kermit, FALSE+leading
  `\x00`, FALSE byte-swap) per captured frame. Variant 5
  (`FALSE+leading\x00`) is the canonical formula but the others
  stay in the dump as a sanity check.

- **Hot loop disassembly window** for the most-frequent BIOS TX
  PC, captured at trap time so we can see what code is doing the
  writes.

- **Stack snapshot at first TX** -- top 64 bytes of stack at the
  moment of the first fred reg `$90` write, plus a search for
  plausible JSL return-address triples in those bytes. Lets us
  trace the call chain for unknown BIOS routines.

- **Auto-reconnect** -- when the BIOS asserts RTS (modem reg `$08`)
  after a hangup, snes9x re-opens the socket using the host/port
  the user originally clicked Connect with. The BIOS retry loop
  works without manual menu clicks.

- **Side-effect-free `xband_peek_byte`** -- for trap-time memory
  reads. Calling `S9xGetByte` from inside `S9xSetXBand` bumps
  `CPU.Cycles` and can trigger `S9xDoHEventProcessing`, which
  shifted modem timing forward enough to break the BIOS. This
  helper reads through `Memory.Map[]` (and `XBand.sram[]` for
  `MAP_XBAND`) without touching `CPU.Cycles`.

- **Python probe tool** at `C:\Users\shany\xband_research\xband_probe.py`
  -- standalone TCP probe with multiple test scenarios (passive
  listen, identity send, brute-force identity variants, ADSP frame
  detection). Pure stdlib, no install. Used to discover that the
  bsnes-plus identity is accepted while the gameplay variant is
  rejected with `sayonara...`.

## Reference material

The XBAND firmware source code from Catapult Entertainment has been
publicly preserved and is available in the Cinghialotto/xband
repository on GitHub. We extracted the relevant pieces locally to
`C:\Users\shany\xband_research\catapult\` (not committed). The most
useful files for further work:

- `XBandOriginal/.../David Ashley/xband_src/xband/rr3/orig/fredequ.h`
  -- complete Fred / Rockwell modem register definitions, with bit
  names. This is the authoritative source for what each register
  does.

- `XBandOriginal/.../David Ashley/xband_src/xband/i/harddef.a` --
  modem status bit definitions (`kRMtxempty`, `kRMtxfull`,
  `kRMrxready` etc.).

- `XBandOriginal/.../Brett Bourbin/Source Code/SegaOS_src/ROMMain.c`
  -- the Genesis BIOS reset / boot path.

- `XBandOriginal/.../SegaOS_src/Database/` -- the database manager
  with CRC validation and TypeNode/ListNode structures.

- `SNES-XBandSRAMs.rar` (in the Snes/ subfolder) -- three preserved
  64KB SNES XBAND SRAM dumps.

- `Modem dial test/Modem.c` -- a Windows COM-port test program
  that contains the canonical CCITT-16 CRC tables and a verification
  routine. Comment in this file documents that the XBAND ROM has
  the same `ccitt_updcrc` function at `$D4:FB41`, accessed via OS
  function ID `$03D9`.

- bsnes-plus `xband_support` branch (commit `0d0a8e92c0`):
  `bsnes/snes/chip/xband/xband_base.cpp` is the closest reference
  implementation. Our register dispatch is a port of this file.

- **Kazade/dreampi** (https://github.com/Kazade/dreampi) -- a
  daemon that bridges a real Dreamcast modem to the internet via a
  Raspberry Pi. The `xband_server` function in `netlink.py` connects
  to the same `xbserver.retrocomputing.network:56969` endpoint and
  is the clearest working reference for how the XBAND wire
  protocol actually behaves. Cloned to
  `C:\Users\shany\xband_research\dreampi\`. Key insights from
  dreampi:
    1. Send identity (`///////PI-<hwid>\n`) once on connect, then
       just relay bytes both ways.
    2. The TX direction accumulates modem bytes until it sees the
       ADSP `\x10\x03` end marker, then sends the full packet as
       one `send()`.
    3. The RX direction is pure forwarding -- no inspection, no
       CRC validation, no HELO filter. Just bytes.
    4. dreampi has ZERO CRC code -- the validation happens in the
       hardware modem (or in our case, the BIOS).

## Suggested next steps

The next milestone blocker is **server-side user validation**. The
two realistic paths forward are:

1. **Self-host Roofgarden** (the community XBAND server software).
   Run it locally with one of the SRAM users (e.g. `0mEgA DeAtH`)
   pre-registered, point snes9x at `localhost:56969` via the
   already-existing host menu. Lets us reach the post-login UI and
   start exploring the actual game features (mail, opponent lookup,
   matchmaking). Effort: 1-2 hours setup. Per the
   `xband_post.txt` recipe, requires asterisk + a softmodem
   extension + Roofgarden but for direct emulator clients we may
   only need Roofgarden itself.

2. **Disassemble the BIOS validation function** that times out after
   "Connected to XBAND!" and NOP it out, similar to the existing
   loop-break ROM patches at `$D5:5B3D` and `$D5:40FC`. Higher
   effort but works against any server. The function we want is
   the one that handles the post-connect "wait for user data"
   timeout.

3. **Reach out to the retrocomputing.network admin** (argirisan)
   to ask about registering our test SRAM users. Lowest effort
   but blocked on someone else's response time. Worth trying in
   parallel with option 1.

4. **Investigate what specifically the BIOS dial-setup validation
   checks** so we can fix Luke2 / SF2DXB. Lower priority than the
   above since Benner already works for the next step.

## How to use what works today

1. Drop the XBAND BIOS at `win32/Roms/X-Band Modem BIOS (U).smc`.
2. Drop one of the three SRAM dumps from
   `Cinghialotto/xband/SNES-XBandSRAMs.rar` into `win32/BIOS/`.
   `Benner.1.SRM` is currently the only one that gets past the
   BIOS dial-setup validation.
3. Build and load the BIOS via `File -> Load Game` (or
   `File -> Open Multicart` with a slot-B game cart for future
   pass-through testing).
4. Use `Netplay -> XBAND: SRAM Benner.1.SRM (0mEgA DeAtH...)` to
   force loading Benner specifically (or skip this if it's the
   only `.srm` in the BIOS dir).
5. `Netplay -> XBAND: Connect to Server...` opens a TCP socket to
   `xbserver.retrocomputing.network:56969`. The default item uses
   the canonical hostname; explicit menu items exist for the other
   three resolvable hostnames.
6. Walk the BIOS UI: smart-card warning -> Yes, registration
   confirmation -> Yes, "Dialing XBAND..." -> "Connected to
   XBAND!" -> wallpaper -> "Sorry, connection lost".
7. Use `Netplay -> XBAND: Show Kill/Ctrl Trace` at any point to
   inspect the live protocol state, captured ADSP frames, and CRC
   validity.
8. Use `Netplay -> XBAND: Show CPU PC Histogram` and `XBAND: Show
   PPU State` for general BIOS execution diagnostics.
9. Standalone Python probe: `python C:\Users\shany\xband_research\xband_probe.py`
   for testing the server without snes9x.

## Branch history

```
f0fa297f  XBAND: SRAM dump selection menu + insight on "Translation problem"
1b37a52a  XBAND: ServerTalk decoder + TX frame capture + RX consumption counter
95961eab  XBAND: classify tiny ADSP control frames separately from bad-CRC
b67d6246  XBAND: identify ADSP CRC formula via multi-variant detector
ba3b9450  XBAND: connected! identity handshake + dreampi-style framing + ADSP detector
70584142  XBAND: multi-cart loader, loop-break ROM patches, BIOS now reaches main UI
512c5a60  XBAND: remove diagnostic popup, add status doc
03dbd9aa  XBAND: load SRAM dump from BIOS dir, refuse to overwrite on save
dad94cb3  XBAND: wire reset hook + bridge XBAND SRAM with snes9x save/load
f54f9444  XBAND: dump outer-loop region in PPU State debug view
4c4b1667  XBAND: add debug menu items for PC histogram and PPU state
f4ceafe9  XBAND: switch to bsnes-plus register model + fix SRAM mirror window
de97ffa5  XBAND: initial pass-through cartridge emulation scaffold
```
