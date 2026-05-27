---
name: snes-debugger-agent
description: Reference for working on the SNES side of the snes9x debugger. Covers 65816 CPU state, memory bus, banks, hooks, side-effect-free reads, and the snes9x globals to read.
---

# SNES debugger reference

Use this when touching anything in `win32/CDebugger*.cpp`, `win32/CDisasm65816.cpp`, `getset.h`, or `cpuexec.cpp` related to the SNES side of the debugger.

## Address format used in the debugger UI

All SNES PCs and BP addresses in our UI are **24-bit display addresses**: `((bank & 0xFF) << 16) | (cpu_addr & 0xFFFF)`.

- `bank` = the value in the program-bank register `Registers.PB` for a code address, or `Registers.DB` for a data access where applicable.
- `cpu_addr` = the 16-bit address as the CPU sees it.

This is what `CDisasmPanel::BankStart` and the entire cache use. Never store a "flat ROM offset" in `DisasmLine::pc` — always the display address.

## 65816 registers we read for the status panel

All live in `struct SRegisters Registers` (`65c816.h`):

| Field          | Meaning                                          |
|----------------|--------------------------------------------------|
| `Registers.A.W`/`.B.l`/`.B.h` | Accumulator (M flag selects 8/16 bit)        |
| `Registers.X.W`/`.B.l`        | X index (X flag selects 8/16 bit)            |
| `Registers.Y.W`/`.B.l`        | Y index                                       |
| `Registers.S.W`/`.B.l`        | Stack pointer (E flag clamps low byte=$01)    |
| `Registers.D.W`               | Direct page register                          |
| `Registers.DB`                | Data bank register                            |
| `Registers.PB`                | Program bank register                         |
| `Registers.PCw`               | Program counter                               |
| `Registers.P.W`               | Processor status (NVMXDIZC + E)               |

`Registers.P.B.l` bits — and what they do for the disassembler:

| Bit | Mask | Name              | Effect on disasm |
|-----|------|-------------------|------------------|
| 7   | 0x80 | N (Negative)      | none (display)   |
| 6   | 0x40 | V (Overflow)      | none             |
| 5   | 0x20 | **M (Memory)**    | **A/operand width: 0 = 16-bit, 1 = 8-bit** |
| 4   | 0x10 | **X (Index)**     | **X/Y width: 0 = 16-bit, 1 = 8-bit**       |
| 3   | 0x08 | D (Decimal)       | ALU mode (no disasm impact) |
| 2   | 0x04 | I (IRQ Disable)   | none             |
| 1   | 0x02 | Z (Zero)          | none             |
| 0   | 0x01 | C (Carry)         | none             |

M and X **change the byte count of immediate operands** for `LDA #$xx` / `LDX #$xx` etc. Always pass current M and X to `CDisasm65816::Disassemble`; otherwise lengths are wrong and the cache walk overshoots / undershoots.

`Registers.PCw` is 16-bit. PC in display format is `((Registers.PB & 0xFF) << 16) | Registers.PCw`.

## SNES memory bus

`S9xGetByte(uint32_t addr24)` in `getset.h` is the **live** CPU read with all side effects (cycle counting, PPU/APU triggers, DMA hooks, BSX, SA-1, etc.). **Never** call it from the debugger's read path or you'll permute emulation state during a paint.

For side-effect-free reads use `SnesBackend::ReadByte` (`CDebuggerSnes.cpp`). It looks up `Memory.Map[]` and reads through directly. Notes:

- Memory.Map[] is a block table indexed by `(addr >> MEMMAP_SHIFT)`. Slots can be:
  - `< MAP_LAST` — pointer constants. Means I/O register; we return 0 (no peek).
  - Normal pointer — raw backing store. Read directly.
- SRAM mask: `Memory.SRAM[addr & Memory.SRAMMask]` for SRAM-mapped blocks. Always use the mask, not `addr - <some base>`.
- WRAM mirror: banks `$7E-$7F` are linear `Memory.RAM[0x0000-$1FFFF]`. Banks `$00-$3F`/`$80-$BF` mirror `$0000-$1FFF` into `Memory.RAM[0x0000-0x1FFF]`.
- `Memory.CalculatedSize` is the actual ROM byte count, not the cart-header value. Use it for "PRG ROM size" in the memory viewer.
- `Memory.SRAMMask + 1` is the actual SRAM byte count when SRAM exists; if `SRAMMask == 0`, there is no SRAM.

## Bank addressing rules our disassembly relies on

- Banks `$00-$3F` and `$80-$BF` LoROM: `$0000-$7FFF` are MMIO/WRAM mirror/SRAM/etc.; `$8000-$FFFF` is ROM.
- Banks `$40-$7D`, `$C0-$FF` LoROM: full `$0000-$FFFF` is ROM.
- HiROM banks `$00-$3F` and `$80-$BF`: `$8000-$FFFF` is ROM (high half of ROM-bank); the LoROM-style mirroring still applies for `$0000-$7FFF`.
- HiROM banks `$40-$7D`, `$C0-$FF`: full ROM.

`AdvancePC` for SNES wraps PC within the **same bank**: `(pc & 0xFF0000) | ((uint16_t)(pc + len) & 0xFFFF)`. This is correct — the 65816 wraps PC on bank boundary. The next instruction after `$XX:FFFF` is `$XX:0000`, not `$(XX+1):0000`. Long branches (`JSL`/`JML`) explicitly change PB; the disassembler must surface that.

## Hook points we own (do not gate on `DEBUGGER` macro)

| Site                          | Function                              | Purpose |
|-------------------------------|---------------------------------------|---------|
| `cpuexec.cpp` opcode dispatch | `S9xDebuggerOnSnesPreInstruction()`   | Per-instruction halt/BP check |
| `getset.h` `S9xGetByte` entry | `S9xDebuggerOnSnesMemAccess(a,v,false)`| R BP check, gated on `g_debugger_check_rw` |
| `getset.h` `S9xSetByte` entry | `S9xDebuggerOnSnesMemAccess(a,v,true)` | W BP check, gated on `g_debugger_check_rw` |

Both R/W hooks are off the hot path when no R/W BPs exist (`g_debugger_check_rw` defaults false; `RecomputeAttached`/`OnBpsChanged` flip it).

## Halt mechanics

`HaltSnesNow` is the only correct way to pause SNES from a hook. It does three things:
1. `Settings.Paused = true`
2. `CPU.Flags |= SCAN_KEYS_FLAG` — makes the inner opcode loop in `cpuexec.cpp` bail at the next opcode boundary
3. `RefreshSnes()` — posts WM_USER_DEBUGGER_REFRESH to the SNES debugger dialog

Without SCAN_KEYS_FLAG the loop runs to end-of-frame.

## Common SNES-debugger gotchas

- **M/X latching**: the disassembler uses the *current* M/X. When the user scrolls back through code, the rows above PC were emitted at a different M/X. Today we accept that imprecision; structured re-disassembly with per-row M/X tracking would be Phase 3.
- **`Memory.SRAMSize` vs `Memory.SRAMMask + 1`**: SRAMSize is the encoded cart-header value, not bytes. Always use the mask.
- **`Registers.PB` is a `uint8`**: when synthesising display addresses, cast to uint32 before shifting.
- **DMA / HDMA channels**: don't read FillRAM/MMIO via SnesBackend::ReadByte expecting current values mid-DMA. The hardware-fill values land in `Memory.FillRAM` but I/O writes alias.
