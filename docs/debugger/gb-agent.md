---
name: gb-debugger-agent
description: Reference for working on the GB side of the snes9x debugger. Covers SM83 CPU state, the bus, MBC bank switching, boot-ROM overlay, side-effect-free reads, and the SGB facade.
---

# GB / SGB debugger reference

Use this when touching anything in `win32/CDebuggerGb.cpp`, `win32/CDisasmGb.cpp`, `sgb/gb_cpu.cpp`, `sgb/gb_memory.cpp`, or the C facade in `sgb/sgb.cpp`.

## Address format used in the debugger UI

GB PCs and BP addresses in our UI are **24-bit display addresses**: `((bank & 0xFF) << 16) | (cpu_addr & 0xFFFF)`.

`bank` here is the **cart ROM bank number** (0..N-1), not a CPU bank in the SNES sense. The GB CPU only has one address space, `$0000-$FFFF`. The display-address `bank` field is meaningful only for `cpu_addr ∈ $4000-$7FFF` (the MBC-switchable window) — for everything else (`$0000-$3FFF`, `$8000-$FFFF`) the `bank` field is 0.

`CurrentDisplayPC` in `CDebuggerGb.cpp` constructs this from the live `s.r.pc` and `S9xSGBGetCurrentRomBank()`.

## SM83 / LR35902 registers we read

Live in `SGB::CpuState s` (returned from `impl_->cpu.State()`). Accessed externally via `S9xSGBGetCpuRegs`:

| Field      | Meaning                                                |
|------------|--------------------------------------------------------|
| `s.r.af`   | 8-bit A in high byte, flags F (Z,N,H,C) in low nibble (bits 7-4) |
| `s.r.bc`   | B (high) + C (low)                                     |
| `s.r.de`   | D (high) + E (low)                                     |
| `s.r.hl`   | H (high) + L (low)                                     |
| `s.r.sp`   | Stack pointer (16-bit)                                 |
| `s.r.pc`   | Program counter (16-bit)                               |
| `s.ime`    | Interrupt master enable                                |
| `s.halted` | CPU halted (waiting for IRQ to wake; PC stays put)     |
| `s.stopped`| CPU stopped (waiting for joypad IRQ; PC stays put)     |
| `s.t_cycles`| Cumulative T-cycle count                              |
| `s.halt_bug` | DMG halt-bug pending (next fetch re-reads byte)      |
| `s.ime_pending` | EI's one-instruction delay                        |

F-register flag bits: Z=bit 7, N=bit 6, H=bit 5, C=bit 4. Low 4 bits always read 0.

## GB CPU memory map ($0000-$FFFF)

| Range         | Region                                  | Notes |
|---------------|-----------------------------------------|-------|
| `$0000-$3FFF` | PRG ROM bank 0                          | Fixed bank, **but boot ROM overlays $0000-$00FF when `boot_rom_enabled`** |
| `$4000-$7FFF` | PRG ROM bank N                          | Bank chosen by `MbcState::rom_bank` (MBC1/MBC3/MBC5/MBC2/Sachen) |
| `$8000-$9FFF` | Video RAM (8KB)                         | PPU bus contention; reads during mode 3 return $FF on real DMG |
| `$A000-$BFFF` | External cart RAM (8KB window)          | Optional; MBC controls bank within the cart's SRAM file |
| `$C000-$DFFF` | Work RAM (8KB; CGB: 4 switchable banks of upper 4KB) |       |
| `$E000-$FDFF` | Echo RAM                                | Mirror of `$C000-$DDFF`; writes go to WRAM too |
| `$FE00-$FE9F` | Sprite RAM (OAM, 160 bytes)             | OAM corruption bug exists on real DMG |
| `$FEA0-$FEFF` | Unusable                                | Reads return $FF or $00 depending on family |
| `$FF00-$FF7F` | I/O Registers                           | Most side-effecting; PeekRAByte returns 0 for these |
| `$FF80-$FFFE` | High RAM (HRAM, 127 bytes)              | Independent fast RAM |
| `$FFFF`       | IE register                             |       |

### **Boot ROM overlay — the gotcha**

When `m.boot_rom_enabled` is true (set during BIOS-mode Reset), the bytes at CPU `$0000-$00FF` are served from `m.boot_rom[]`, **not** from `m.cart->rom[]`. The boot ROM is the 256-byte (DMG/SGB) or 2048-byte (CGB) Nintendo splash + handshake routine. Its opcodes are entirely different from whatever the cart has at the same addresses.

Boot disables itself by writing `$01` to `$FF50` as its final act; `gb_memory.cpp` clears `boot_rom_enabled` on that write, and the cart bytes underneath become visible.

**Implications for the debugger**:
- Any read path used by the disasm cache must consult `boot_rom_enabled` for `addr < 0x100`, otherwise the cache walks cart bytes while the CPU executes boot bytes, instruction boundaries diverge, and `FindIndexForPC` returns -1 for legitimate PCs.
- The cache is built once when the debugger opens. If built during boot, it's a snapshot of boot bytes; after `$FF50` flips, the cache is stale for `$0000-$00FF`. A "boot transition → invalidate cache" hook is needed for that case.

## MBC bank switching

`S9xSGBGetCurrentRomBank()` returns the current bank number selected for the `$4000-$7FFF` window. Implementation: reads `impl_->cart.mbc.rom_bank` (after MBC translation).

Bank 0 mapped to the switchable window:
- MBC1: bank 0 in the low-bank register selects bank 1 (hardware quirk). Our MBC code already handles this.
- MBC3/MBC5: bank 0 means bank 0 (mirrors $0000-$3FFF).

The disasm walks the linear ROM image (00:0000 → 00:3FFF → 01:4000 → 01:7FFF → 02:4000 → ...). `AdvancePC` for GB handles the bank-boundary transitions:
- `bank=0, cpu reaches $4000`: → `bank=1, cpu=$4000`
- `bank=N>0, cpu reaches $8000`: → `bank=N+1, cpu=$4000`
- `bank=N` past last bank: clamp to `$7FFF` and stop

This is **a layout fiction for the disasm view**, not what the CPU does at runtime. At runtime, the CPU keeps `cpu` in `$0000-$FFFF` and the MBC remaps `$4000-$7FFF` to whichever bank the cart wrote. Our linear walk lets the user scroll through the whole cart contiguously.

## Reading bytes side-effect-free

The C facade in `sgb/sgb.cpp`:

| Function                       | What it reads                              | Honours boot overlay? |
|--------------------------------|--------------------------------------------|-----------------------|
| `S9xSGBPeekRAByte(addr)`       | GB CPU bus at `addr` (16-bit), no side effects | **YES** (since fix `8e203afb`) |
| `S9xSGBPeekROMByte(rom_off)`   | Raw cart ROM byte by **linear file offset** | N/A — cart only |
| `S9xSGBPeekBootROMByte(off)`   | Boot ROM byte by offset (0..255)            | N/A — boot only |
| `S9xSGBGetROMSize()`           | Cart ROM byte count                         |                       |
| `S9xSGBGetBootROMSize()`       | 256 (DMG/SGB) — hardcoded for now           |                       |

**PeekRAByte vs MemRead must stay in sync**. They're the side-effect-free and live versions of the same bus read. The live `MemRead` in `gb_memory.cpp` handles boot overlay; PeekRAByte must too. If you find a discrepancy (a new MemRead branch that Peek lacks, or vice versa), that's a debugger-correctness bug.

`GbBackend::ReadByte` in `CDebuggerGb.cpp` is the disasm's per-byte reader. It dispatches:
- bank=0, cpu<$4000 → `PeekRAByte(cpu)` (handles boot overlay)
- bank>0, cpu in $4000-$7FFF → `PeekROMByte(bank * 0x4000 + (cpu - 0x4000))` (specific bank's bytes; not affected by current MBC bank selection)
- otherwise → `PeekRAByte(cpu)` (RAM regions; VRAM is gated by PPU, returns 0 if not exposed)

## SM83 instruction set notes for the disasm

- One-byte opcodes for most; `$CB`-prefixed two-byte opcodes for bit ops / rotates / shifts.
- Operand sizes are encoded in opcode (no width-flag latching like SNES M/X). Length is fixed by `op` and possibly `Fetch16`.
- `STOP` (`$10`) is two bytes — the byte after `$10` is consumed as a filler.
- HALT bug: if `HALT` executes with `IME=0` and an IRQ is pending, the **next** byte is read twice. `Cpu::Step` decrements PC after Fetch8 to compensate. The trace hook fires *after* this decrement, so `pc_at_fetch` and `state_.r.pc` agree on the right value to display.
- Conditional control flow: `JR cc,e` / `JP cc,nn` / `CALL cc,nn` / `RET cc`. Branch target depends on flags; for the disasm view we always treat branches as taken for label inference.

## Cpu::Step hook contract

`sgb/gb_cpu.cpp:78-96`:
```cpp
const uint16_t pc_at_fetch = state_.r.pc;
const uint8_t  op          = Fetch8(state_, mem);   // PC = pc_at_fetch + 1
if (state_.halt_bug) { state_.r.pc--; state_.halt_bug = false; }   // PC back to pc_at_fetch

if (g_trace_hook) g_trace_hook(pc_at_fetch, op, state_);

if (S9xSGBDebuggerBreakRequested()) {
    state_.r.pc = pc_at_fetch;   // rewind so the to-be-executed instruction is the next dispatch
    return;
}

Dispatch(state_, mem, op);
```

When the hook routes through `CDebugger::OnGbPreInstruction → HaltGbNow → S9xSGBRequestDebuggerBreak`, the rewind on line 94 ensures the displayed PC is the address of the instruction *about to execute*, not the one *after*. Step Over depends on this — if you change the rewind, Step Over breaks.

## SGB BIOS mode specifics

- The GB is **gated** by `S9xSGBBIOSGBIsReleased()` in `S9xSGBSyncToSnesCycle`. Before BIOS writes `r6003.d7 = 1`, no GB cycles execute. `Pause(DbgSystem::Gb)` correctly avoids freezing the SNES so BIOS can reach the release point — see `gb_break_pending_` in `CDebugger.cpp`.
- Authentic BIOS mode (`boot_rom_loaded` true): GB Resets with `PC = $0000` and `boot_rom_enabled = true`. The Nintendo boot routine runs and unmaps itself.
- BIOS-less mode (`boot_rom_loaded` false): GB Resets with `PC = $0100`, `boot_rom_enabled = false`, post-boot register values pre-loaded from `Cpu::Reset()`.

## Common GB-debugger gotchas

- **PeekRAByte must match MemRead.** Any new MemRead branch (overlay, MBC quirk, CGB WRAM banking, etc.) must be mirrored in PeekRAByte or the disasm will lie.
- **Disasm cache is a snapshot.** Boot transition, MBC bank switches, or cart-RAM remapping invalidate ranges of the cache; we don't yet auto-invalidate.
- **`s.r.pc` after rewind**: if you change anywhere in `Cpu::Step` to read PC, do it before line 79 (Fetch8) or use `pc_at_fetch`.
- **`gb_break_pending_` only triggers on the next *dispatched* instruction.** If GB is `halted`/`stopped` and no IRQ fires, the hook never runs and the trap stays armed forever. The status bar says "Waiting for GB execution..." in that state.
- **Bank in display address ≠ MBC bank latch.** `Disasm row.pc = ((view_bank << 16) | cpu)`; this is for the linear-walk view. The actual MBC bank at runtime could be any of them. BP matching for `cpu ∈ $4000-$7FFF` uses `S9xSGBGetCurrentRomBank()` to compare against the BP's stored bank.
