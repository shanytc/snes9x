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
