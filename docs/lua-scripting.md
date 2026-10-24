# Lua scripting

Snes9x runs [Lua 5.4](https://www.lua.org/manual/5.4/) scripts against the game:
they can read and change memory, watch the CPU, press buttons, draw on the
screen, save and load states, and react to what the emulator does. Scripts run
in the Qt, GTK and Windows versions.

- **File > Script > Lua...** opens the script editor: tabs, syntax colouring,
  completion, an API reference and a console for the scripts' output.
- `-lua <script.lua>` on the command line runs a script once the game is loaded
  (repeat it for several scripts). Their output also goes to stdout.

## A first script

```lua
-- Keep the player's health topped up. HEALTH is where the game keeps it.
local HEALTH = 0x06C0
local MAX = 28

while true do
  if memory.wram.read_u8(HEALTH) < MAX then
    memory.wram.write_u8(HEALTH, MAX)
  end
  emu.frame_advance()
end
```

Paste it into the editor, press **Run** (F5) and it runs until you press
**Stop**. *Help > Examples* has more to start from.

## How a script runs

A script's main body runs as a coroutine. `emu.frame_advance()` hands control
back to the emulator, which emulates the next frame, then continues the script
right after it - before the frame is shown, so anything drawn then appears on
that frame. Code that never calls `emu.frame_advance()` (or `emu.yield()`)
holds the emulator up: after `script.set_watchdog` seconds (5 by default) the
script is stopped with a message pointing at the line it was stuck on.

Scripts can also register callbacks (`event.on_frame_end`, `memory.on_write`,
...). A script keeps running while its body is waiting in `emu.frame_advance()`
or while it has callbacks; it finishes when its body returns and no callbacks
are left.

An error stops the script. The console shows the message and a traceback, and
the editor marks the line it happened on; double-click an error in the console
to jump to it. `print`, `io.write` and the `console` functions write to the
console.

Each script has its own Lua state, so several scripts can run at once without
seeing each other's variables. Drawings stack in the order the scripts started.

### Frames and timing

| When | What runs |
| --- | --- |
| Before a frame | `event.on_frame_start` callbacks; button overrides from `joypad.set` are applied |
| During the frame | memory hooks (`memory.on_read`, `on_write`, `on_exec`) |
| After the frame, before it is shown | `event.on_frame_end` callbacks, then script bodies waiting in `emu.frame_advance()` |
| While paused or with no game | `event.on_idle` callbacks and bodies waiting in `emu.yield()`, about 60 times a second |

With run-ahead on, the emulator also runs frames it later rolls back. Scripts
never see those: callbacks, hooks and counters only follow the frames that
count, and the overlay is drawn on the frame that is displayed.

### What memory hooks may do

Hooks run in the middle of a CPU instruction. They can read and write memory,
draw and print, but not load or save states, reset, or call
`emu.frame_advance()`. Read and write hooks can't change registers either;
exec hooks run before the instruction and may (`cpu.set("pc", ...)` skips code).

## Conventions

- Names follow the [LuaRocks style guide](https://github.com/luarocks/lua-style-guide):
  `snake_case` functions in modules.
- Addresses are numbers. On the bus they are 24-bit, `0xBBAAAA`.
- Coordinates are game pixels with `(0, 0)` at the top left of the picture; a
  hi-res or interlaced frame scales them up so scripts don't have to care.
- Colours can be a name (`"red"`, `"white"`, `"none"`, ...), `"#rgb"`,
  `"#rrggbb"`, `"#rrggbbaa"`, a number `0xRRGGBB` or `0xAARRGGBB`, or a table
  `{r, g, b[, a]}` / `{r = , g = , b = , a = }` with 0-255 components. A number
  whose alpha byte is 0 is opaque; use `"none"` or `false` for no colour.
- Functions that return handles (callbacks, hooks) return an object with
  `handle:remove()`, `handle:set_enabled(on)` and `handle:is_active()`.

## Modules

### emu

| Function | Description |
| --- | --- |
| `emu.frame_advance([frames])` | Runs frames (default 1) and returns once the last one is emulated, before it is shown. Main body only. |
| `emu.yield()` | Lets the emulator run until the next frame, or the next idle tick while paused. Main body only. |
| `emu.frame_count()` | Frames emulated since the game was loaded. Savestates don't change it; use `movie.frame()` for a movie's count. |
| `emu.lag_count()` | Frames in which the game didn't read the controllers. |
| `emu.is_lag_frame()` | True if the last frame didn't read the controllers. |
| `emu.pause()`, `emu.unpause()`, `emu.is_paused()` | Pause control. A paused script keeps running through `emu.yield()` and `event.on_idle`. |
| `emu.reset()`, `emu.power_cycle()` | Soft reset and hard reset, as the menu items. |
| `emu.set_turbo(on)`, `emu.is_turbo()` | Fast-forward. |
| `emu.message(text [, seconds])` | Shows text in the on-screen message area. |
| `emu.screenshot()` | Saves a screenshot of the next frame to the screenshot folder. |
| `emu.is_rom_loaded()` | True once a game is loaded. |
| `emu.rom()` | `{title, path, crc32, sha256, size, region, mapping, id, platform}`, or nil without a game. |
| `emu.region()` | `"ntsc"` or `"pal"`. |
| `emu.fps()` | The console's native frame rate. |
| `emu.version()` | The emulator version. |
| `emu.platform()` | `"snes"`, `"gb"` (the Game Boy core) or `"sgb"` (a Game Boy game under the Super Game Boy BIOS). |

### cpu

The SNES 65816. Not available while a Game Boy game runs on its own core.

| Function | Description |
| --- | --- |
| `cpu.get(name)` | A register: `a`, `x`, `y`, `s`/`sp`, `d`/`dp`, `db`/`dbr`, `pb`/`pbr`/`k`, `pc`, `pbpc` (24-bit), `p`, or `e` (boolean). |
| `cpu.set(name, value)` | Sets a register. Setting `pc`, `pb` or `pbpc` moves execution. |
| `cpu.registers()` | `{a, x, y, s, d, db, pb, pc, p, e}`. |
| `cpu.flag(f)`, `cpu.set_flag(f, on)` | Status flags `n v m x d i z c e`. |
| `cpu.scanline()`, `cpu.cycle()` | Where the CPU is in the frame: scanline, and master clock within it. |

### memory

Reads and writes go straight to the memory, without the side effects or bus
timing a CPU access would have, so scripts never disturb the emulation.

| Function | Description |
| --- | --- |
| `memory.read_u8/s8/u16/s16/u24/s24/u32/s32(address [, domain])` | Little-endian values, unsigned or signed. |
| `memory.read_u16_be/s16_be/u24_be/u32_be/s32_be(address [, domain])` | Big-endian values. |
| `memory.write_u8/u16/u24/u32(address, value [, domain])` | Writes little-endian values; `_be` versions write big-endian. |
| `memory.read_bytes(address, count [, domain])` | A table of bytes. |
| `memory.read_string(address, count [, domain])` | The raw bytes as a Lua string. |
| `memory.write_bytes(address, bytes [, domain])` | Writes a table of bytes or a string. |
| `memory.size([domain])`, `memory.domains()` | Domain size; the domains the game has. |
| `memory.domain(name)` | The domain table, e.g. `memory.domain("wram") == memory.wram`. |
| `memory.on_read(where, fn)` | Calls `fn(address, value)` on CPU reads. Return a number to change the value read. |
| `memory.on_write(where, fn)` | Calls `fn(address, value)` on CPU writes. Return a number to change the value written. |
| `memory.on_exec(where, fn)` | Calls `fn(address)` before the CPU runs an instruction there. |

`domain` defaults to `"bus"`. Every domain is also a table with the same
functions minus the argument: `memory.wram.read_u16(0x10)`.

| Domain | SNES | Game Boy |
| --- | --- | --- |
| `bus` | The 24-bit CPU address space. Writes reach RAM and cartridge RAM; ROM and I/O registers aren't written, and I/O reads give the last value written. | The 16-bit GB address space |
| `wram` | Work RAM, 128 KB | Work RAM |
| `sram` | Cartridge battery RAM | Cartridge RAM |
| `rom` | The ROM image; writes patch it in memory | The ROM image (read-only) |
| `vram` | Video RAM, 64 KB | Video RAM (read-only) |
| `oam` | Sprite table, 544 bytes | Sprite table (read-only) |
| `cgram` | Palette, 512 bytes | - |
| `aram` | The sound CPU's 64 KB | - |
| `hram` | - | High RAM |

Hooks take a bus address or a `{first, last}` range; `memory.wram.on_write`
takes WRAM offsets. Mirrors are folded together: a hook on `0x7E0100` also sees
the game write `$00:0100`, and I/O registers match in every system bank. Hooks
watch the main CPU only (not DMA or HDMA transfers), and aren't available for
Game Boy games yet. While a hook is set the CPU runs a hooked copy of its
instruction handlers; with none set it runs the normal ones at full speed.

```lua
-- Who writes to $7E:0100?
memory.wram.on_write(0x0100, function(address, value)
  print(string.format("$%06X = $%02X from $%06X", address, value, cpu.get("pbpc")))
end)
```

### gui

Drawings go on the frame the script draws them on and are cleared at the next
frame start, so a script redraws every frame. `gui.set_auto_clear(false)`
keeps them until `gui.clear()`.

| Function | Description |
| --- | --- |
| `gui.text(x, y, text [, color [, background]])` | Text, white with a black outline by default. |
| `gui.text(x, y, text, style)` | `style` = `{color, background (or bg), outline, font, align, valign, scale}`. Fonts: `"normal"` (proportional), `"fixed"` (5x7) and `"small"` (3x5). `align` is `"left"`, `"center"` or `"right"` of `x`; `valign` is `"top"`, `"middle"` or `"bottom"` of `y`; `scale` is 1-8; `outline = false` drops the outline. |
| `gui.textf(x, y, format, ...)` | `gui.text` with `string.format`. |
| `gui.text_size(text [, font or style])` | Width and height in pixels. |
| `gui.pixel(x, y [, color])` | One pixel. |
| `gui.line(x1, y1, x2, y2 [, color])` | A line. |
| `gui.rect(x, y, width, height [, outline [, fill]])` | A rectangle. Pass `nil` as the outline to only fill. |
| `gui.circle(x, y, radius [, outline [, fill]])` | A circle around `x, y`. |
| `gui.ellipse(x, y, width, height [, outline [, fill]])` | The ellipse inside a box. |
| `gui.polygon(points [, outline [, fill]])` | `points` is `{x1, y1, x2, y2, ...}` or `{{x1, y1}, ...}`. |
| `gui.image(x, y, image or path [, options])` | A PNG. `options` = `{sx, sy, w, h}` (a part of it), `scale`, `opacity` (0-1). |
| `gui.load_image(path)` | Loads a PNG once; returns an image with `:width()` and `:height()`. |
| `gui.clear()`, `gui.set_auto_clear(on)`, `gui.set_opacity(alpha)` | Layer control; opacity (0-1) applies to everything drawn after it. |
| `gui.width()`, `gui.height()` | The picture in game pixels (256 x 224/239; 160 x 144 for Game Boy). |
| `gui.screen()` | `{left, top, width, height, hires, interlace}`. With a widescreen hack, `left` is negative and `width` includes the extra columns. |
| `gui.get_pixel(x, y)` | The game picture's colour there, as `0xRRGGBB`. |
| `gui.rgb(r, g, b [, a])` | A colour number from 0-255 components. |

### joypad

Ports are numbered 1 to 8; the port defaults to 1. Buttons are `a b x y l r
start select up down left right`.

| Function | Description |
| --- | --- |
| `joypad.get([port])` | The buttons the game saw last frame, as `{a = true, ...}`. |
| `joypad.get_physical([port])` | What the player is holding now. |
| `joypad.get_bits([port])` | Last frame's buttons as the 16-bit SNES value. |
| `joypad.set([port,] buttons)` | Overrides buttons for the next frame: `true` holds a button, `false` keeps it released, `nil` leaves it to the player. Call it every frame to keep holding. |
| `joypad.set_bits(port, bits)` | Sets every button for the next frame from a 16-bit value. |
| `joypad.buttons()` | The button names. |

A movie being recorded records the overridden input; a movie playing back wins
over `joypad.set`.

### savestate

| Function | Description |
| --- | --- |
| `savestate.create()` | Captures the state in memory and returns it. |
| `savestate.save(target)` | Saves to a slot number, a file path, or a state object (recapturing it). |
| `savestate.load(target)` | Loads a slot, a file or a state object. |
| `state:save()`, `state:load()`, `state:size()` | The same through the object. |

Paths are relative to the script's folder. Slots are the emulator's own
save-state slots.

### event

| Function | Description |
| --- | --- |
| `event.on_frame_start(fn)` | Before each frame is emulated. |
| `event.on_frame_end(fn)` | After each frame is emulated, before it is shown. |
| `event.on_idle(fn)` | About 60 times a second while paused or without a game. |
| `event.on_reset(fn)` | After a reset; `fn` gets `"soft"` or `"power"`. |
| `event.on_state_saved(fn)`, `event.on_state_loaded(fn)` | After a savestate is written or loaded; `fn` gets the path (`""` for state objects). |
| `event.on_rom_loaded(fn)` | When a game is loaded. |
| `event.on_stop(fn)` | When the script stops, however it stops: a place to save data. |
| `event.remove(handle)`, `event.clear()` | Remove one callback or hook, or all of them. |

### movie

`movie.is_active()`, `movie.is_playing()`, `movie.is_recording()`,
`movie.is_read_only()`, `movie.frame()` and `movie.length()` (nil without a
movie).

### console

`console.log(...)` (as `print`), `console.info(...)`, `console.warn(...)`,
`console.error(...)` (prints in red but doesn't stop the script),
`console.write(...)` (no line break, as `io.write`) and `console.clear()`.

### script

| Function | Description |
| --- | --- |
| `script.name()`, `script.path()`, `script.dir()` | The script's file name, path and folder. An unsaved script has no path. |
| `script.id()` | A number for this run of the script. |
| `script.stop()` | Ends the script cleanly (`os.exit()` does the same). |
| `script.set_watchdog(seconds)` | How long code may run without letting the emulator continue before it is stopped. 0 turns it off. |

## Files and modules

`require`, `dofile`, `loadfile`, `io.open`, `io.lines`, `gui.image` and
`savestate` paths look in the script's folder first, so a script can ship with
its own modules and data. The standard libraries are all there, except that
scripts have no console input (`io.read`) and `os.exit()` ends the script
rather than the emulator. Lua modules written in C can't be loaded, and neither
can precompiled chunks: only Lua source.

## The editor

| Key | Action |
| --- | --- |
| F5 | Run, or restart a running script |
| F6 | Pause or resume the script |
| Shift+F5 | Stop |
| Ctrl+Space | Complete a name (also offered while typing) |
| Ctrl+/ | Comment or uncomment lines |
| Tab, Shift+Tab | Indent, outdent (two spaces) |
| Ctrl+F, F3 | Find, find next |
| Ctrl+G | Go to a line |
| F1 | Show or hide the API reference |

Run uses the text in the editor, saved or not. Closing the editor window
leaves its scripts running; *File > Script > Stop All Scripts* stops them. The
open files come back the next time the editor opens.

## The debugger

The Windows editor can stop a script at a line and step through it. While a
script is stopped the emulator waits with it, in the middle of the frame (or
of the CPU instruction, inside a memory hook), and the sound pauses.

| Key | Action |
| --- | --- |
| F9 | Set or remove a breakpoint on the caret's line (or click the gutter) |
| F5 | Continue a stopped script |
| F8 | Break: stop the running script at its next line |
| F10, F11, Shift+F11 | Step over, step into, step out |
| Shift+F9 | Watch the selection, or the name under the caret |
| Ctrl+Shift+F9 | Remove all breakpoints |

The panel under the editor shows the stopped script:

- **Variables**: the selected frame's locals, its upvalues, and the globals the
  script made (the standard ones are left out). Tables open as you expand them.
- **Watch**: expressions evaluated in the selected frame at every stop, such as
  `player.x + 1` or `memory.wram.read_u8(0x0DBF)`.
- **Call Stack**: click a frame to see its variables and its line.
- **Breakpoints**: every breakpoint, with a box to switch it off. Select one to
  give it a **condition** (stop only when `lives < 3` is true), a **hit count**
  (`5` stops on the 5th hit, `>= 5` from the 5th on, `% 5` every 5th) or a
  **log message**, which prints instead of stopping, `{expressions}` filled in.

Stepping over `emu.frame_advance()` runs the frame and stops on the next line.
*Debug > Break on Errors* stops a script where it fails, with its variables
still there, instead of just ending it. Breakpoints are lines in the script's
own file; code it loads with `require` or `dofile` can be stepped into but not
given breakpoints. Watches and conditions get one second to finish.
