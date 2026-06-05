---
name: gb-mappers
description: Reference for every Game Boy / Game Boy Color cartridge mapper (MBC) implemented in the snes9x `sgb/` core — register maps, banking math, quirks, detection heuristics, and the games each one fixes.
---

# Game Boy cartridge mappers (`sgb/` core)

This is the complete reference for how the snes9x SGB/GB/GBC core handles
cartridge memory-bank controllers. Everything here is implemented in:

| File | Role |
|------|------|
| `sgb/gb_mbc.h`   | `MbcType` enum, `MbcState` (the savestate-serialized struct), API |
| `sgb/gb_mbc.cpp` | `MbcReset` / `MbcRead` / `MbcWrite` — all banking + register logic |
| `sgb/gb_cart.cpp`| header parse, `$0147` → mapper table, structural unlicensed/multicart detection, SRAM allocation, battery I/O |
| `sgb/gb_memory.cpp` | routes CPU `$0000-$7FFF` / `$A000-$BFFF` to `MbcRead`/`MbcWrite`; boot-ROM overlay + `$FF50` |

MBC1, MBC3, and MBC5 cover ~95% of commercial carts. MBC2, HuC1, HuC3, MMM01,
Sachen MMC1, TAMA5, and M161 are also implemented. MBC6 and MBC7 are stubbed
(treated as read-only no-MBC).

---

## How a cart's mapper is chosen

`CartLoad` (`gb_cart.cpp`) picks the mapper in this priority order. The
unlicensed/multicart checks run **first**, because those carts forge their
`$0147` cart-type byte and must be identified by structural markers instead:

1. **Sachen scrambled logo** → `SachenMMC1` (the real Nintendo logo sits at
   address-bit-permuted offsets across `$0104-$01FF`).
2. **MMM01** → an extra Nintendo logo in the **last** 32 KiB (the boot menu).
   The "real" header (cart-type/rom/ram) is re-read from that menu bank.
3. **M161** → a second Nintendo logo at `$8000` *and* `$8148 == 0x00` (page 1
   is a flat 32 KiB game, not a banked sub-header).
4. Otherwise → **`DecodeCartType($0147)`** (table below). Unknown ⇒ load fails.

Two more *sub-variants* are flagged on top of a base mapper (they don't change
`MbcType`, they set a `Cart` bool):

- `mbc1_multicart` — base MBC1 **and** a second logo at `$40104` ⇒ MBC1M.
- `duz_multicart`  — base MBC3 **and** logos at both `$0104` and `$8104` with
  `$8148 != 0x00` ⇒ the "Duz" 2-in-1 register-port multicart.

### `$0147` cart-type → mapper (`DecodeCartType`)

| `$0147` | Mapper | battery / rtc / rumble |
|--------|--------|------------------------|
| `00` | None | — |
| `01`–`03` | MBC1 | `03` battery |
| `05`–`06` | MBC2 | `06` battery |
| `08`–`09` | None (ROM+RAM) | `09` battery |
| `0B`–`0D` | MMM01 | `0D` battery |
| `0F`–`10` | MBC3 | battery + RTC |
| `11`–`13` | MBC3 | `13` battery |
| `19`–`1E` | MBC5 | `1B`/`1E` battery, `1C`–`1E` rumble |
| `20` | MBC6 *(stub)* | — |
| `22` | MBC7 *(stub)* | battery + rumble |
| `FD` | TAMA5 | battery + RTC |
| `FE` | HuC3 | battery + RTC |
| `FF` | HuC1 | battery |

Header fields also parsed: title (`$0134`, CGB-aware), CGB flag (`$0143`),
SGB flag (`$0146`), ROM size (`$0148`), RAM size (`$0149`), destination
(`$014A`), header checksum (`$014D`, validated), global checksum (`$014E`).

---

## Shared conventions

**`MbcState` is savestate ABI.** The whole struct is blob-serialized by
`sizeof`, so **adding or removing a field breaks every existing GB savestate.**
New mappers therefore *reuse* existing fields rather than add new ones (TAMA5
and the Duz multicart both borrow the Sachen/RTC fields — see their sections).

**Fresh SRAM is initialized to `$FF`**, not zero (`gb_cart.cpp`). Real
battery-less / first-boot SRAM cells read `$FF`, and games use that as the
"no save present" sentinel. (Zero-init broke *Initial D Gaiden*: it thought a
save existed and rendered an empty dialog box.) MBC2 gets 512×4-bit, TAMA5 gets
a 32-byte EEPROM, otherwise the header RAM size is honored.

**SRAM stays readable even when "disabled" for MBC5** (`MbcRead` line ~467:
`if (!ram_enable && type != MBC5) return 0xFF`). *Nettou King of Fighters '97*
trampolines executable code out of SRAM during SGB boot and hangs if a disabled
SRAM reads back `$FF`.

**ROM/SRAM reads wrap** (`offset % size`) so out-of-range banking can't crash.

---

## None ( `$00` / `$08` / `$09` )

Flat 32 KiB ROM at `$0000-$7FFF`, optional 8 KiB RAM at `$A000-$BFFF`. No
banking registers. Writes to `$A000-$BFFF` go straight to SRAM.

---

## MBC1 ( `$01`–`$03` )

The classic mapper. Up to 2 MiB ROM / 32 KiB RAM.

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | RAM enable (low nibble `== $0A`) |
| `$2000-$3FFF` | ROM bank, low 5 bits (`0`→`1` auto-correct) |
| `$4000-$5FFF` | BANK2: 2 bits — RAM bank **or** ROM bank bits 5-6 (mode-dependent) |
| `$6000-$7FFF` | Mode: `0` = ROM banking, `1` = RAM banking |

- `$4000-$5FFF` is stored raw; the mode gate is applied **at read time**.
- BANK2 supplies ROM A19-A20: `$0000-$3FFF` is normally bank 0, but mode 1 on a
  ≥1 MiB cart exposes banks `$20/$40/$60` there (`Mbc1Bank0`).
- The bank-0→1 / `$20`→`$21` etc. "bank 0 is unreachable in the high window"
  quirk is handled in `Mbc1BankN`.

### MBC1 multicart (MBC1M) — sub-variant

Flagged by `mbc1_multicart`. Here BANK2 drives ROM **A18-A19** (one bit lower),
so each BANK2 value selects a **256 KiB game slot** and BANK1 keeps only its
low 4 bits (`Mbc1Bank0`/`Mbc1BankN` take a `multicart` flag). The slot's own
bank-0 (menu / sub-game header) appears at `$0000-$3FFF`. Multicarts carry no
cart RAM. **Fixes:** *"Mortal Kombat & Mortal Kombat II"* game-select menu.

---

## MBC2 ( `$05`–`$06` )

Up to 256 KiB ROM, plus **512×4-bit built-in RAM** (no external RAM chip).

| Range | Write effect |
|-------|--------------|
| `$0000-$3FFF` | bit 8 of address selects function: `A8=0` RAM enable, `A8=1` ROM bank (low 4 bits, `0`→`1`) |
| `$A000-$BFFF` | low nibble stored to the 512×4-bit RAM (`addr & $1FF`) |

RAM reads OR in `$F0` (upper nibble is unwired). SRAM allocated 512 bytes of
`$FF` regardless of the header RAM-size byte.

---

## MBC3 ( `$0F`–`$13` )

Up to 2 MiB ROM / 32 KiB RAM, plus an optional real-time clock.

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | RAM/RTC enable (`$0A`) |
| `$2000-$3FFF` | ROM bank, 7 bits (`0`→`1`) |
| `$4000-$5FFF` | `$00`–`$07` RAM bank, `$08`–`$0C` RTC register select |
| `$6000-$7FFF` | RTC latch: `0`→`1` edge copies live RTC into the latched regs |
| `$A000-$BFFF` | RAM, or the selected latched RTC register |

RTC = 5 counters (`rtc_regs`) + 5 latched (`rtc_latched`) + latch gate. Reads
in the RAM window return the **latched** RTC value when an RTC register is
selected.

### Duz 2-in-1 multicart — sub-variant

Flagged by `duz_multicart` (e.g. *Pokemon Red/Blue 2-in-1*). A genuine MBC3
whose menu selects the active game through a **custom register port in the SRAM
window**, reusing Sachen fields to avoid growing `MbcState`:

- `sachen_outer_mask` = port-unlock latch: a `$C0` write to `$0000-$1FFF` arms
  it; any other write disarms.
- While armed, `$A000` latches a register index into `sachen_unlock_ctr`;
  `$A100` writes it. Only index `$A3` (ROM base, in 32 KiB pages) matters:
  `sachen_outer_bank = value << 1`.
- Effective bank = `(selected MBC3 bank) + sachen_outer_bank`; the `$0000-$3FFF`
  window maps to `sachen_outer_bank` (the sub-game's base).

---

## MBC5 ( `$19`–`$1E` )

The CGB-era mapper. Up to 8 MiB ROM / 128 KiB RAM, optional rumble.

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | RAM enable (`$0A`) |
| `$2000-$2FFF` | ROM bank low 8 bits (bank `0` is selectable, no remap) |
| `$3000-$3FFF` | ROM bank bit 8 (value bit 0) |
| `$4000-$5FFF` | RAM bank `$0`–`$0F` (bit 3 = rumble on rumble carts; not driven here) |

**Quirk:** SRAM stays *readable* while disabled (see Shared conventions; KOF '97).

---

## MBC6 ( `$20` ) / MBC7 ( `$22` ) — stubbed

Recognized by the type table but not implemented; `MbcWrite` ignores their
registers and they behave as read-only no-MBC. MBC7 (accelerometer/EEPROM,
e.g. *Kirby Tilt 'n' Tumble*) and MBC6 (*Net de Get*) are not yet supported.

---

## HuC1 ( `$FF` ) — Hudson, MBC1-like + infrared

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | RAM/IR **select**: value `$0E` routes `$A000-$BFFF` to the IR register, anything else to cart RAM. *No separate RAM enable* — RAM is always live (`ram_enable` doubles as the select; defaulted true in `MbcReset`). |
| `$2000-$3FFF` | ROM bank, low 6 bits |
| `$4000-$5FFF` | RAM bank, low 2 bits |
| `$A000-$BFFF` | cart RAM, or IR LED (writes discarded). IR **reads** return `$C0` (no light / no link partner). |

**Fixes:** *Chousoku Spinner* — dropped ROM-bank writes caused a `$FC` illegal-
opcode crash (PR #88).

---

## HuC3 ( `$FE` ) — Hudson, RTC command interface + IR

Never coexists with MBC3, so it **borrows the MBC3 RTC fields** for its own
state (`rtc_select` = window mode, `rtc_regs[0]` = register pointer,
`rtc_regs[1]` = last extended command, `rtc_regs[2]` = response nibble,
`rtc_regs[3..4]`+`rtc_latched[0..1]` = minutes/days).

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | `$A000-$BFFF` window mode (low nibble): `$0/$A` RAM (ro/rw), `$B` RTC command, `$C` response, `$D` semaphore, `$E` IR |
| `$2000-$3FFF` | ROM bank, 7 bits (MBC5-style, no `0`→`1`) |
| `$4000-$5FFF` | RAM bank, low 2 bits |
| `$A000-$BFFF` | per mode: RAM, or RTC command (executes immediately) |

Reads by mode: `$C` returns `1` when the boot status command `$62` was issued
(boot gate), `$D` semaphore always reads ready (`1`), `$E` IR reads `0`.

**Fixes:** *Robopon* — boots once the RTC status command `$62` reads back `1`
(PR #89).

---

## MMM01 ( `$0B`–`$0D` ) — Nintendo multicart meta-mapper

Used by Mani / Taito compilation carts. After power-on the **last 32 KiB (the
menu)** is mapped at `$0000-$7FFF`; the menu programs ROM/RAM base+mask
registers, then writes bit 6 of `$0000-$1FFF` to **lock** into game mode, where
the chosen sub-game runs under MBC1-style banking inside its allotted window.

- Field set mirrors SameBoy's `mmm01` struct (`mmm01_rom_bank_low/mid/high`,
  `mmm01_ram_bank_low/high`, `mmm01_rom_bank_mask`, `mmm01_ram_bank_mask`,
  multiplex/mbc1-mode flags, `mmm01_locked`).
- `Mmm01Rom0Bank` forces all outer ROM lines high while unlocked (so the menu
  is visible at `$0000`); once locked, the base overlays the masked low bits.
- `mmm01_just_locked` is a one-shot flag the SGB layer polls: on lock it injects
  PAL01/PAL23 packets forcing the 4 SGB palettes to grayscale, else the
  sub-game inherits the menu's (often inverted) palette.

Detection re-reads the true cart-type/rom/ram from the menu bank; battery only
honored if the menu-bank type is the real MMM01+RAM+BATTERY (`$0D`).

---

## Sachen MMC1 ( forged `$01` ) — unlicensed 4B-series multicarts

Detected structurally: the real Nintendo logo is hidden under an **address
bit-permutation** (`A0↔A6`, `A1↔A4`) across `$0104-$01FF`, so
`LooksLikeSachenScrambledLogo` finds it at the permuted offsets even though the
header lies (claims MBC1).

**Key insight (the whole mechanism):** the bit-permutation exists **only** to
make the boot ROM's Nintendo-logo check pass. What the cart does *after* boot
splits into two kinds, which is why the lock (`SachenLockedHeaderXform` /
`sachen_locked`) is **per-cart**, decided at load by `Cart::sachen_runs_raw`
(true ⇔ the scrambled entry decodes to a `JP` back into the scrambled header
region itself, `< $0200` — no real loader there):

- **runs-raw carts** (e.g. 4B-005, scrambled entry `JP $0150` = a per-game logo
  checksum, not a loader). The game is actually stored *unscrambled*. The lock
  is **dropped at the `$FF50` boot hand-off** (`gb_memory.cpp`) and starts clear
  in BIOS-less mode (`sgb.cpp`: `sachen_locked = boot_rom_enabled || !runs_raw`),
  so `$0100` executes raw — matching Mesen (which runs these raw, no mapper).
- **scrambled-entry carts** (4B-007 `JP $6F60`, 4B-008 `JP $6200`, 4B-009 `JP
  $0200`). The xform **stays on** so the entry decodes to its real loader; the
  cart then runs from raw ROM and clears the lock via `sachen_unlock_ctr` /
  `MbcNotifyHighWrite` (the `$31`-write counter — *not* vestigial for these).
  4B-009 is why the boundary is `$0200`, not a bank line: its loader sits at
  `$0200`, just past the header, still inside bank 0.

Getting this wrong hangs one kind or the other: keeping the xform always-on
hangs 4B-005 in its `$0158` checksum loop; dropping it always-off sends 4B-008's
raw `JP $00CE` into `$FF`-filled memory → the `RST $38` loop.

Banking registers (latch only while inner-bank `D5:D4 == 0b11`, per Tauwasser):

| Range | Write effect |
|-------|--------------|
| `$0000-$1FFF` | outer bank (`sachen_outer_bank`) |
| `$2000-$3FFF` | inner ROM bank (`rom_bank`, `0`→`1`) |
| `$4000-$5FFF` | outer mask (`sachen_outer_mask`) |

Effective bank = `outer & mask` (`$0000-$3FFF`) or `(outer & mask) | (inner &
~mask)` (`$4000-$7FFF`).

**Fixes:** *4 in 1* **4B-005** (Sachen-Commin), **4B-007**, **4B-008**, **4B-009**
— all boot to their game-select menus.

---

## TAMA5 ( `$FD` ) — Bandai Tamagotchi

Not the usual banking — a **nibble register port** plus a 32-byte EEPROM and a
TAMA6 RTC. Reuses existing fields (`rom_bank`, `rtc_select` as the `$A001`
latch, `sachen_outer_bank`/`sachen_outer_mask` as packed write-data/address-and-
mode bytes, RTC fields as the BCD timer page).

| Address | Effect |
|---------|--------|
| `$A001` | register select (which TAMA5 register the next `$A000` access hits) |
| `$A000` | nibble data port |

Registers: `0`/`1` ROM bank lo/hi, `4`/`5` write-data lo/hi, `6`/`7` address+mode
(writing reg `7` **commits** the op), `$A` ready handshake (reads `$F1`), `$C`/`$D`
read-result lo/hi. The 5-bit address + 3-bit mode selects an EEPROM cell or a
TAMA6 RTC register/command (`Tama5Trigger`). RTC seeded from host localtime on
reset (`Tama5SeedRtc`).

**Fixes:** *Game de Hakken!! Tamagotchi — Osutchi to Mesutchi* loads.

---

## M161 ( forged `$11`/MBC3 ) — Mani 4-in-1 "Tetris Set"

A **write-once** register selects one of up to eight raw **32 KiB pages** into
the entire `$0000-$7FFF` window. Detected structurally (second Nintendo logo at
`$8000`, `$8148 == 0x00`).

- Power-on page = 0 (the menu); `MbcReset` sets `rom_bank = 0`.
- The first write to `$0000-$7FFF` latches `value & 0x07` as the page; all later
  writes are ignored (`mbc1_mode` reused as the "latched" flag).
- Reads map the whole window flat: `(page << 15) | (addr & 0x7FFF)`.

**Fixes:** *Mani 4-in-1 (Tetris + Alleyway + Yakuman + Tennis)* game-select.

---

## Diagnosing mapper bugs

The headless harness (`docs/debugger/gb-headless-harness/`) drives the real
`sgb/` core with no GUI — load a raw ROM, run frames, watch PC/banking. For
boot-dependent mappers (Sachen logo check, MMM01 menu) stage the boot ROM and
run boot → cart so the `$FF50` hand-off and post-boot raw execution happen
exactly as on hardware. The Win32 debugger's **Mapper panel** (`CMapperPanel`)
shows the live mapper type, ROM/RAM bank, and per-mapper state in the GUI.
