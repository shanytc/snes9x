/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The emu, cpu, event, script, console and movie modules.

#include <algorithm>
#include <cstring>

#include "lua/lua_internal.h"
#include "memmap.h"
#include "cpuexec.h"
#include "gfx.h"
#include "movie.h"
#include "display.h"
#include "sgb/sgb.h"

namespace
{

const char	kHandleType[] = "snes9x.handle";

struct HandleData
{
	int	handle;
};

LuaCallback *FindCallback (LuaScript *s, int handle)
{
	for (auto &cb : s->callbacks)
		if (cb.handle == handle && !cb.removed)
			return &cb;
	return nullptr;
}

bool IsHookKind (int kind)
{
	return kind == LUA_CB_READ || kind == LUA_CB_WRITE || kind == LUA_CB_EXEC;
}

void RemoveCallback (lua_State *L, LuaCallback *cb)
{
	cb->removed = true;
	luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
	cb->ref = LUA_NOREF;
	if (IsHookKind(cb->kind))
		LuaRebuildHooks();
}

int handle_remove (lua_State *L)
{
	auto	*h = (HandleData *) luaL_checkudata(L, 1, kHandleType);
	if (LuaCallback *cb = FindCallback(LuaScriptOf(L), h->handle))
		RemoveCallback(L, cb);
	return 0;
}

int handle_set_enabled (lua_State *L)
{
	auto	*h = (HandleData *) luaL_checkudata(L, 1, kHandleType);
	bool	enabled = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
	if (LuaCallback *cb = FindCallback(LuaScriptOf(L), h->handle))
	{
		cb->enabled = enabled;
		if (IsHookKind(cb->kind))
			LuaRebuildHooks();
	}
	return 0;
}

int handle_is_active (lua_State *L)
{
	auto	*h = (HandleData *) luaL_checkudata(L, 1, kHandleType);
	LuaCallback	*cb = FindCallback(LuaScriptOf(L), h->handle);
	lua_pushboolean(L, cb && cb->enabled);
	return 1;
}

int handle_tostring (lua_State *L)
{
	auto	*h = (HandleData *) luaL_checkudata(L, 1, kHandleType);
	lua_pushfstring(L, "handle #%d", h->handle);
	return 1;
}

//------------------------------------------------------------------------------
// event

template <int kind>
int event_on (lua_State *L)
{
	return LuaAddCallback(L, kind, 1);
}

int event_remove (lua_State *L)
{
	if (lua_isnoneornil(L, 1))
		return 0;
	return handle_remove(L);
}

int event_clear (lua_State *L)
{
	for (auto &cb : LuaScriptOf(L)->callbacks)
		if (!cb.removed)
			RemoveCallback(L, &cb);
	return 0;
}

const luaL_Reg	event_functions[] =
{
	{ "on_frame_start",		event_on<LUA_CB_FRAME_START> },
	{ "on_frame_end",		event_on<LUA_CB_FRAME_END> },
	{ "on_idle",			event_on<LUA_CB_IDLE> },
	{ "on_reset",			event_on<LUA_CB_RESET> },
	{ "on_state_saved",		event_on<LUA_CB_STATE_SAVED> },
	{ "on_state_loaded",	event_on<LUA_CB_STATE_LOADED> },
	{ "on_rom_loaded",		event_on<LUA_CB_ROM_LOADED> },
	{ "on_stop",			event_on<LUA_CB_STOP> },
	{ "remove",				event_remove },
	{ "clear",				event_clear },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// emu

LuaScript *RequireMain (lua_State *L, const char *what)
{
	LuaScript	*s = LuaScriptOf(L);
	if (L != s->main || !lua_isyieldable(L))
		luaL_error(L, "%s only works in the script's main body, not in a callback or coroutine", what);
	return s;
}

int emu_frame_advance (lua_State *L)
{
	LuaScript	*s = RequireMain(L, "emu.frame_advance");
	lua_Integer	frames = luaL_optinteger(L, 1, 1);
	if (frames < 1)
		return 0;
	s->wait = LuaScript::WAIT_FRAMES;
	s->wait_frames = (int) std::min<lua_Integer>(frames, 0x7fffffff);
	return lua_yield(L, 0);
}

int emu_yield (lua_State *L)
{
	LuaScript	*s = RequireMain(L, "emu.yield");
	s->wait = LuaScript::WAIT_TICK;
	return lua_yield(L, 0);
}

int emu_frame_count (lua_State *L)
{
	lua_pushinteger(L, LuaEng.frame_count);
	return 1;
}

int emu_lag_count (lua_State *L)
{
	lua_pushinteger(L, LuaEng.lag_count);
	return 1;
}

int emu_is_lag_frame (lua_State *L)
{
	lua_pushboolean(L, LuaEng.lagged);
	return 1;
}

int emu_pause (lua_State *L)
{
	if (LuaEng.frontend.set_paused)
		LuaEng.frontend.set_paused(true);
	return 0;
}

int emu_unpause (lua_State *L)
{
	if (LuaEng.frontend.set_paused)
		LuaEng.frontend.set_paused(false);
	return 0;
}

int emu_is_paused (lua_State *L)
{
	lua_pushboolean(L, Settings.Paused);
	return 1;
}

void Reset (lua_State *L, bool power)
{
	LuaRequireSafe(L, power ? "emu.power_cycle" : "emu.reset");
	LuaRequireGame(L);
	if (LuaEng.frontend.reset)
		LuaEng.frontend.reset(power);
	else
	{
		S9xMovieUpdateOnReset();
		if (power)
			S9xReset();
		else
			S9xSoftReset();
		S9xLuaNotifyReset(power);
	}
}

int emu_reset (lua_State *L)
{
	Reset(L, false);
	return 0;
}

int emu_power_cycle (lua_State *L)
{
	Reset(L, true);
	return 0;
}

int emu_set_turbo (lua_State *L)
{
	Settings.TurboMode = lua_toboolean(L, 1);
	return 0;
}

int emu_is_turbo (lua_State *L)
{
	lua_pushboolean(L, Settings.TurboMode);
	return 1;
}

int emu_message (lua_State *L)
{
	lua_Number	seconds = luaL_optnumber(L, 2, 0);
	const char	*text = luaL_tolstring(L, 1, nullptr);
	S9xSetInfoString(text);
	if (seconds > 0)
		GFX.InfoStringTimeout = (uint32) (seconds * (Memory.ROMFramesPerSecond ? Memory.ROMFramesPerSecond : 60));
	return 0;
}

int emu_screenshot (lua_State *L)
{
	LuaRequireGame(L);
	Settings.TakeScreenshot = TRUE;
	return 0;
}

int emu_is_rom_loaded (lua_State *L)
{
	lua_pushboolean(L, LuaGameLoaded());
	return 1;
}

const char *Platform (void)
{
	if (Settings.SuperGameBoy)
		return "gb";
	if (Settings.SGB_BIOSModeActive)
		return "sgb";
	return "snes";
}

int emu_rom (lua_State *L)
{
	if (!LuaGameLoaded())
		return 0;

	std::string	title = Memory.ROMName;
	if (Settings.SuperGameBoy || Settings.SGB_BIOSModeActive)
	{
		// A Game Boy cart's title sits in its own header at $0134-$0143.
		unsigned int		size = 0;
		const unsigned char	*rom = S9xSGBCheatROMPtr(&size);
		title.clear();
		for (unsigned int i = 0x134; rom && i < 0x144 && i < size && rom[i] >= 0x20 && rom[i] < 0x7f; i++)
			title += (char) rom[i];
	}
	while (!title.empty() && (title.back() == ' ' || title.back() == '\0'))
		title.pop_back();

	char	hash[65];
	for (int i = 0; i < 32; i++)
		snprintf(hash + i * 2, 3, "%02x", Memory.ROMSHA256[i]);

	lua_createtable(L, 0, 9);
	lua_pushstring(L, title.c_str());
	lua_setfield(L, -2, "title");
	lua_pushstring(L, Memory.ROMFilename.c_str());
	lua_setfield(L, -2, "path");
	lua_pushinteger(L, Memory.ROMCRC32);
	lua_setfield(L, -2, "crc32");
	lua_pushstring(L, hash);
	lua_setfield(L, -2, "sha256");
	lua_pushinteger(L, Memory.CalculatedSize);
	lua_setfield(L, -2, "size");
	lua_pushstring(L, Settings.PAL ? "pal" : "ntsc");
	lua_setfield(L, -2, "region");
	lua_pushstring(L, Memory.HiROM ? "hirom" : "lorom");
	lua_setfield(L, -2, "mapping");
	lua_pushstring(L, Memory.ROMId);
	lua_setfield(L, -2, "id");
	lua_pushstring(L, Platform());
	lua_setfield(L, -2, "platform");
	return 1;
}

int emu_region (lua_State *L)
{
	lua_pushstring(L, Settings.PAL ? "pal" : "ntsc");
	return 1;
}

int emu_fps (lua_State *L)
{
	if (Settings.SuperGameBoy)
		lua_pushnumber(L, 4194304.0 / 70224.0);
	else
		lua_pushnumber(L, Settings.PAL ? 21281370.0 / (1364.0 * 312.0) : 21477272.0 / (1364.0 * 262.0 - 2.0));
	return 1;
}

int emu_version (lua_State *L)
{
	lua_pushstring(L, VERSION_DISPLAY);
	return 1;
}

int emu_platform (lua_State *L)
{
	lua_pushstring(L, Platform());
	return 1;
}

const luaL_Reg	emu_functions[] =
{
	{ "frame_advance",	emu_frame_advance },
	{ "yield",			emu_yield },
	{ "frame_count",	emu_frame_count },
	{ "lag_count",		emu_lag_count },
	{ "is_lag_frame",	emu_is_lag_frame },
	{ "pause",			emu_pause },
	{ "unpause",		emu_unpause },
	{ "is_paused",		emu_is_paused },
	{ "reset",			emu_reset },
	{ "power_cycle",	emu_power_cycle },
	{ "set_turbo",		emu_set_turbo },
	{ "is_turbo",		emu_is_turbo },
	{ "message",		emu_message },
	{ "screenshot",		emu_screenshot },
	{ "is_rom_loaded",	emu_is_rom_loaded },
	{ "rom",			emu_rom },
	{ "region",			emu_region },
	{ "fps",			emu_fps },
	{ "version",		emu_version },
	{ "platform",		emu_platform },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// cpu: the 65816. Mid-loop the N/V/Z/C flags live unpacked in ICPU.

void RequireSnesCpu (lua_State *L)
{
	LuaRequireGame(L);
	if (Settings.SuperGameBoy)
		luaL_error(L, "cpu is the SNES 65816, which a Game Boy game doesn't run");
}

void SetFlagsWord (uint16 w)
{
	Registers.P.W = w & 0x1ff;
	if (CheckEmulation())
	{
		SetFlags(MemoryFlag | IndexFlag);
		Registers.SH = 1;
	}
	if (CheckIndex())
	{
		Registers.XH = 0;
		Registers.YH = 0;
	}
	S9xUnpackStatus();
	S9xFixCycles();
}

bool GetRegister (lua_State *L, const char *name)
{
	S9xPackStatus();
	struct { const char *name; lua_Integer value; } table[] =
	{
		{ "a", Registers.A.W }, { "x", Registers.X.W }, { "y", Registers.Y.W },
		{ "s", Registers.S.W }, { "sp", Registers.S.W },
		{ "d", Registers.D.W }, { "dp", Registers.D.W },
		{ "db", Registers.DB }, { "dbr", Registers.DB },
		{ "pb", Registers.PB }, { "pbr", Registers.PB }, { "k", Registers.PB },
		{ "pc", Registers.PCw }, { "pbpc", Registers.PBPC & 0xffffff },
		{ "p", Registers.PL }
	};
	for (auto &r : table)
	{
		if (!strcmp(r.name, name))
		{
			lua_pushinteger(L, r.value);
			return true;
		}
	}
	if (!strcmp(name, "e"))
	{
		lua_pushboolean(L, CheckEmulation());
		return true;
	}
	return false;
}

int cpu_get (lua_State *L)
{
	RequireSnesCpu(L);
	const char	*name = luaL_checkstring(L, 1);
	if (!GetRegister(L, name))
		return luaL_argerror(L, 1, lua_pushfstring(L, "no register named '%s'", name));
	return 1;
}

int cpu_set (lua_State *L)
{
	RequireSnesCpu(L);
	if (LuaEng.context == LUA_CTX_HOOK)
		luaL_error(L, "cpu.set can't be used inside a read or write hook");

	std::string	name = luaL_checkstring(L, 1);
	if (name == "e")
	{
		S9xPackStatus();
		uint16	w = Registers.P.W & ~Emulation;
		SetFlagsWord(lua_toboolean(L, 2) ? (w | Emulation) : w);
		return 0;
	}

	uint32	v = (uint32) luaL_checkinteger(L, 2);
	if (name == "a")
		Registers.A.W = v;
	else if (name == "x")
		Registers.X.W = CheckIndex() ? (v & 0xff) : v;
	else if (name == "y")
		Registers.Y.W = CheckIndex() ? (v & 0xff) : v;
	else if (name == "s" || name == "sp")
		Registers.S.W = CheckEmulation() ? (0x100 | (v & 0xff)) : v;
	else if (name == "d" || name == "dp")
		Registers.D.W = v;
	else if (name == "db" || name == "dbr")
	{
		Registers.DB = v;
		ICPU.ShiftedDB = (v & 0xff) << 16;
	}
	else if (name == "pb" || name == "pbr" || name == "k" || name == "pc" || name == "pbpc")
	{
		if (name == "pc")
			Registers.PCw = v;
		else if (name == "pbpc")
			Registers.PBPC = v & 0xffffff;
		else
			Registers.PB = v;
		ICPU.ShiftedPB = Registers.PB << 16;
		S9xSetPCBase(Registers.PBPC);
	}
	else if (name == "p")
		SetFlagsWord((Registers.P.W & Emulation) | (v & 0xff));
	else
		return luaL_argerror(L, 1, lua_pushfstring(L, "no register named '%s'", name.c_str()));
	return 0;
}

int cpu_registers (lua_State *L)
{
	RequireSnesCpu(L);
	static const char	*names[] = { "a", "x", "y", "s", "d", "db", "pb", "pc", "p", "e" };
	lua_createtable(L, 0, 10);
	for (const char *name : names)
	{
		GetRegister(L, name);
		lua_setfield(L, -2, name);
	}
	return 1;
}

uint16 FlagBit (lua_State *L, int arg)
{
	const char	*name = luaL_checkstring(L, arg);
	static const struct { char name; uint16 bit; } flags[] =
	{
		{ 'n', Negative }, { 'v', Overflow }, { 'm', MemoryFlag }, { 'x', IndexFlag },
		{ 'd', Decimal }, { 'i', IRQ }, { 'z', Zero }, { 'c', Carry }, { 'e', Emulation }
	};
	if (name[0] && !name[1])
		for (auto &f : flags)
			if (f.name == name[0])
				return f.bit;
	luaL_argerror(L, arg, "expected one of n v m x d i z c e");
	return 0;
}

int cpu_flag (lua_State *L)
{
	RequireSnesCpu(L);
	uint16	bit = FlagBit(L, 1);
	S9xPackStatus();
	lua_pushboolean(L, (Registers.P.W & bit) != 0);
	return 1;
}

int cpu_set_flag (lua_State *L)
{
	RequireSnesCpu(L);
	if (LuaEng.context == LUA_CTX_HOOK)
		luaL_error(L, "cpu.set_flag can't be used inside a read or write hook");
	uint16	bit = FlagBit(L, 1);
	S9xPackStatus();
	SetFlagsWord(lua_toboolean(L, 2) ? (Registers.P.W | bit) : (Registers.P.W & ~bit));
	return 0;
}

int cpu_scanline (lua_State *L)
{
	lua_pushinteger(L, CPU.V_Counter);
	return 1;
}

int cpu_cycle (lua_State *L)
{
	lua_pushinteger(L, CPU.Cycles);
	return 1;
}

const luaL_Reg	cpu_functions[] =
{
	{ "get",		cpu_get },
	{ "set",		cpu_set },
	{ "registers",	cpu_registers },
	{ "flag",		cpu_flag },
	{ "set_flag",	cpu_set_flag },
	{ "scanline",	cpu_scanline },
	{ "cycle",		cpu_cycle },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// script

int script_name (lua_State *L)
{
	lua_pushstring(L, LuaScriptOf(L)->name.c_str());
	return 1;
}

int script_path (lua_State *L)
{
	lua_pushstring(L, LuaScriptOf(L)->path.c_str());
	return 1;
}

int script_dir (lua_State *L)
{
	lua_pushstring(L, LuaScriptOf(L)->dir.c_str());
	return 1;
}

int script_id (lua_State *L)
{
	lua_pushinteger(L, LuaScriptOf(L)->id);
	return 1;
}

int script_stop (lua_State *L)
{
	return LuaRaiseStop(L);
}

int script_set_watchdog (lua_State *L)
{
	lua_Number	seconds = luaL_checknumber(L, 1);
	LuaScriptOf(L)->watchdog = seconds > 0 ? seconds : 0;
	return 0;
}

const luaL_Reg	script_functions[] =
{
	{ "name",			script_name },
	{ "path",			script_path },
	{ "dir",			script_dir },
	{ "id",				script_id },
	{ "stop",			script_stop },
	{ "set_watchdog",	script_set_watchdog },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// console

std::string JoinArgs (lua_State *L)
{
	std::string	line;
	int			n = lua_gettop(L);
	for (int i = 1; i <= n; i++)
	{
		size_t		length;
		const char	*text = luaL_tolstring(L, i, &length);
		if (i > 1)
			line += '\t';
		line.append(text, length);
		lua_pop(L, 1);
	}
	return line;
}

template <S9xLuaEventKind kind>
int console_line (lua_State *L)
{
	LuaFlushOutput(LuaScriptOf(L));
	LuaEmit(LuaScriptOf(L)->id, kind, JoinArgs(L));
	return 0;
}

int console_write (lua_State *L)
{
	std::string	text;
	for (int i = 1, n = lua_gettop(L); i <= n; i++)
	{
		size_t		length;
		const char	*part = luaL_tolstring(L, i, &length);
		text.append(part, length);
		lua_pop(L, 1);
	}
	LuaWrite(LuaScriptOf(L), text);
	return 0;
}

int console_clear (lua_State *L)
{
	LuaEmit(LuaScriptOf(L)->id, S9X_LUA_EVENT_CLEAR, "");
	return 0;
}

const luaL_Reg	console_functions[] =
{
	{ "log",	console_line<S9X_LUA_EVENT_PRINT> },
	{ "info",	console_line<S9X_LUA_EVENT_INFO> },
	{ "warn",	console_line<S9X_LUA_EVENT_WARNING> },
	{ "error",	console_line<S9X_LUA_EVENT_ERROR> },
	{ "write",	console_write },
	{ "clear",	console_clear },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// movie

int movie_is_active (lua_State *L)
{
	lua_pushboolean(L, S9xMovieActive());
	return 1;
}

int movie_is_playing (lua_State *L)
{
	lua_pushboolean(L, S9xMoviePlaying());
	return 1;
}

int movie_is_recording (lua_State *L)
{
	lua_pushboolean(L, S9xMovieRecording());
	return 1;
}

int movie_is_read_only (lua_State *L)
{
	lua_pushboolean(L, S9xMovieActive() && S9xMovieReadOnly());
	return 1;
}

int movie_frame (lua_State *L)
{
	if (!S9xMovieActive())
		return 0;
	lua_pushinteger(L, S9xMovieGetFrameCounter());
	return 1;
}

int movie_length (lua_State *L)
{
	if (!S9xMovieActive())
		return 0;
	lua_pushinteger(L, S9xMovieGetLength());
	return 1;
}

const luaL_Reg	movie_functions[] =
{
	{ "is_active",		movie_is_active },
	{ "is_playing",		movie_is_playing },
	{ "is_recording",	movie_is_recording },
	{ "is_read_only",	movie_is_read_only },
	{ "frame",			movie_frame },
	{ "length",			movie_length },
	{ nullptr, nullptr }
};

void NewModule (lua_State *L, const char *name, const luaL_Reg *functions)
{
	lua_newtable(L);
	luaL_setfuncs(L, functions, 0);
	lua_setglobal(L, name);
}

}	// namespace

void LuaPushHandle (lua_State *L, int handle)
{
	auto	*h = (HandleData *) lua_newuserdatauv(L, sizeof(HandleData), 0);
	h->handle = handle;
	luaL_setmetatable(L, kHandleType);
}

void LuaOpenEmu (lua_State *L)
{
	static const luaL_Reg	handle_methods[] =
	{
		{ "remove",			handle_remove },
		{ "set_enabled",	handle_set_enabled },
		{ "is_active",		handle_is_active },
		{ nullptr, nullptr }
	};

	luaL_newmetatable(L, kHandleType);
	luaL_newlib(L, handle_methods);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, handle_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	NewModule(L, "emu", emu_functions);
	NewModule(L, "cpu", cpu_functions);
	NewModule(L, "event", event_functions);
	NewModule(L, "script", script_functions);
	NewModule(L, "console", console_functions);
	NewModule(L, "movie", movie_functions);
}
