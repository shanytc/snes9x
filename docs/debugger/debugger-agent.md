---
name: debugger-agent
description: Architecture and contracts for the multi-panel Win32 debugger spanning SNES, GB, and SGB. Read alongside snes-agent and gb-agent.
---

# snes9x debugger architecture

Cross-system Win32 debugger for SNES (`.smc`/`.sfc`), GB/GBC (BIOS-less), and SGB (BIOS mode runs both at once). Single source tree, two dialogs.

## Top-level files

| File                              | Role |
|-----------------------------------|------|
| `win32/CDebugger.{h,cpp}`         | Central state machine: attach/detach, breakpoints, halt/step/run, hook plumbing |
| `win32/CDebuggerDlg.{h,cpp}`      | One dialog per system (`gSnesDebuggerHWND` / `gGbDebuggerHWND`); menu, toolbar, splitters, status bar |
| `win32/CDebuggerSnes.{h,cpp}`     | SNES backend: register snapshot, side-effect-free reads |
| `win32/CDebuggerGb.{h,cpp}`       | GB backend: register snapshot, side-effect-free reads (via SGB facade) |
| `win32/CDisasm65816.{h,cpp}`      | 65816 disassembler (mnemonic table + operand decoder, lifted from `debug.cpp`) |
| `win32/CDisasmGb.{h,cpp}`         | SM83 disassembler |
| `win32/CDisasmPanel.{h,cpp}`      | Virtual-mode ListView disassembly; same code for both systems |
| `win32/CStatusPanel.{h,cpp}`      | Register grid + flags + PPU state |
| `win32/CBreakpointsPanel.{h,cpp}` | Breakpoint list + Edit dialog |
| `win32/CMemoryViewer.{h,cpp}`     | Memory hex view (Ctrl+M) |
| `win32/CMemoryRegions.{h,cpp}`    | Per-system list of memory regions for the viewer |
| `win32/debugger_hook.h`           | Inline hooks declared here; included from `cpuexec.cpp`, `getset.h`, `sgb/gb_memory.cpp` |

## Cross-cutting contracts

### Display address

A 24-bit `uint32_t` of the form `((bank & 0xFF) << 16) | (cpu_addr & 0xFFFF)`. Used everywhere — `DisasmLine::pc`, BP storage, `GetCurrentPC`, the cache index lookup. See snes-agent and gb-agent for system-specific semantics of `bank`.

### `g_debugger_attached` and `g_debugger_check_rw`

Two `bool` globals in `win32/CDebugger.cpp` that gate the hot-path hooks.

- `g_debugger_attached` — true when at least one debugger dialog exists. Set by `RecomputeAttached()` on `AttachSnes`/`AttachGb`/`DetachSnes`/`DetachGb`.
- `g_debugger_check_rw` — true when `g_debugger_attached` AND at least one enabled BP has `brk_read || brk_write`. Set by `OnBpsChanged()` (called by every BP CRUD) and by `RecomputeAttached()`.

Both are checked **inline** at the hook call sites:
- `cpuexec.cpp` pre-instruction: `if (g_debugger_attached) S9xDebuggerOnSnesPreInstruction();`
- `getset.h` `S9xGetByte`/`S9xSetByte` entry: `if (g_debugger_check_rw) S9xDebuggerOnSnesMemAccess(...);`
- `sgb/gb_memory.cpp` `MemRead`/`MemWrite` entry: same shape

Cost when no debugger is open: one global load + branch-not-taken per opcode / memory access.

### Halt mechanism

| Reason              | Path                                                  |
|---------------------|-------------------------------------------------------|
| User clicks Break (SNES) | `Pause(DbgSystem::Snes)` → `Settings.Paused=true` + `RefreshSnes/Gb` |
| User clicks Break (GB)   | `Pause(DbgSystem::Gb)` → `gb_break_pending_=true` (no `Settings.Paused` yet) |
| Step Into/Over/Out  | `gb_free_run_=false; gb_step_remaining_=N`; hook calls `HaltGbNow` when N exhausted |
| BP hit (exec)       | Hook calls `HaltSnesNow`/`HaltGbNow` directly         |
| BP hit (R/W)        | `S9xDebuggerOnSnesMemAccess`/`OnGbMemAccess` calls Halt |
| Frame step          | `snes_frame_step_armed_`/`gb_frame_step_armed_`; armed at next VBlank |

`HaltSnesNow` sets `SCAN_KEYS_FLAG` so the SNES opcode loop bails immediately. `HaltGbNow` sets it too (since `01a991a9`), plus `S9xSGBRequestDebuggerBreak` to bail the SGB run loop.

### Refresh pipeline

Each frame (after `S9xMainLoop` returns):
1. `S9xDebuggerRefreshAll()` (`wsnes9x.cpp:4253`) posts `WM_USER_DEBUGGER_REFRESH` to each open dialog and calls `MemoryViewerRefreshAll()`.
2. The dialog's WndProc handles the message → calls `DebuggerDlgRefresh(hwnd)`.
3. `DebuggerDlgRefresh` updates the toolbar Run/Pause enable, the status bar, then fans out to the child panels (`DisasmPanelRefresh`, `StatusPanelRefresh`, `BreakpointsPanelRefresh`).
4. `InvalidateRect(hwnd, NULL, FALSE)` on the dialog itself.

While paused, `S9xMainLoop` doesn't run → no per-frame refresh. The halt paths explicitly call `RefreshSnes`/`RefreshGb` to push state to the UI; the inline `DebuggerDlgRefresh(hwnd)` in the menu/toolbar handler does the same synchronously.

`DisasmPanelRefresh` carries its own state (`shown_pc`, `shown_paused`, `shown_bps_version`, `view_initialized`) and gates two side-effects:
- `EnsurePCVisible(st)` — fires on `first_run || just_paused || (Paused && pc_changed)`
- `InvalidateRect(lv) + UpdateWindow(lv)` — fires on `first_run || Paused || just_unpaused || bps_changed`

`UpdateWindow` forces synchronous WM_PAINT so the marker is visible before the menu handler returns; queued posts don't drop the highlight.

## Disasm cache contract

`CDisasmPanel` maintains a `DisasmLine *lines` array (raw malloc, not `std::vector` — a memset on the panel state used to corrupt vector internals).

Each `DisasmLine` stores `{ pc, length, is_sub_start, is_label_row, section_index }`. `pc` is a display address.

The cache is built **monotonically forward** from `view_start_pc`:
- SNES: `view_start_pc = BankStart(cur_pc) = pc & 0xFF0000`. Re-anchored on bank change.
- GB: `view_start_pc = 0`. Never re-anchored. The cache walks `00:0000 → 00:3FFF → 01:4000 → ...` linearly, including bank crossings via `AdvancePC`.

`FindOrExtendForPC(pc, max_rows)`:
- Scans the existing cache for an exact match on `pc`.
- If not found, extends the cache forward by `EnsureLineCached` until either `last_pc == pc`, `last_pc > pc` (overshoot — means PC is mid-instruction), or `line_count >= max_rows`.
- Returns the row index or -1.

`EnsureLineCached` calls `DisasmOne` to get the opcode length, then `AdvancePC` for the next position. It also captures branch targets for label inference (subroutine starts).

**Cache staleness — the unsolved problem**: the cache is a snapshot of whatever the read path returned at build time. Any of these invalidate ranges of the cache without our knowledge:
- GB boot ROM overlay transition (`boot_rom_enabled` true → false)
- MBC bank rewrites that change cart layout (uncommon — MBC banks are static)
- Self-modifying code (rare)
- SRAM written to (irrelevant to disasm — SRAM isn't in the cache range)

For now, the user can close + reopen the debugger to rebuild. Hooking the `$FF50` write to invalidate `$0000-$00FF` is the obvious next step.

## Hook installation

`AttachSnes(HWND)`:
- Sets `snes_attached_ = true`, `snes_dlg_ = hwnd`, `gSnesDebuggerHWND = hwnd`.
- Calls `RecomputeAttached()` to set `g_debugger_attached` and `g_debugger_check_rw`.
- (No GB hook needed — SNES pre-instruction is unconditional on `g_debugger_attached`.)

`AttachGb(HWND)`:
- Same shape, plus `S9xSGBSetDebuggerHook(&S9xDebuggerOnGbPreInstruction)` — installs the GB CPU's trace hook (`g_trace_hook` in `gb_cpu.cpp`).
- `DetachGb` calls `S9xSGBSetDebuggerHook(nullptr)`.

In SGB BIOS mode the user may have both dialogs open; both attach flags become true and both hooks fire.

## Memory viewer regions

`CMemoryRegions.cpp` defines two tables, `kSnesRegions` and `kGbRegions`. Each entry: `{ name, get_size(), read_byte(off), addr_format_width }`. The viewer's combo populates from the table, the virtual ListView renders 16 bytes per row, ASCII column on the right.

GB regions (current set after `27c2855e`):
| Name              | Backing                          | Notes |
|-------------------|----------------------------------|-------|
| CPU Memory        | `S9xSGBPeekRAByte(off & 0xFFFF)` | Honours boot overlay (`8e203afb`) |
| PRG ROM           | `S9xSGBPeekROMByte(off)`         | Linear cart bytes |
| Work RAM          | WRAM via PeekRA at $C000+off     |       |
| High RAM          | HRAM via PeekRA at $FF80+off     |       |
| Boot ROM          | `S9xSGBPeekBootROMByte(off)`     | Always reads `m.boot_rom[]` regardless of `_enabled` — for inspecting boot after it's unmapped |
| Video RAM         | (returns 0 — PPU bus not exposed) |       |
| Sprite RAM (OAM)  | (returns 0 — same)                |       |
| I/O Registers     | (returns 0 — side-effecting)      |       |
| SRAM              | cart SRAM                          |       |

The CPU Memory and PRG ROM regions are the most useful in practice; the others are convenience views.

## Validation checklist before claiming a fix works

This is **required** for debugger work. The recent boot-overlay bug cost a build cycle because I didn't follow it.

1. **Trace the live path.** Find the actual emulation code that does the same operation (read, write, dispatch, halt) and read every line.
2. **Trace the debugger path.** Find every read/write the debugger does for the same address/range and read every line.
3. **Compare side by side.** They must agree on every conditional that affects the result. If the live path has `if (boot_rom_enabled)` and the debugger path doesn't, that's a divergence to fix or document explicitly.
4. **Check the cache.** The disasm cache is a snapshot. If your fix changes what the read path returns, the existing cache is stale until rebuilt.
5. **Pick a reproducible scenario.** "User opens debugger, presses Break, sees X" — write the title-bar diagnostic dump or note what specific values you expect to observe.
6. **Only then claim the fix works.** Don't write commit messages in the future tense ("this will fix...") — write them after verifying. If the user has to rebuild to test, say so explicitly.

See `feedback_validate_emulation_state.md` in the user's memory for the cause of this rule.
