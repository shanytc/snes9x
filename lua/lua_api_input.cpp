/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The joypad and savestate modules.

#include <cstring>

#include "lua/lua_internal.h"
#include "memmap.h"
#include "movie.h"
#include "snapshot.h"
#include "fscompat.h"

namespace
{

const struct
{
	const char	*name;
	uint16		mask;
} buttons[] =
{
	{ "a", SNES_A_MASK }, { "b", SNES_B_MASK }, { "x", SNES_X_MASK }, { "y", SNES_Y_MASK },
	{ "l", SNES_TL_MASK }, { "r", SNES_TR_MASK },
	{ "start", SNES_START_MASK }, { "select", SNES_SELECT_MASK },
	{ "up", SNES_UP_MASK }, { "down", SNES_DOWN_MASK }, { "left", SNES_LEFT_MASK }, { "right", SNES_RIGHT_MASK }
};

const uint16	kAllButtons = 0xfff0;

int CheckPort (lua_State *L, int arg)
{
	lua_Integer	port = luaL_optinteger(L, arg, 1);
	luaL_argcheck(L, port >= 1 && port <= 8, arg, "ports are numbered 1 to 8");
	return (int) port - 1;
}

void PushButtons (lua_State *L, uint16 bits)
{
	lua_createtable(L, 0, 12);
	for (auto &b : buttons)
	{
		lua_pushboolean(L, (bits & b.mask) != 0);
		lua_setfield(L, -2, b.name);
	}
}

int joypad_get (lua_State *L)
{
	PushButtons(L, LuaEng.input[CheckPort(L, 1)]);
	return 1;
}

int joypad_get_physical (lua_State *L)
{
	PushButtons(L, MovieGetJoypad(CheckPort(L, 1)));
	return 1;
}

int joypad_get_bits (lua_State *L)
{
	lua_pushinteger(L, LuaEng.input[CheckPort(L, 1)]);
	return 1;
}

// joypad.set([port,] {a = true, b = false, ...}): true holds a button, false
// keeps it released, nil leaves it to the player. For the next frame only.
int joypad_set (lua_State *L)
{
	int	table = lua_istable(L, 1) ? 1 : 2;
	int	port = table == 1 ? 0 : CheckPort(L, 1);
	luaL_checktype(L, table, LUA_TTABLE);

	LuaScript	*s = LuaScriptOf(L);
	lua_pushnil(L);
	while (lua_next(L, table))
	{
		const char	*name = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
		uint16		mask = 0;
		for (auto &b : buttons)
			if (!strcmp(b.name, name))
				mask = b.mask;
		if (!mask)
			luaL_error(L, "joypad.set: no button named '%s'", name);

		if (lua_isnil(L, -1))
		{
			s->pad_on[port] &= ~mask;
			s->pad_off[port] &= ~mask;
		}
		else if (lua_toboolean(L, -1))
		{
			s->pad_on[port] |= mask;
			s->pad_off[port] &= ~mask;
		}
		else
		{
			s->pad_off[port] |= mask;
			s->pad_on[port] &= ~mask;
		}
		lua_pop(L, 1);
	}
	return 0;
}

int joypad_set_bits (lua_State *L)
{
	int		port = CheckPort(L, 1);
	uint16	bits = (uint16) luaL_checkinteger(L, 2) & kAllButtons;
	LuaScript	*s = LuaScriptOf(L);
	s->pad_on[port] = bits;
	s->pad_off[port] = kAllButtons & ~bits;
	return 0;
}

int joypad_buttons (lua_State *L)
{
	lua_createtable(L, 12, 0);
	int	n = 0;
	for (auto &b : buttons)
	{
		lua_pushstring(L, b.name);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

const luaL_Reg	joypad_functions[] =
{
	{ "get",			joypad_get },
	{ "get_physical",	joypad_get_physical },
	{ "get_bits",		joypad_get_bits },
	{ "set",			joypad_set },
	{ "set_bits",		joypad_set_bits },
	{ "buttons",		joypad_buttons },
	{ nullptr, nullptr }
};

//------------------------------------------------------------------------------
// savestate

const char	kStateType[] = "snes9x.savestate";

struct StateData
{
	std::vector<uint8>	*bytes;
};

StateData *CheckState (lua_State *L, int arg)
{
	return (StateData *) luaL_checkudata(L, arg, kStateType);
}

bool Capture (StateData *state)
{
	// No preview picture: a bot may keep hundreds of these.
	bool8	screenshots = Settings.SnapshotScreenshots;
	Settings.SnapshotScreenshots = FALSE;
	uint32	size = S9xFreezeSize();
	state->bytes->resize(size);
	bool8	ok = S9xFreezeGameMem(state->bytes->data(), size);
	Settings.SnapshotScreenshots = screenshots;
	return ok;
}

std::string SlotPath (lua_State *L, lua_Integer slot)
{
	luaL_argcheck(L, slot >= 0 && slot <= 999, 1, "slots are numbered 0 to 999");
	if (LuaEng.frontend.state_slot_path)
		return LuaEng.frontend.state_slot_path((int) slot);

	char	ext[8];
	snprintf(ext, sizeof(ext), ".%03d", (int) slot);
	return S9xGetFilename(ext, SNAPSHOT_DIR);
}

// A path relative to the script resolves against the script's folder.
std::string ResolvePath (lua_State *L, const char *path)
{
	std::string	p = path;
	const std::string	&dir = LuaScriptOf(L)->dir;
	bool	absolute = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
	return absolute || dir.empty() ? p : dir + p;
}

int savestate_create (lua_State *L)
{
	LuaRequireSafe(L, "savestate.create");
	LuaRequireGame(L);
	auto	*state = (StateData *) lua_newuserdatauv(L, sizeof(StateData), 0);
	state->bytes = new std::vector<uint8>;
	luaL_setmetatable(L, kStateType);
	if (!Capture(state))
		luaL_error(L, "savestate.create: the emulator couldn't save its state");
	return 1;
}

int savestate_save (lua_State *L)
{
	LuaRequireSafe(L, "savestate.save");
	LuaRequireGame(L);

	bool	ok;
	if (lua_isuserdata(L, 1))
		ok = Capture(CheckState(L, 1));
	else if (lua_type(L, 1) == LUA_TNUMBER)
		ok = S9xFreezeGame(SlotPath(L, luaL_checkinteger(L, 1)).c_str());
	else
		ok = S9xFreezeGame(ResolvePath(L, luaL_checkstring(L, 1)).c_str());
	lua_pushboolean(L, ok);
	return 1;
}

int savestate_load (lua_State *L)
{
	LuaRequireSafe(L, "savestate.load");
	LuaRequireGame(L);

	bool	ok;
	if (lua_isuserdata(L, 1))
	{
		StateData	*state = CheckState(L, 1);
		ok = !state->bytes->empty() &&
		     S9xUnfreezeGameMem(state->bytes->data(), (uint32) state->bytes->size()) == SUCCESS;
		if (ok)
			S9xLuaNotifyStateLoaded("");
	}
	else if (lua_type(L, 1) == LUA_TNUMBER)
		ok = S9xUnfreezeGame(SlotPath(L, luaL_checkinteger(L, 1)).c_str());
	else
		ok = S9xUnfreezeGame(ResolvePath(L, luaL_checkstring(L, 1)).c_str());
	S9xLuaSelectOpcodes();	// a load puts the normal opcode tables back
	lua_pushboolean(L, ok);
	return 1;
}

int state_size (lua_State *L)
{
	lua_pushinteger(L, (lua_Integer) CheckState(L, 1)->bytes->size());
	return 1;
}

int state_gc (lua_State *L)
{
	StateData	*state = CheckState(L, 1);
	delete state->bytes;
	state->bytes = nullptr;
	return 0;
}

int state_tostring (lua_State *L)
{
	lua_pushfstring(L, "savestate (%d bytes)", (int) CheckState(L, 1)->bytes->size());
	return 1;
}

const luaL_Reg	savestate_functions[] =
{
	{ "create",	savestate_create },
	{ "save",	savestate_save },
	{ "load",	savestate_load },
	{ nullptr, nullptr }
};

}	// namespace

void LuaApplyInput (void)
{
	uint16	on[8] = {}, off[8] = {};
	for (auto &s : LuaEng.scripts)
	{
		if (s->paused || s->ending)
			continue;
		for (int p = 0; p < 8; p++)
		{
			// A later script wins a button two scripts disagree on.
			on[p] = (on[p] & ~s->pad_off[p]) | s->pad_on[p];
			off[p] = (off[p] & ~s->pad_on[p]) | s->pad_off[p];
			s->pad_on[p] = s->pad_off[p] = 0;
		}
	}

	for (int p = 0; p < 8; p++)
	{
		uint16	mask = on[p] | off[p];
		LuaEng.forced_mask[p] = mask;
		if (!mask)
			continue;
		uint16	live = MovieGetJoypad(p);
		LuaEng.saved_bits[p] = live & mask;
		MovieSetJoypad(p, (live & ~mask) | on[p]);
	}
}

void LuaRestoreInput (void)
{
	for (int p = 0; p < 8; p++)
	{
		uint16	mask = LuaEng.forced_mask[p];
		if (!mask)
			continue;
		MovieSetJoypad(p, (MovieGetJoypad(p) & ~mask) | LuaEng.saved_bits[p]);
		LuaEng.forced_mask[p] = 0;
	}
}

void LuaOpenInput (lua_State *L)
{
	luaL_newlib(L, joypad_functions);
	lua_setglobal(L, "joypad");

	static const luaL_Reg	state_methods[] =
	{
		{ "size", state_size },
		{ nullptr, nullptr }
	};
	luaL_newmetatable(L, kStateType);
	luaL_newlib(L, state_methods);
	lua_pushcfunction(L, savestate_save);
	lua_setfield(L, -2, "save");
	lua_pushcfunction(L, savestate_load);
	lua_setfield(L, -2, "load");
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, state_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, state_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	luaL_newlib(L, savestate_functions);
	lua_setglobal(L, "savestate");
}
