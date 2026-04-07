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
  `S9xGetDirectory(BIOS_DIR)` for one of:
  `XBAND.srm`, `xband.srm`, `XBAND.bin`, `xband.bin`,
  `Benner.1.SRM`, `XBand_luke2.srm`, `SF2DXB.S04.srm`.
  The first 64KB file found is loaded into `XBand.sram[]`.
  We use `BIOS_DIR` rather than `SRAM_DIR` because snes9x never
  writes to `BIOS_DIR`, so the dump survives across runs.
  `Memory::SaveSRAM` is a no-op in XBAND mode for the same reason
  (don't clobber the dump with half-initialized garbage).

  The Cinghialotto/xband repo includes three preserved SNES XBAND
  SRAM dumps (`Benner.1.SRM`, `XBand_luke2.srm`, `SF2DXB.S04.srm`)
  -- drop one in `win32/BIOS/` to use it.

- **Save state coverage.** `SnapXBand` in `snapshot.cpp` serializes
  the full XBAND state (regs, modem regs, kill/control, rxbuf/txbuf,
  net step, sram). Snapshot version bumped to 13.

- **TCP socket bridging.** `S9xXBandConnect`, `S9xXBandDisconnect`,
  and `S9xXBandPoll` provide cross-platform (Winsock / POSIX) TCP
  bridging between the modem RX/TX FIFOs and a remote server.
  `S9xXBandPoll` is wired into `cpuexec.cpp` so it runs once per
  frame. The Win32 build has a Netplay menu entry to connect to
  `16bit.retrocomputing.network:56969` (the server hardcoded in the
  bsnes-plus xband_support branch -- the host is currently alive).

- **Diagnostic menus.** `Netplay -> XBAND: Show CPU PC Histogram`
  prints a per-bank PC histogram with a 4KB sub-histogram for bank
  `$D0`. `Netplay -> XBAND: Show PPU State` dumps the key PPU/CPU
  registers, current PC and surrounding bytes, and a window of the
  outer-loop region in bank `$D5`. Both are reset/refreshed on
  every open. The `cpuexec.cpp` deadlock handler also captures the
  first BRK/COP executed inside the firmware bank for post-mortem
  analysis.

## What does not work

- **The BIOS does not display anything.** Boot reaches a steady
  state where the firmware spins through what appears to be a
  database / timer scheduler loop, alternating between a 32-bit
  signed comparison helper at `$D0:11B1` and an outer record
  iterator at `$D5:5B27`. PPU is never enabled (`$2100 INIDISP`,
  `$212C TM`, `$4200 NMITIMEN` all stay zero). CGRAM is set up
  with a default palette, but with brightness 0 the screen renders
  pitch black.

- This was confirmed to **not depend on SRAM contents**: empty
  SRAM, Benner's dump, and Luke2's dump all produce the same
  loop, same PC, same registers.

- It was also confirmed to **not be a snes9x-specific issue**:
  Mesen running the same BIOS does not write to `$2100` either.
  No general-purpose SNES emulator gets the BIOS to display
  standalone -- which strongly suggests the BIOS is fundamentally
  designed to require either:
  1. A game cartridge plugged in on top (pass-through mode), or
  2. Live network traffic from a paired XBAND server (full ADSP
     protocol, not just raw TCP), or
  3. Specific hardware behaviour we are not yet emulating.

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
  -- the Genesis BIOS reset / boot path: `main -> BootStrap ->
  BootOS -> BootOSVector -> StartupSega -> DoIntroStuff`. The SNES
  BIOS is a port of the same architecture.

- `XBandOriginal/.../SegaOS_src/Database/` -- the database manager
  with CRC validation and TypeNode/ListNode structures. The SNES
  BIOS uses the same database design (this is what we suspect the
  stuck loop is iterating).

- `SNES-XBandSRAMs.rar` (in the Snes/ subfolder) -- three preserved
  64KB SNES XBAND SRAM dumps from real users.

- bsnes-plus `xband_support` branch (commit `0d0a8e92c0`):
  `bsnes/snes/chip/xband/xband_base.cpp` is the closest reference
  implementation. Our register dispatch is a port of this file.

## Suggested next steps

1. **Build and run bsnes-plus xband_support locally** with the same
   BIOS dump and the same SRAM file. If it shows the UI, find the
   first MMIO divergence between our trace and theirs. If it does
   not show the UI either, then "boots-but-no-display standalone"
   is the actual upper bound for any non-pass-through emulator and
   we should reframe the milestone accordingly.

2. **Implement pass-through cartridge mode.** The XBAND was always
   meant to sit between the SNES and a game cart. Loading a game
   ROM "on top of" the XBAND BIOS via a multi-cart-style loader
   (similar to how Sufami Turbo is handled in `LoadSufamiTurbo`)
   would let us test whether the firmware reaches its UI when it
   has a real game cart to detect.

3. **Implement the Fred patch chip.** The 11 patch vectors in
   the Fred general register file (offsets 0-41 of `regs[]`) need
   to actually intercept game ROM reads and substitute bytes. This
   is what XBAND used to redirect game controller polls so it could
   inject network-received inputs.

4. **Implement minimal ADSP framing.** The XBAND server speaks a
   modified Apple ADSP protocol on top of TCP, not raw bytes. Even
   if our socket connects, the firmware's network code may be
   waiting for valid ADSP frames before considering the link up.
   Sample packets are in
   `xband_research/sample_packets.txt` (from Cinghialotto).

5. **Static disassembly with IDA + symbol matching to the Catapult
   source.** The Genesis source has function names and structure
   definitions that should map onto the SNES disassembly. The
   currently-stuck loop at `$D0:11B1` / `$D5:5B27` has the visible
   shape of a database CRC validation pass; the real names from
   the source would tell us exactly which function it is and what
   condition is supposed to break it.

## How to use what works today

1. Drop the XBAND BIOS at `win32/Roms/X-Band Modem BIOS (U).smc`
   (or wherever, the loader will detect it by signature).
2. Drop one of the three SRAM dumps from
   `Cinghialotto/xband/SNES-XBandSRAMs.rar` into `win32/BIOS/` --
   any of `Benner.1.SRM`, `XBand_luke2.srm`, `SF2DXB.S04.srm`.
3. Build snes9x and load the BIOS via File -> Load Game.
4. Use `Netplay -> XBAND: Show PPU State` and
   `Netplay -> XBAND: Show CPU PC Histogram` to inspect what the
   firmware is doing.
5. `Netplay -> XBAND: Connect to Server...` opens a TCP socket to
   `16bit.retrocomputing.network:56969`. The server is currently
   alive but our network stack does not yet speak the ADSP framing
   the firmware expects.

## Branch history

```
03dbd9aa  XBAND: load SRAM dump from BIOS dir, refuse to overwrite on save
dad94cb3  XBAND: wire reset hook + bridge XBAND SRAM with snes9x save/load
f54f9444  XBAND: dump outer-loop region in PPU State debug view
4c4b1667  XBAND: add debug menu items for PC histogram and PPU state
f4ceafe9  XBAND: switch to bsnes-plus register model + fix SRAM mirror window
de97ffa5  XBAND: initial pass-through cartridge emulation scaffold
```
