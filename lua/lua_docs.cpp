/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// One line of documentation per API function, for the editors' completion
// and reference panels. docs/lua-scripting.md is the long form.

#include "lua/lua_engine.h"

static const S9xLuaApiDoc	docs[] =
{
	// emu
	{ "emu.frame_advance",	"([frames])",	"Runs frames (default 1) and returns once the last one is emulated, before it is shown. Main body only." },
	{ "emu.yield",			"()",			"Lets the emulator run until the next frame, or the next idle tick while paused. Main body only." },
	{ "emu.frame_count",	"()",			"Frames emulated since the game was loaded." },
	{ "emu.lag_count",		"()",			"Frames in which the game didn't read the controllers." },
	{ "emu.is_lag_frame",	"()",			"True if the last frame didn't read the controllers." },
	{ "emu.pause",			"()",			"Pauses emulation; the script keeps running through emu.yield() and event.on_idle." },
	{ "emu.unpause",		"()",			"Resumes emulation." },
	{ "emu.is_paused",		"()",			"True while emulation is paused." },
	{ "emu.reset",			"()",			"Soft reset, as the console's reset button." },
	{ "emu.power_cycle",	"()",			"Hard reset: power off and on again." },
	{ "emu.set_turbo",		"(on)",			"Turns fast-forward on or off." },
	{ "emu.is_turbo",		"()",			"True while fast-forwarding." },
	{ "emu.message",		"(text [, seconds])",	"Shows text in the on-screen message area." },
	{ "emu.screenshot",		"()",			"Saves a screenshot of the next frame to the screenshot folder." },
	{ "emu.is_rom_loaded",	"()",			"True once a game is loaded." },
	{ "emu.rom",			"()",			"Table about the game: title, path, crc32, sha256, size, region, mapping, id, platform. Nil without a game." },
	{ "emu.region",			"()",			"\"ntsc\" or \"pal\"." },
	{ "emu.fps",			"()",			"The console's native frame rate." },
	{ "emu.clock",			"()",			"Seconds of real time on a steady clock, for frame rates and timing code." },
	{ "emu.version",		"()",			"The emulator version string." },
	{ "emu.platform",		"()",			"\"snes\", \"gb\" (Game Boy core) or \"sgb\" (Super Game Boy BIOS)." },

	// cpu
	{ "cpu.get",			"(register)",	"A 65816 register: a, x, y, s, d, db, pb, pc, pbpc, p, or e (boolean)." },
	{ "cpu.set",			"(register, value)",	"Sets a 65816 register. Not inside read/write hooks." },
	{ "cpu.registers",		"()",			"Table of every register: a, x, y, s, d, db, pb, pc, p, e." },
	{ "cpu.flag",			"(flag)",		"A status flag as a boolean: n, v, m, x, d, i, z, c or e." },
	{ "cpu.set_flag",		"(flag, on)",	"Sets or clears a status flag." },
	{ "cpu.scanline",		"()",			"The scanline the CPU is on." },
	{ "cpu.cycle",			"()",			"The master-clock position within the scanline." },

	// memory
	{ "memory.read_u8",		"(address [, domain])",	"Unsigned byte. Domains: bus (default), wram, sram, rom, vram, oam, cgram, aram; hram for Game Boy." },
	{ "memory.read_s8",		"(address [, domain])",	"Signed byte." },
	{ "memory.read_u16",	"(address [, domain])",	"Unsigned 16-bit little-endian value." },
	{ "memory.read_s16",	"(address [, domain])",	"Signed 16-bit little-endian value." },
	{ "memory.read_u24",	"(address [, domain])",	"Unsigned 24-bit little-endian value." },
	{ "memory.read_s24",	"(address [, domain])",	"Signed 24-bit little-endian value." },
	{ "memory.read_u32",	"(address [, domain])",	"Unsigned 32-bit little-endian value." },
	{ "memory.read_s32",	"(address [, domain])",	"Signed 32-bit little-endian value." },
	{ "memory.read_u16_be",	"(address [, domain])",	"Unsigned 16-bit big-endian value." },
	{ "memory.read_s16_be",	"(address [, domain])",	"Signed 16-bit big-endian value." },
	{ "memory.read_u24_be",	"(address [, domain])",	"Unsigned 24-bit big-endian value." },
	{ "memory.read_u32_be",	"(address [, domain])",	"Unsigned 32-bit big-endian value." },
	{ "memory.read_s32_be",	"(address [, domain])",	"Signed 32-bit big-endian value." },
	{ "memory.write_u8",	"(address, value [, domain])",	"Writes a byte." },
	{ "memory.write_u16",	"(address, value [, domain])",	"Writes a 16-bit little-endian value." },
	{ "memory.write_u24",	"(address, value [, domain])",	"Writes a 24-bit little-endian value." },
	{ "memory.write_u32",	"(address, value [, domain])",	"Writes a 32-bit little-endian value." },
	{ "memory.write_u16_be",	"(address, value [, domain])",	"Writes a 16-bit big-endian value." },
	{ "memory.write_u24_be",	"(address, value [, domain])",	"Writes a 24-bit big-endian value." },
	{ "memory.write_u32_be",	"(address, value [, domain])",	"Writes a 32-bit big-endian value." },
	{ "memory.read_bytes",	"(address, count [, domain])",	"Table of count bytes." },
	{ "memory.read_string",	"(address, count [, domain])",	"count raw bytes as a Lua string." },
	{ "memory.write_bytes",	"(address, bytes [, domain])",	"Writes a table of bytes or a string." },
	{ "memory.size",		"([domain])",	"Size of a domain in bytes." },
	{ "memory.domains",		"()",			"Names of the domains the loaded game has." },
	{ "memory.domain",		"(name)",		"The domain table, e.g. memory.domain(\"wram\") == memory.wram." },
	{ "memory.on_read",		"(address | {first, last}, fn)",	"Calls fn(address, value) on CPU reads; return a number to change what's read. Returns a handle." },
	{ "memory.on_write",	"(address | {first, last}, fn)",	"Calls fn(address, value) on CPU writes; return a number to change what's written. Returns a handle." },
	{ "memory.on_exec",		"(address | {first, last}, fn)",	"Calls fn(address) before the CPU runs an instruction there. Returns a handle." },
	{ "memory.bus",			"",				"The 24-bit CPU address space, read without side effects." },
	{ "memory.wram",		"",				"Work RAM (128 KB): memory.wram.read_u8(0x06C0). Hooks take WRAM offsets too." },
	{ "memory.sram",		"",				"Cartridge battery RAM." },
	{ "memory.rom",			"",				"The game's ROM image; writes patch it in memory." },
	{ "memory.vram",		"",				"Video RAM (64 KB)." },
	{ "memory.oam",			"",				"Sprite attribute memory (544 bytes)." },
	{ "memory.cgram",		"",				"Palette memory (512 bytes)." },
	{ "memory.aram",		"",				"The sound CPU's 64 KB RAM." },
	{ "memory.hram",		"",				"Game Boy high RAM." },

	// gui
	{ "gui.text",			"(x, y, text [, color [, background]] | (x, y, text, style))",	"Draws text. style: {color, background, outline, font = normal|fixed|small, align, valign, scale}." },
	{ "gui.textf",			"(x, y, format, ...)",	"gui.text with string.format." },
	{ "gui.text_size",		"(text [, font | style])",	"Width and height of text in pixels." },
	{ "gui.pixel",			"(x, y [, color])",	"Draws one pixel." },
	{ "gui.line",			"(x1, y1, x2, y2 [, color])",	"Draws a line." },
	{ "gui.rect",			"(x, y, width, height [, outline [, fill]])",	"Draws a rectangle; pass nil as outline to only fill." },
	{ "gui.circle",			"(x, y, radius [, outline [, fill]])",	"Draws a circle centred on x, y." },
	{ "gui.ellipse",		"(x, y, width, height [, outline [, fill]])",	"Draws the ellipse inside a box." },
	{ "gui.polygon",		"(points [, outline [, fill]])",	"points is {x1, y1, x2, y2, ...} or {{x1, y1}, ...}." },
	{ "gui.image",			"(x, y, image | path [, options])",	"Draws a PNG. options: {sx, sy, w, h, scale, opacity}." },
	{ "gui.load_image",		"(path)",		"Loads a PNG once; returns an image with :width() and :height()." },
	{ "gui.clear",			"()",			"Removes this script's drawings now." },
	{ "gui.set_auto_clear",	"(on)",			"False keeps drawings across frames until gui.clear()." },
	{ "gui.set_opacity",	"(alpha)",		"Opacity (0 to 1) for everything drawn after this." },
	{ "gui.set_antialias",	"(on)",			"True smooths the lines, circles, ellipses and polygons drawn after this." },
	{ "gui.width",			"()",			"Width of the picture in game pixels (256, or 160 for Game Boy)." },
	{ "gui.height",			"()",			"Height of the picture in game pixels." },
	{ "gui.screen",			"()",			"Table: left, top, width, height (with widescreen columns), hires, interlace." },
	{ "gui.get_pixel",		"(x, y)",		"The game picture's colour at x, y as 0xRRGGBB." },
	{ "gui.rgb",			"(r, g, b [, a])",	"A colour number from 0-255 components. Colours are also names, \"#rrggbb[aa]\" or {r, g, b, a}." },

	// joypad
	{ "joypad.get",			"([port])",		"Buttons the game saw last frame: {a, b, x, y, l, r, start, select, up, down, left, right}." },
	{ "joypad.get_physical",	"([port])",	"Buttons the player is holding now." },
	{ "joypad.get_bits",	"([port])",		"Last frame's buttons as the SNES 16-bit value." },
	{ "joypad.set",			"([port,] buttons)",	"Overrides buttons for the next frame: true holds, false releases, nil leaves the player's." },
	{ "joypad.set_bits",	"(port, bits)",	"Sets every button for the next frame from a 16-bit value." },
	{ "joypad.buttons",		"()",			"The button names." },

	// savestate
	{ "savestate.create",	"()",			"Captures the emulator's state in memory; load it with savestate.load(state)." },
	{ "savestate.save",		"(slot | path | state)",	"Saves to a numbered slot, a file, or an existing state object." },
	{ "savestate.load",		"(slot | path | state)",	"Loads from a numbered slot, a file, or a state object." },

	// event
	{ "event.on_frame_start",	"(fn)",		"Calls fn() before each frame is emulated. Returns a handle." },
	{ "event.on_frame_end",	"(fn)",			"Calls fn() after each frame is emulated, before it is shown. Returns a handle." },
	{ "event.on_idle",		"(fn)",			"Calls fn() about 60 times a second while emulation is paused." },
	{ "event.on_reset",		"(fn)",			"Calls fn(kind) after a reset; kind is \"soft\" or \"power\"." },
	{ "event.on_state_saved",	"(fn)",		"Calls fn(path) after a savestate is written." },
	{ "event.on_state_loaded",	"(fn)",		"Calls fn(path) after a savestate is loaded (path is \"\" for savestate objects)." },
	{ "event.on_rom_loaded",	"(fn)",		"Calls fn() when a game is loaded." },
	{ "event.on_stop",		"(fn)",			"Calls fn() when the script stops, however it stops." },
	{ "event.remove",		"(handle)",		"Removes a callback or memory hook; same as handle:remove()." },
	{ "event.clear",		"()",			"Removes all of this script's callbacks and hooks." },

	// movie
	{ "movie.is_active",	"()",			"True while a movie is playing or recording." },
	{ "movie.is_playing",	"()",			"True while a movie plays back." },
	{ "movie.is_recording",	"()",			"True while a movie records." },
	{ "movie.is_read_only",	"()",			"True if the movie is read-only." },
	{ "movie.frame",		"()",			"The movie's current frame, or nil." },
	{ "movie.length",		"()",			"The movie's length in frames, or nil." },

	// console
	{ "console.log",		"(...)",		"Prints a line, like print." },
	{ "console.info",		"(...)",		"Prints an information line." },
	{ "console.warn",		"(...)",		"Prints a warning line." },
	{ "console.error",		"(...)",		"Prints an error line without stopping the script." },
	{ "console.write",		"(...)",		"Prints without a line break, like io.write." },
	{ "console.clear",		"()",			"Clears the console." },

	// script
	{ "script.name",		"()",			"The script's file name." },
	{ "script.path",		"()",			"The script's full path (\"\" if unsaved)." },
	{ "script.dir",			"()",			"The script's folder (the emulator's folder for an unsaved script); require, io.open and gui.image look there first." },
	{ "script.id",			"()",			"A number identifying this run of the script." },
	{ "script.stop",		"()",			"Ends the script cleanly." },
	{ "script.set_watchdog",	"(seconds)",	"How long code may run without yielding before it is stopped (default 5, 0 = never)." }
};

const S9xLuaApiDoc *S9xLuaApiDocs (size_t *count)
{
	*count = sizeof(docs) / sizeof(docs[0]);
	return docs;
}

const S9xLuaApiDoc *S9xLuaFindApiDoc (const std::string &name)
{
	for (auto &d : docs)
		if (name == d.name)
			return &d;

	// memory.wram.read_u8 is memory.read_u8 with the domain bound.
	size_t	last = name.rfind('.');
	if (name.compare(0, 7, "memory.") == 0 && last > 7)
		return S9xLuaFindApiDoc("memory" + name.substr(last));
	return nullptr;
}
