/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Example scripts every port's editor offers under Help > Examples.

#include "lua/lua_engine.h"

static const S9xLuaExample	examples[] =
{
	{ "Frame HUD", "frame_hud.lua", R"lua(-- Frame HUD: frame and lag counters in the corner of the screen.

while true do
  local lag = emu.is_lag_frame() and "LAG" or ""
  gui.textf(2, 2, "Frame %d  Lag %d %s", emu.frame_count(), emu.lag_count(), lag)
  emu.frame_advance()
end
)lua" },

	{ "FPS Counter", "fps_counter.lua", R"lua(-- FPS counter: frames emulated per real second, the speed against the
-- console's own rate, and a graph of how long each frame took (the line
-- is a full-speed frame). Green at full speed, yellow under 95%, red under 80%.

local NATIVE = emu.fps()
local X, Y, W, H = 4, 4, 120, 32
local BUDGET = 1000 / NATIVE        -- milliseconds a full-speed frame takes
local times, last = {}, emu.clock()
local fps, frames, since = NATIVE, 0, last

while true do
  local now = emu.clock()
  table.insert(times, (now - last) * 1000)
  if #times > W then table.remove(times, 1) end
  last, frames = now, frames + 1
  if now - since >= 0.5 then
    fps, frames, since = frames / (now - since), 0, now
  end

  local speed = fps / NATIVE
  local color = speed >= 0.95 and "#60ff60" or speed >= 0.8 and "yellow" or "#ff6060"
  gui.rect(X, Y, W + 4, H + 16, nil, "#000000b0")
  gui.text(X + 2, Y + 2, string.format("%.1f fps  %d%%", fps, math.floor(speed * 100 + 0.5)), { color = color })

  -- Frame times, two frames' worth of height.
  local base = Y + 14 + H
  for i, ms in ipairs(times) do
    local h = math.floor(math.min(ms / (BUDGET * 2), 1) * H)
    gui.line(X + 2 + i - 1, base, X + 2 + i - 1, base - h, ms > BUDGET * 1.5 and "#ff6060" or "#60a0ff")
  end
  local line = base - math.floor(H / 2)
  gui.line(X + 2, line, X + 1 + W, line, "#ffffff60")
  emu.frame_advance()
end
)lua" },

	{ "Input Display", "input_display.lua", R"lua(-- Input display: the buttons player 1's game saw last frame.

local X, Y = 6, 190
local ON, OFF = "#ff4040", "#ffffff50"

local function button(x, y, w, h, pressed)
  gui.rect(X + x, Y + y, w, h, "black", pressed and ON or OFF)
end

while true do
  local pad = joypad.get(1)
  gui.rect(X - 3, Y - 12, 76, 34, nil, "#00000090")
  button(8, 0, 7, 7, pad.up)
  button(8, 14, 7, 7, pad.down)
  button(1, 7, 7, 7, pad.left)
  button(15, 7, 7, 7, pad.right)
  button(26, 10, 9, 5, pad.select)
  button(37, 10, 9, 5, pad.start)
  button(58, 0, 7, 7, pad.x)
  button(51, 7, 7, 7, pad.y)
  button(65, 7, 7, 7, pad.a)
  button(58, 14, 7, 7, pad.b)
  button(0, -9, 14, 5, pad.l)
  button(57, -9, 14, 5, pad.r)
  emu.frame_advance()
end
)lua" },

	{ "Memory Watch", "memory_watch.lua", R"lua(-- Memory watch: WRAM values on screen, flashing yellow when they change.
-- Put the addresses your game uses in the list.

local watches = {
  {label = "Byte $0000", offset = 0x0000, size = 1},
  {label = "Word $0002", offset = 0x0002, size = 2},
  {label = "Byte $0100", offset = 0x0100, size = 1},
}

local last, flash = {}, {}

while true do
  gui.rect(2, 2, 96, #watches * 9 + 3, nil, "#00000090")
  for i, w in ipairs(watches) do
    local value = w.size == 2 and memory.wram.read_u16(w.offset) or memory.wram.read_u8(w.offset)
    if last[i] and last[i] ~= value then flash[i] = 30 end
    last[i] = value
    flash[i] = math.max((flash[i] or 0) - 1, 0)
    local text = string.format("%-10s %0" .. (w.size * 2) .. "X", w.label, value)
    gui.text(4, 4 + (i - 1) * 9, text, flash[i] > 0 and "yellow" or "white")
  end
  emu.frame_advance()
end
)lua" },

	{ "Keep a Value", "keep_value.lua", R"lua(-- Keep a value: stop a WRAM counter (health, lives, timer) from dropping.
-- HEALTH is the counter's WRAM offset in your game, MAX its full value.

local HEALTH = 0x06C0
local MAX = 28

local function keep_max_health()
  if memory.wram.read_u8(HEALTH) < MAX then
    memory.wram.write_u8(HEALTH, MAX)
  end
end

while true do
  keep_max_health()
  emu.frame_advance()
end
)lua" },

	{ "Write Logger", "write_logger.lua", R"lua(-- Write logger: report every CPU write to a WRAM address, with the code
-- that made it. Handy for finding the routine that changes a value.

local OFFSET = 0x0100

memory.wram.on_write(OFFSET, function(address, value)
  print(string.format("frame %d: $%06X = $%02X from $%06X",
    emu.frame_count(), address, value, cpu.get("pbpc")))
end)

-- Nothing else to do: the hook keeps the script alive until you stop it.
)lua" },

	{ "Autofire", "autofire.lua", R"lua(-- Autofire: holding B on player 1 presses and releases it every 4 frames.

local PERIOD = 4

while true do
  if joypad.get_physical(1).b then
    joypad.set(1, {b = emu.frame_count() % PERIOD < PERIOD / 2})
  end
  emu.frame_advance()
end
)lua" },

	{ "Savestate Loop", "savestate_loop.lua", R"lua(-- Savestate loop: replay the next two seconds over and over, as a
-- practice tool. Stop the script to carry on from wherever you are.

local LENGTH = 120
local start = savestate.create()

event.on_stop(function() emu.message("Practice loop off") end)

while true do
  for i = 1, LENGTH do
    gui.textf(4, 4, "Loop %d/%d", i, LENGTH)
    emu.frame_advance()
  end
  savestate.load(start)
end
)lua" },

	{ "Paused Overlay", "paused_overlay.lua", R"lua(-- Paused overlay: scripts keep running while emulation is paused.
-- emu.yield() returns on every frame and, while paused, on idle ticks.

local ticks = 0

while true do
  if emu.is_paused() then
    ticks = ticks + 1
    local blink = (ticks // 20) % 2 == 0
    gui.rect(78, 100, 100, 20, "white", "#000000c0")
    gui.text(128, 106, blink and "PAUSED" or "", {align = "center", color = "yellow"})
  else
    ticks = 0
  end
  emu.yield()
end
)lua" },

	{ "Sprite Boxes", "sprite_boxes.lua", R"lua(-- Sprite boxes: outline every sprite on screen, with its OAM number.
-- Colours show priority: blue 0, green 1, yellow 2, red 3.

-- The {small, large} sprite sizes for each OBSEL setting.
local SIZES = {
  { {8, 8}, {16, 16} }, { {8, 8}, {32, 32} }, { {8, 8}, {64, 64} }, { {16, 16}, {32, 32} },
  { {16, 16}, {64, 64} }, { {32, 32}, {64, 64} }, { {16, 32}, {32, 64} }, { {16, 32}, {32, 32} },
}
local COLORS = { "#4080ffc0", "#40ff40c0", "#ffff40c0", "#ff4040c0" }

while true do
  -- OBSEL ($2101) is write-only: the bus read gives the last value written.
  local sizes = SIZES[(memory.read_u8(0x2101) >> 5) + 1]
  local oam = memory.oam.read_string(0, 544)
  for i = 0, 127 do
    local x, y, _, attr = oam:byte(i * 4 + 1, i * 4 + 4)
    local high = oam:byte(513 + i // 4) >> ((i % 4) * 2)
    if high & 1 ~= 0 then x = x - 256 end
    local size = sizes[high & 2 ~= 0 and 2 or 1]
    local w, h = size[1], size[2]
    if y >= 224 then y = y - 256 end    -- wraps in from the top
    if x > -w and x < 256 and y > -h and y < 224 then
      local color = COLORS[((attr >> 4) & 3) + 1]
      gui.rect(x, y + 1, w, h, color)
      gui.text(x + 1, y + 2, tostring(i), { font = "small", color = color })
    end
  end
  emu.frame_advance()
end
)lua" },

	{ "Palette Viewer", "palette_viewer.lua", R"lua(-- Palette viewer: the 256 colours in CGRAM, live. The top eight rows are
-- the background palettes, the bottom eight the sprite palettes.
-- Hold Select to hide it.

local X, Y, CELL = 168, 8, 5

local function rgb(bgr)
  local r, g, b = bgr & 31, (bgr >> 5) & 31, (bgr >> 10) & 31
  return gui.rgb(r * 255 // 31, g * 255 // 31, b * 255 // 31)
end

while true do
  if not joypad.get_physical(1).select then
    gui.rect(X - 2, Y - 2, 16 * CELL + 4, 16 * CELL + 4, "white", "black")
    for i = 0, 255 do
      gui.rect(X + (i % 16) * CELL, Y + (i // 16) * CELL, CELL, CELL, nil, rgb(memory.cgram.read_u16(i * 2)))
    end
  end
  emu.frame_advance()
end
)lua" },

	{ "RAM Heat Map", "ram_heat_map.lua", R"lua(-- RAM heat map: the first 8 KB of WRAM as a 128x64 grid, one pixel per
-- byte. A byte flashes yellow when it changes and cools to red, then fades:
-- watch which variables the game is busy with.

local X, Y, W, SIZE = 124, 152, 128, 0x2000
local heat = {}
local last = memory.wram.read_string(0, SIZE)

while true do
  local now = memory.wram.read_string(0, SIZE)
  gui.rect(X - 1, Y - 1, W + 2, SIZE // W + 2, "gray", "#000000a0")
  for i = 1, SIZE do
    if now:byte(i) ~= last:byte(i) then
      heat[i] = 64
    end
    local h = heat[i]
    if h then
      gui.pixel(X + (i - 1) % W, Y + (i - 1) // W, gui.rgb(255, h * 4, 0, 60 + h * 3))
      heat[i] = h > 1 and h - 1 or nil
    end
  end
  last = now
  emu.frame_advance()
end
)lua" },

	{ "Input History", "input_history.lua", R"lua(-- Input history: player 1's buttons over the last frames, newest at the
-- bottom, like a piano roll. A row that repeats is counted, not repeated.

local KEYS = { "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r" }
local NAMES = { "B", "Y", "s", "S", "U", "D", "L", "R", "A", "X", "l", "r" }
local ROWS, X, Y = 16, 4, 60
local STYLE = { font = "fixed", color = "white" }
local WIDTH = gui.text_size("9999 " .. string.rep("W", #KEYS), "fixed") + 4
local history = {}

local function row(pad)
  local text = ""
  for i, key in ipairs(KEYS) do
    text = text .. (pad[key] and NAMES[i] or ".")
  end
  return text
end

while true do
  local text = row(joypad.get(1))
  local newest = history[#history]
  if newest and newest.text == text then
    newest.count = newest.count + 1
  else
    table.insert(history, { text = text, count = 1 })
    if #history > ROWS then table.remove(history, 1) end
  end

  gui.rect(X - 2, Y - 2, WIDTH, ROWS * 9 + 3, nil, "#00000090")
  for i, entry in ipairs(history) do
    STYLE.color = i == #history and "yellow" or "white"
    gui.text(X, Y + (i - 1) * 9, string.format("%4d %s", math.min(entry.count, 9999), entry.text), STYLE)
  end
  emu.frame_advance()
end
)lua" },

	{ "Instant Rewind", "instant_rewind.lua", R"lua(-- Instant rewind: hold Select and L to go back in time, up to ten seconds.
-- A snapshot is kept every 10 frames; let go to play on from there.

local EVERY, KEEP = 10, 60
local snapshots = {}

while true do
  local pad = joypad.get_physical(1)
  if pad.select and pad.l and #snapshots > 0 then
    savestate.load(table.remove(snapshots))
    gui.text(128, 8, "<< REWIND", { align = "center", color = "yellow", outline = "black", scale = 2 })
    joypad.set(1, { select = false, l = false })    -- the game doesn't see the rewind keys
  elseif emu.frame_count() % EVERY == 0 then
    table.insert(snapshots, savestate.create())
    if #snapshots > KEEP then table.remove(snapshots, 1) end
  end
  emu.frame_advance()
end
)lua" },

	{ "Cheat Finder", "cheat_finder.lua", R"lua(-- Cheat finder: narrow WRAM down to the byte holding a value such as lives
-- or health. Hold Select (the game won't see it) and press:
--   L      start over: every byte is a candidate
--   Up     keep the bytes that went up since the last check
--   Down   keep the bytes that went down
--   R      keep the bytes that stayed the same
-- Lose a life, press Select+Down, and so on, until only a few are left.

local SIZE = 0x20000
local snapshot, candidates    -- candidates nil: every byte
local previous = {}

local function restart()
  snapshot, candidates = memory.wram.read_string(0, SIZE), nil
  print("Every WRAM byte is a candidate")
end

local function narrow(keep, label)
  local now, kept = memory.wram.read_string(0, SIZE), {}
  local function test(i)
    if keep(snapshot:byte(i), now:byte(i)) then kept[#kept + 1] = i end
  end
  if candidates then
    for _, i in ipairs(candidates) do test(i) end
  else
    for i = 1, SIZE do test(i) end
  end
  snapshot, candidates = now, kept
  print(string.format("%s: %d candidates", label, #kept))
  for k = 1, math.min(#kept, 20) do
    print(string.format("  $%06X = %d", 0x7E0000 + kept[k] - 1, now:byte(kept[k])))
  end
end

restart()
while true do
  local pad = joypad.get_physical(1)
  if pad.select then
    local function pressed(key) return pad[key] and not previous[key] end
    if pressed("l") then restart()
    elseif pressed("up") then narrow(function(a, b) return b > a end, "Went up")
    elseif pressed("down") then narrow(function(a, b) return b < a end, "Went down")
    elseif pressed("r") then narrow(function(a, b) return b == a end, "Stayed the same")
    end
    joypad.set(1, { select = false, l = false, r = false, up = false, down = false })
  end
  previous = pad

  local count = candidates and #candidates or SIZE
  gui.rect(2, 2, 104, 12 + math.min(count, 8) * 9, nil, "#000000a0")
  gui.textf(4, 4, "Candidates: %d", count)
  if count <= 8 then
    for k, i in ipairs(candidates) do
      gui.textf(4, 4 + k * 9, "$%06X = %d", 0x7E0000 + i - 1, memory.wram.read_u8(i - 1))
    end
  end
  emu.frame_advance()
end
)lua" },

	{ "Speedrun Timer", "speedrun_timer.lua", R"lua(-- Speedrun timer: game time counted in frames, so it stays exact when the
-- emulator slows down or fast-forwards. Hold Select and press R to split,
-- Select and L to reset.

local FPS = emu.fps()
local start, splits, previous = emu.frame_count(), {}, {}

local function clock(frames)
  local seconds = frames / FPS
  return string.format("%d:%02d.%02d", seconds // 60, math.floor(seconds % 60), math.floor(seconds * 100) % 100)
end

while true do
  local pad = joypad.get_physical(1)
  if pad.select and pad.r and not previous.r then
    table.insert(splits, emu.frame_count() - start)
  elseif pad.select and pad.l and not previous.l then
    start, splits = emu.frame_count(), {}
  end
  previous = pad

  local shown = math.min(#splits, 8)
  gui.rect(176, 4, 76, 13 + shown * 9, nil, "#000000a0")
  gui.text(249, 6, clock(emu.frame_count() - start), { align = "right", color = "white" })
  for k = #splits - shown + 1, #splits do
    local row = k - (#splits - shown)
    gui.text(249, 8 + row * 9, string.format("%d  %s", k, clock(splits[k] - (splits[k - 1] or 0))),
      { align = "right", color = "#80ff80" })
  end
  emu.frame_advance()
end
)lua" },

	{ "Value Graph", "value_graph.lua", R"lua(-- Value graph: a WRAM value over the last 200 frames. OFFSET is set for
-- Mario's horizontal speed in Super Mario World; use one from your game
-- (SIGNED for values that go below zero).

local OFFSET, SIGNED = 0x007B, true
local X, Y, W, H = 28, 160, 200, 50
local read = SIGNED and memory.wram.read_s8 or memory.wram.read_u8
local samples = {}

while true do
  table.insert(samples, read(OFFSET))
  if #samples > W then table.remove(samples, 1) end

  local lo, hi = math.huge, -math.huge
  for _, v in ipairs(samples) do lo, hi = math.min(lo, v), math.max(hi, v) end
  local span = math.max(hi - lo, 1)

  gui.rect(X - 1, Y - 1, W + 2, H + 2, "gray", "#000000a0")
  local px, py
  for i, v in ipairs(samples) do
    local x, y = X + i - 1, Y + H - 1 - (v - lo) * (H - 1) // span
    if px then gui.line(px, py, x, y, "#40ff80") end
    px, py = x, y
  end
  gui.textf(X, Y - 10, "$%04X = %d   (%d to %d)", OFFSET, samples[#samples], lo, hi)
  emu.frame_advance()
end
)lua" },

	{ "Routine Profiler", "routine_profiler.lua", R"lua(-- Routine profiler: count the instructions the CPU runs for one second,
-- then list the busiest addresses - the game's hottest loops and routines.
-- The exec hook calls Lua for every instruction, so the game crawls while
-- it measures.

local FRAMES = 60
local counts = {}

local hook = memory.on_exec({ 0x000000, 0xFFFFFF }, function(address)
  counts[address] = (counts[address] or 0) + 1
end)
emu.frame_advance(FRAMES)
hook:remove()

local list = {}
for address, n in pairs(counts) do list[#list + 1] = { address = address, n = n } end
table.sort(list, function(a, b) return a.n > b.n end)

print(string.format("The 20 busiest instructions over %d frames:", FRAMES))
for i = 1, math.min(20, #list) do
  print(string.format("  $%06X  %9d", list[i].address, list[i].n))
end
)lua" },

	{ "Drawing Showcase", "drawing_showcase.lua", R"lua(-- Drawing showcase: what the gui module can draw, animated.

local t = 0
while true do
  t = t + 1

  -- A pulsing circle.
  gui.circle(48, 64, 20 + math.floor(math.sin(t / 15) * 8), "white", gui.rgb(255, 64, 64, 128))

  -- A spinning triangle.
  local points = {}
  for k = 0, 2 do
    local angle = t / 30 + k * 2 * math.pi / 3
    points[#points + 1] = 128 + math.floor(math.cos(angle) * 30)
    points[#points + 1] = 64 + math.floor(math.sin(angle) * 30)
  end
  gui.polygon(points, "yellow", "#ffff0040")

  -- A swinging box and a line that follows it.
  local x = 180 + math.floor(math.sin(t / 20) * 50)
  gui.rect(x, 150, 24, 24, "cyan", "#00ffff60")
  gui.line(128, 64, x + 12, 150, "#ffffff80")

  -- Text styles.
  gui.text(128, 104, "SuperSnes9x", { align = "center", color = "white", outline = "blue", scale = 2 })
  gui.text(128, 126, "small font, outlined", { align = "center", font = "small", color = "yellow", outline = "black" })
  gui.textf(4, 210, "frame %d", emu.frame_count())
  emu.frame_advance()
end
)lua" },

	{ "Hex Viewer", "hex_viewer.lua", R"lua(-- Hex viewer: 16 rows of WRAM in hex; bytes flash red when they change.
-- Hold Select (the game won't see it) and press Up/Down to scroll a row,
-- Left/Right a page, L or R to jump between banks $7E and $7F.

local ROWS, X, Y = 16, 2, 40
local SIZE = 0x20000
local CW = gui.text_size("00", "small") - gui.text_size("0", "small")
local PLAIN = { font = "small", color = "white" }
local CHANGED = { font = "small", color = "#ff5050" }
local ADDRESS = { font = "small", color = "#80c0ff" }
local offset, previous, last, flash = 0, {}, {}, {}

while true do
  local pad = joypad.get_physical(1)
  if pad.select then
    local function pressed(key) return pad[key] and not previous[key] end
    if pressed("up") then offset = offset - 16
    elseif pressed("down") then offset = offset + 16
    elseif pressed("left") then offset = offset - ROWS * 16
    elseif pressed("right") then offset = offset + ROWS * 16
    elseif pressed("l") or pressed("r") then offset = offset ~ 0x10000
    end
    offset = math.max(0, math.min(offset, SIZE - ROWS * 16))
    joypad.set(1, { select = false, up = false, down = false, left = false, right = false, l = false, r = false })
  end
  previous = pad

  local bytes = memory.wram.read_string(offset, ROWS * 16)
  gui.rect(X - 1, Y - 1, 56 * CW + 2, ROWS * 7 + 1, nil, "#000000c0")
  for row = 0, ROWS - 1 do
    local base, y = offset + row * 16, Y + row * 7
    gui.text(X, y, string.format("%02X:%04X", 0x7E + (base >> 16), base & 0xFFFF), ADDRESS)
    for col = 0, 15 do
      local address, value = base + col, bytes:byte(row * 16 + col + 1)
      if last[address] and last[address] ~= value then flash[address] = 20 end
      last[address] = value
      local f = flash[address]
      gui.text(X + (8 + col * 3) * CW, y, string.format("%02X", value), f and CHANGED or PLAIN)
      if f then flash[address] = f > 1 and f - 1 or nil end
    end
  end
  emu.frame_advance()
end
)lua" },

	{ "VRAM Tile Viewer", "vram_tiles.lua", R"lua(-- VRAM tile viewer: 256 tiles of VRAM decoded as 4bpp graphics, coloured
-- with one of the 16 palettes (8 to 15 are the sprites'). Hold Select and
-- press Up/Down to page through VRAM, L/R to change the palette. Decoding
-- in Lua takes a while, so it redraws twice a second or when you change it.

local X, Y = 124, 8
local page, palette, previous = 0, 0, {}
local dirty, countdown = true, 0

local function color(index)
  local bgr = memory.cgram.read_u16((palette * 16 + index) * 2)
  return gui.rgb((bgr & 31) * 255 // 31, (bgr >> 5 & 31) * 255 // 31, (bgr >> 10 & 31) * 255 // 31)
end

local function draw()
  gui.clear()
  local colors = {}
  for i = 1, 15 do colors[i] = color(i) end
  gui.rect(X - 2, Y - 2, 132, 142, "white", "black")
  -- A 4bpp tile is 32 bytes: bitplanes 0 and 1 for its 8 rows, then 2 and 3.
  local data = memory.vram.read_string(page * 0x2000, 0x2000)
  for tile = 0, 255 do
    local base, tx, ty = tile * 32, X + (tile % 16) * 8, Y + (tile // 16) * 8
    for row = 0, 7 do
      local p0, p1 = data:byte(base + row * 2 + 1, base + row * 2 + 2)
      local p2, p3 = data:byte(base + row * 2 + 17, base + row * 2 + 18)
      for col = 0, 7 do
        local bit = 7 - col
        local c = (p0 >> bit & 1) | (p1 >> bit & 1) << 1 | (p2 >> bit & 1) << 2 | (p3 >> bit & 1) << 3
        if c ~= 0 then gui.pixel(tx + col, ty + row, colors[c]) end
      end
    end
  end
  gui.textf(X, Y + 130, "word $%04X  palette %d", page * 0x1000, palette)
end

gui.set_auto_clear(false)    -- the drawing stays up between redraws
while true do
  local pad = joypad.get_physical(1)
  if pad.select then
    local function pressed(key) return pad[key] and not previous[key] end
    if pressed("up") then page, dirty = (page - 1) % 8, true
    elseif pressed("down") then page, dirty = (page + 1) % 8, true
    elseif pressed("l") then palette, dirty = (palette - 1) % 16, true
    elseif pressed("r") then palette, dirty = (palette + 1) % 16, true
    end
    joypad.set(1, { select = false, up = false, down = false, l = false, r = false })
  end
  previous = pad

  countdown = countdown - 1
  if dirty or countdown <= 0 then
    draw()
    dirty, countdown = false, 30
  end
  emu.frame_advance()
end
)lua" },

	{ "PPU State", "ppu_state.lua", R"lua(-- PPU state: the background mode, which layers make the main and sub
-- screens, colour math, windows, and where each background's map and tiles
-- sit in VRAM. The registers are write-only on the SNES: these are the
-- values the game last wrote.

local SIZES = { [0] = "32x32", "64x32", "32x64", "64x64" }
local GRAY = { font = "small", color = "gray" }
local WHITE = { font = "small", color = "white" }

local function reg(address) return memory.read_u8(address) end
local function flag(value, bit) return (value >> bit) & 1 == 1 and "X" or "-" end

while true do
  local inidisp, bgmode, mosaic = reg(0x2100), reg(0x2105), reg(0x2106)
  local tm, ts, tmw, tsw = reg(0x212C), reg(0x212D), reg(0x212E), reg(0x212F)
  local cgwsel, cgadsub, setini = reg(0x2130), reg(0x2131), reg(0x2133)
  local tiles = { reg(0x210B) & 15, reg(0x210B) >> 4, reg(0x210C) & 15, reg(0x210C) >> 4 }
  local X, Y = 4, 128

  gui.rect(X - 2, Y - 2, 248, 92, nil, "#000000c0")
  gui.textf(X, Y, "Mode %d%s   brightness %d%s", bgmode & 7, (bgmode >> 3) & 1 == 1 and " (BG3 on top)" or "",
    inidisp & 15, inidisp & 0x80 ~= 0 and "  BLANKED" or "")
  gui.text(X, Y + 11, "       MAIN SUB MATH WIN  16PX MOSAIC  MAP         TILES", GRAY)
  for i = 0, 4 do
    local name = i < 4 and "BG" .. (i + 1) or "OBJ"
    local line = string.format("%-6s  %s    %s    %s    %s/%s  %s    %s", name,
      flag(tm, i), flag(ts, i), flag(cgadsub, i), flag(tmw, i), flag(tsw, i),
      i < 4 and flag(bgmode, 4 + i) or " ", i < 4 and flag(mosaic, i) or " ")
    if i < 4 then
      local sc = reg(0x2107 + i)
      line = line .. string.format("       $%04X %s  $%04X", (sc & 0xFC) << 8, SIZES[sc & 3], tiles[i + 1] << 12)
    end
    gui.text(X, Y + 18 + i * 7, line, WHITE)
  end
  gui.textf(X, Y + 56, "Colour math: %s%s, with the %s; backdrop %s",
    cgadsub & 0x80 ~= 0 and "subtract" or "add", cgadsub & 0x40 ~= 0 and " half" or "",
    cgwsel & 2 ~= 0 and "sub screen" or "fixed colour", flag(cgadsub, 5) == "X" and "too" or "not")
  gui.textf(X, Y + 67, "%s%s%s", setini & 1 ~= 0 and "interlace  " or "", setini & 4 ~= 0 and "239 lines  " or "",
    setini & 8 ~= 0 and "pseudo hi-res" or "")
  emu.frame_advance()
end
)lua" },

	{ "DMA Inspector", "dma_inspector.lua", R"lua(-- DMA inspector: the eight DMA channels as the game last set them up. The
-- HDMA ones, which feed raster effects line by line, are yellow, with the
-- PPU register each channel writes. Handy for working out wavy or gradient
-- effects.

local REGS = {
  [0x00] = "INIDISP", [0x04] = "OAMDATA", [0x05] = "BGMODE", [0x06] = "MOSAIC",
  [0x0D] = "BG1HOFS", [0x0E] = "BG1VOFS", [0x0F] = "BG2HOFS", [0x10] = "BG2VOFS",
  [0x11] = "BG3HOFS", [0x12] = "BG3VOFS", [0x13] = "BG4HOFS", [0x14] = "BG4VOFS",
  [0x18] = "VMDATAL", [0x19] = "VMDATAH", [0x1B] = "M7A", [0x1C] = "M7B", [0x1D] = "M7C",
  [0x1E] = "M7D", [0x1F] = "M7X", [0x20] = "M7Y", [0x21] = "CGADD", [0x22] = "CGDATA",
  [0x26] = "WH0", [0x27] = "WH1", [0x28] = "WH2", [0x29] = "WH3", [0x2C] = "TM",
  [0x2D] = "TS", [0x31] = "CGADSUB", [0x32] = "COLDATA", [0x80] = "WMDATA",
}
local X, Y = 4, 8

while true do
  local hdma = memory.read_u8(0x420C)
  gui.rect(X - 2, Y - 2, 214, 8 * 8 + 12, nil, "#000000c0")
  gui.text(X, Y, "CH KIND   PPU REGISTER     FROM         MODE", { font = "small", color = "gray" })
  for ch = 0, 7 do
    local base = 0x4300 + ch * 16
    local dmap, bbad = memory.read_u8(base), memory.read_u8(base + 1)
    local source = memory.read_u8(base + 4) << 16 | memory.read_u16(base + 2)
    local on = (hdma >> ch) & 1 == 1
    local kind = on and (dmap & 0x40 ~= 0 and "HDMA*" or "HDMA") or "DMA"
    local text = string.format("%d  %-6s $21%02X %-9s %s $%06X  %d", ch, kind, bbad, REGS[bbad] or "",
      dmap & 0x80 ~= 0 and "<-" or "->", source, dmap & 7)
    gui.text(X, Y + 8 + ch * 8, text, { font = "small", color = on and "yellow" or "white" })
  end
  gui.text(X, Y + 74, "HDMA* reads its values through pointers in the table", { font = "small", color = "gray" })
  emu.frame_advance()
end
)lua" },

	{ "Color Picker", "color_picker.lua", R"lua(-- Color picker: aim the crosshair with Select and the D-pad (add B to move
-- faster). Shows the colour under it, its SNES value, and which CGRAM
-- entries hold that colour: the palette slots to edit to recolour it.
-- Colour math and fades change what's on screen, so those may not match.

local x, y = 128, 112
local W, H = gui.width(), gui.height()

local function snes(rgb)    -- 0xRRGGBB to the SNES's 15-bit BGR
  local r, g, b = rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255
  return (r * 31 + 127) // 255 | ((g * 31 + 127) // 255) << 5 | ((b * 31 + 127) // 255) << 10
end

while true do
  local pad = joypad.get_physical(1)
  if pad.select then
    local step = pad.b and 4 or 1
    if pad.left then x = x - step elseif pad.right then x = x + step end
    if pad.up then y = y - step elseif pad.down then y = y + step end
    x, y = math.max(0, math.min(x, W - 1)), math.max(0, math.min(y, H - 1))
    joypad.set(1, { select = false, up = false, down = false, left = false, right = false, b = false })
  end

  local rgb = gui.get_pixel(x, y)
  gui.line(x - 6, y, x - 2, y, "white")
  gui.line(x + 2, y, x + 6, y, "white")
  gui.line(x, y - 6, x, y - 2, "white")
  gui.line(x, y + 2, x, y + 6, "white")

  local bx = x < W / 2 and W - 110 or 4
  gui.rect(bx, 4, 106, 40, "white", "#000000c0")
  if rgb then
    local value = snes(rgb)
    gui.rect(bx + 3, 7, 14, 14, "white", rgb | 0xff000000)
    gui.textf(bx + 21, 7, "#%06X", rgb)
    gui.textf(bx + 21, 16, "SNES $%04X  %d,%d", value, x, y)
    local found = {}
    for i = 0, 255 do
      if memory.cgram.read_u16(i * 2) & 0x7FFF == value then
        found[#found + 1] = string.format("%d:%d", i // 16, i % 16)
        if #found == 4 then break end
      end
    end
    gui.text(bx + 3, 27, #found > 0 and "palette " .. table.concat(found, " ") or "not in CGRAM",
      { font = "small", color = #found > 0 and "yellow" or "gray" })
  end
  emu.frame_advance()
end
)lua" },

	{ "Memory Breakpoint", "memory_breakpoint.lua", R"lua(-- Memory breakpoint: pause the emulator when the game writes to an address
-- (and, if VALUE is set, only when it writes that value), and print the
-- CPU's registers at the write. Unpause and it keeps watching.
-- ADDRESS is set for the lives counter in Super Mario World.

local ADDRESS = 0x7E0DBE
local VALUE = nil    -- a number, such as 0, to stop only on that value

memory.on_write(ADDRESS, function(address, value)
  if VALUE and value ~= VALUE then return end
  local r = cpu.registers()
  print(string.format("frame %d: $%06X <- $%02X  PC:%02X:%04X A:%04X X:%04X Y:%04X S:%04X D:%04X DB:%02X P:%02X",
    emu.frame_count(), address, value, r.pb, r.pc, r.a, r.x, r.y, r.s, r.d, r.db, r.p))
  emu.pause()    -- takes effect at the end of the frame
end)

print(string.format("Watching $%06X for writes%s", ADDRESS, VALUE and string.format(" of $%02X", VALUE) or ""))
)lua" },

	{ "Trace Logger", "trace_logger.lua", R"lua(-- Trace logger: write every instruction the CPU runs over FRAMES frames to
-- trace.txt in the script's folder: address, the bytes there and the
-- registers before it runs. The game crawls while it logs, and a frame
-- comes to a few megabytes.

local FRAMES = 1
local NAME = "trace.txt"
local file = assert(io.open(NAME, "w"))
local count = 0

local hook = memory.on_exec({ 0x000000, 0xFFFFFF }, function(pc)
  local r = cpu.registers()
  file:write(string.format("%02X:%04X  %02X %02X %02X %02X  A:%04X X:%04X Y:%04X S:%04X D:%04X DB:%02X P:%02X%s\n",
    pc >> 16, pc & 0xFFFF, memory.read_u8(pc), memory.read_u8(pc + 1), memory.read_u8(pc + 2),
    memory.read_u8(pc + 3), r.a, r.x, r.y, r.s, r.d, r.db, r.p, r.e and " E" or ""))
  count = count + 1
end)
emu.frame_advance(FRAMES)
hook:remove()
file:close()

print(string.format("%d instructions written to %s%s", count, script.dir(), NAME))
)lua" },

	{ "SMW Info", "smw_info.lua", R"lua(-- Super Mario World: Mario's position, speed and P-meter, and a box on
-- every active sprite with its slot, sprite number and state. Addresses
-- are from the SMW Central RAM map; any game's RAM map works the same way.

local STATES = {
  [1] = "init", [2] = "falling", [3] = "smushed", [4] = "spun", [5] = "burning", [6] = "goal",
  [8] = "normal", [9] = "carryable", [10] = "kicked", [11] = "carried", [12] = "goal coin",
}
local ram = memory.wram

-- A sprite coordinate: its low and high bytes are in two tables of 12 slots.
local function coord(low, high, slot)
  return ram.read_u8(low + slot) | ram.read_u8(high + slot) << 8
end

while true do
  local cam_x, cam_y = ram.read_u16(0x1A), ram.read_u16(0x1C)
  local mario_x, mario_y = ram.read_u16(0x94), ram.read_u16(0x96)
  gui.rect(mario_x - cam_x, mario_y - cam_y + 16, 16, 16, "#40ff40")

  gui.rect(2, 2, 132, 31, nil, "#000000b0")
  gui.textf(4, 4, "Mario  X %d  Y %d", mario_x, mario_y)
  gui.textf(4, 13, "Speed %d, %d   P-meter %d", ram.read_s8(0x7B), ram.read_s8(0x7D), ram.read_u8(0x13E4))
  gui.textf(4, 22, "Lives %d   Coins %d", ram.read_u8(0x0DBE) + 1, ram.read_u8(0x0DBF))

  for slot = 0, 11 do
    local state = ram.read_u8(0x14C8 + slot)
    if state ~= 0 then
      local x = coord(0xE4, 0x14E0, slot) - cam_x
      local y = coord(0xD8, 0x14D4, slot) - cam_y
      if x > -16 and x < 256 and y > -16 and y < 224 then
        gui.rect(x, y, 16, 16, "#ff8040")
        gui.text(x, y - 7, string.format("%d:%02X %s", slot, ram.read_u8(0x9E + slot), STATES[state] or state),
          { font = "small", color = "#ffc080", outline = "black" })
      end
    end
  end
  emu.frame_advance()
end
)lua" }
};

const S9xLuaExample *S9xLuaExamples (size_t *count)
{
	*count = sizeof(examples) / sizeof(examples[0]);
	return examples;
}

const char *S9xLuaNewScriptTemplate (void)
{
	return R"lua(-- Runs once per frame until you press Stop.

while true do
  gui.text(2, 2, "Frame " .. emu.frame_count())
  emu.frame_advance()
end
)lua";
}
