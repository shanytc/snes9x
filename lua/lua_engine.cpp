/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "lua/lua_internal.h"
#include "memmap.h"
#include "cpuexec.h"
#include "gfx.h"
#include "display.h"
#include "movie.h"

uint8		S9xLuaActive = 0;
uint8		S9xLuaHookMask = 0;
LuaEngine	LuaEng;

extern bool8	pad_read_last;

namespace
{

const int	kHookInterval = 10000;	// VM instructions between interrupt checks
const size_t	kMaxEvents = 20000;
char		kStopSentinel;			// its address marks a clean stop

std::atomic<int>	next_script_id { 1 };
std::atomic<bool>	game_loaded { false };

std::mutex	event_mutex;
std::deque<S9xLuaEvent>	pending_events;
bool		echo_output = false;

std::mutex	info_mutex;
std::vector<S9xLuaScriptInfo>	script_info;

std::mutex	stop_mutex;
std::set<int>	stop_requests;
std::atomic<bool>	any_stop_request { false };

struct Notice
{
	int		kind;
	std::string	text;
	bool	flag;
};
std::mutex	notice_mutex;
std::vector<Notice>	notices;

struct HookRef
{
	LuaScript	*script;
	size_t		index;
};
std::vector<HookRef>	hook_refs[3];
uint8		hook_pages[3][0x10000 / 8];	// one bit per 256-byte page of the bus
int			dispatch_depth = 0;
bool		hooks_dirty = false;

std::chrono::steady_clock::time_point	last_idle;

int HookSlot (int kind)
{
	return kind == LUA_CB_READ ? 0 : kind == LUA_CB_WRITE ? 1 : 2;
}

std::string DirectoryOf (const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string BaseName (const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

void UpdateInfo (void)
{
	std::vector<S9xLuaScriptInfo>	list;
	for (auto &s : LuaEng.scripts)
		list.push_back({ s->id, s->name, s->path, s->paused ? S9X_LUA_PAUSED : S9X_LUA_RUNNING });

	std::lock_guard<std::mutex>	lock(info_mutex);
	script_info = std::move(list);
}

void UpdateActive (void)
{
	S9xLuaActive = !LuaEng.scripts.empty() || LuaEng.present_pending;
}

bool TakeStopRequest (int id)
{
	if (!any_stop_request.load(std::memory_order_relaxed))
		return false;

	std::lock_guard<std::mutex>	lock(stop_mutex);
	bool	found = stop_requests.erase(id) != 0;
	any_stop_request = !stop_requests.empty();
	return found;
}

int FindScriptLine (lua_State *T, LuaScript *s, int level);

void CountHook (lua_State *L, lua_Debug *)
{
	LuaScript	*s = LuaScriptOf(L);

	if (TakeStopRequest(s->id))
		s->interrupt = true;

	if (!s->interrupt && s->watchdog > 0.0)
	{
		std::chrono::duration<double>	ran = std::chrono::steady_clock::now() - LuaEng.call_started;
		if (ran.count() > s->watchdog)
		{
			s->interrupt = true;
			s->failed = true;
			s->end_line = FindScriptLine(L, s, 0);
			char	text[300];
			snprintf(text, sizeof(text), "%s:%d: stopped after %g s without letting the emulator run "
			         "(a loop without emu.frame_advance()?)", s->name.c_str(), s->end_line, s->watchdog);
			s->end_message = text;
		}
	}

	if (s->interrupt)
	{
		// Check every instruction from now on so a pcall loop can't swallow the stop.
		lua_sethook(L, CountHook, LUA_MASKCOUNT, 1);
		lua_pushlightuserdata(L, &kStopSentinel);
		lua_error(L);
	}
}

int FindScriptLine (lua_State *T, LuaScript *s, int level)
{
	lua_Debug	ar;
	std::string	chunk = "@" + s->name;

	for (; lua_getstack(T, level, &ar); level++)
	{
		if (lua_getinfo(T, "Sl", &ar) && ar.currentline > 0 && chunk == ar.source)
			return ar.currentline;
	}
	return 0;
}

// "name.lua:12: message" from the compiler.
int ParseErrorLine (const std::string &message, const std::string &name)
{
	std::string	prefix = name + ":";
	if (message.compare(0, prefix.size(), prefix) != 0)
		return 0;
	return atoi(message.c_str() + prefix.size());
}

const char *ErrorText (lua_State *L, int index)
{
	if (const char *text = lua_tostring(L, index))
		return text;
	if (luaL_callmeta(L, index, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
		return lua_tostring(L, -1);
	return lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, index));
}

int MessageHandler (lua_State *L)
{
	if (lua_touserdata(L, 1) == &kStopSentinel)
		return 1;

	LuaScript	*s = LuaScriptOf(L);
	const char	*text = ErrorText(L, 1);
	if (!s->end_line)
		s->end_line = FindScriptLine(L, s, 1);
	luaL_traceback(L, L, text, 1);
	return 1;
}

void RecordFailure (LuaScript *s, lua_State *L)
{
	s->ending = true;
	if (lua_touserdata(L, -1) == &kStopSentinel)
		return;
	if (!s->failed)
	{
		s->failed = true;
		s->end_message = ErrorText(L, -1);
	}
}

struct CallScope
{
	LuaScript	*running;
	LuaContext	context;

	CallScope (LuaScript *s, LuaContext ctx)
		: running(LuaEng.running), context(LuaEng.context)
	{
		if (context == LUA_CTX_NONE)
			LuaEng.call_started = std::chrono::steady_clock::now();
		LuaEng.running = s;
		LuaEng.context = ctx;
	}

	~CallScope ()
	{
		LuaEng.running = running;
		LuaEng.context = context;
	}
};

// Calls the function below `nargs` arguments on s->L. False if it failed,
// in which case the script is already marked to end.
bool CallLua (LuaScript *s, int nargs, int nresults, LuaContext ctx)
{
	lua_State	*L = s->L;
	int			base = lua_gettop(L) - nargs;

	lua_pushcfunction(L, MessageHandler);
	lua_insert(L, base);

	int	status;
	{
		CallScope	scope(s, ctx);
		status = lua_pcall(L, nargs, nresults, base);
	}

	lua_remove(L, base);
	if (status == LUA_OK)
		return true;

	RecordFailure(s, L);
	lua_pop(L, 1);
	return false;
}

bool HasLiveCallbacks (const LuaScript *s)
{
	for (auto &cb : s->callbacks)
		if (!cb.removed && cb.kind != LUA_CB_STOP)
			return true;
	return false;
}

void ResumeMain (LuaScript *s)
{
	if (s->ending || s->wait == LuaScript::WAIT_DONE)
		return;

	// A bare coroutine.yield() in the body behaves like emu.yield().
	s->wait = LuaScript::WAIT_TICK;

	int	results = 0;
	int	status;
	{
		CallScope	scope(s, LUA_CTX_SAFE);
		status = lua_resume(s->main, s->L, 0, &results);
	}

	if (status == LUA_YIELD)
	{
		lua_pop(s->main, results);
		return;
	}

	if (status == LUA_OK)
	{
		lua_pop(s->main, results);
		s->wait = LuaScript::WAIT_DONE;
		return;
	}

	s->wait = LuaScript::WAIT_DONE;
	s->ending = true;
	if (lua_touserdata(s->main, -1) == &kStopSentinel)
		return;

	if (!s->failed)
	{
		s->failed = true;
		const char	*text = ErrorText(s->main, -1);
		s->end_line = FindScriptLine(s->main, s, 0);
		luaL_traceback(s->L, s->main, text, 0);
		s->end_message = lua_tostring(s->L, -1);
		lua_pop(s->L, 1);
	}
}

void FireStopCallbacks (LuaScript *s)
{
	// A stopped script still gets to clean up, with a fresh watchdog.
	TakeStopRequest(s->id);
	s->interrupt = false;
	s->ending = false;
	s->failed = false;
	s->end_line = 0;
	lua_sethook(s->L, CountHook, LUA_MASKCOUNT, kHookInterval);

	size_t	count = s->callbacks.size();
	for (size_t i = 0; i < count; i++)
	{
		if (s->callbacks[i].kind != LUA_CB_STOP || s->callbacks[i].removed)
			continue;
		lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->callbacks[i].ref);
		if (!CallLua(s, 0, 0, LUA_CTX_SAFE))
		{
			if (s->failed)
				LuaEmit(s->id, S9X_LUA_EVENT_ERROR, s->end_message, s->end_line);
			break;
		}
	}
	s->ending = true;
}

void SweepCallbacks (LuaScript *s)
{
	auto	&v = s->callbacks;
	size_t	before = v.size();
	v.erase(std::remove_if(v.begin(), v.end(), [](const LuaCallback &cb) { return cb.removed; }), v.end());
	if (v.size() != before)
		hooks_dirty = true;
}

void RedrawPaused (void);

// Closes every script that has ended. Only at the outermost level: no script
// code may be on the stack.
void Reap (void)
{
	if (LuaEng.running)
		return;

	bool	changed = false;
	for (size_t i = 0; i < LuaEng.scripts.size();)
	{
		LuaScript	*s = LuaEng.scripts[i].get();
		SweepCallbacks(s);

		bool	finished = false;
		if (!s->ending && s->wait == LuaScript::WAIT_DONE && !HasLiveCallbacks(s))
			s->ending = finished = true;

		if (!s->ending)
		{
			i++;
			continue;
		}

		LuaFlushOutput(s);
		bool	failed = s->failed;
		int		line = s->end_line;
		if (failed)
			LuaEmit(s->id, S9X_LUA_EVENT_ERROR, s->end_message, line);

		FireStopCallbacks(s);
		LuaFlushOutput(s);
		LuaEmit(s->id, S9X_LUA_EVENT_ENDED, failed ? "failed" : finished ? "finished" : "stopped",
		        failed ? std::max(line, 1) : 0);

		if (!s->layer.empty())
			LuaEng.overlay_dirty = true;
		lua_close(s->L);
		LuaEng.scripts.erase(LuaEng.scripts.begin() + i);
		changed = true;
	}

	if (changed)
	{
		hooks_dirty = true;
		UpdateInfo();
	}
	if (hooks_dirty)
		LuaRebuildHooks();
	UpdateActive();
}

void Fire (int kind, LuaContext ctx, const std::function<int (lua_State *)> &push)
{
	for (size_t i = 0; i < LuaEng.scripts.size(); i++)
	{
		LuaScript	*s = LuaEng.scripts[i].get();
		if (s->paused || s->ending)
			continue;

		size_t	count = s->callbacks.size();
		for (size_t j = 0; j < count && !s->ending; j++)
		{
			const LuaCallback	&cb = s->callbacks[j];
			if (cb.kind != kind || cb.removed || !cb.enabled)
				continue;
			lua_rawgeti(s->L, LUA_REGISTRYINDEX, cb.ref);
			int	nargs = push ? push(s->L) : 0;
			CallLua(s, nargs, 0, ctx);
		}
	}
}

void ClearLayers (bool only_tick_scripts)
{
	for (auto &s : LuaEng.scripts)
	{
		if (!s->auto_clear || s->layer.empty())
			continue;
		if (only_tick_scripts)
		{
			bool	runs = !s->paused && (s->wait == LuaScript::WAIT_TICK || s->wait == LuaScript::WAIT_START);
			for (auto &cb : s->callbacks)
				runs = runs || (cb.kind == LUA_CB_IDLE && !cb.removed && !s->paused);
			if (!runs)
				continue;
		}
		s->layer.clear();
		LuaEng.overlay_dirty = true;
	}
}

void ResumeScripts (bool frame)
{
	for (size_t i = 0; i < LuaEng.scripts.size(); i++)
	{
		LuaScript	*s = LuaEng.scripts[i].get();
		if (s->paused || s->ending)
			continue;

		switch (s->wait)
		{
			case LuaScript::WAIT_START:
			case LuaScript::WAIT_TICK:
				ResumeMain(s);
				break;

			case LuaScript::WAIT_FRAMES:
				if (frame && --s->wait_frames <= 0)
					ResumeMain(s);
				break;

			case LuaScript::WAIT_DONE:
				break;
		}
	}
}

void DispatchNotices (void)
{
	std::vector<Notice>	list;
	{
		std::lock_guard<std::mutex>	lock(notice_mutex);
		list.swap(notices);
	}

	for (size_t i = 0; i < list.size(); i++)
	{
		const Notice	&n = list[i];
		// One load can pass through several loaders (GB under the SGB BIOS).
		if (n.kind == LUA_CB_ROM_LOADED && i && list[i - 1].kind == LUA_CB_ROM_LOADED)
			continue;

		switch (n.kind)
		{
			case LUA_CB_RESET:
				Fire(LUA_CB_RESET, LUA_CTX_SAFE, [&](lua_State *L) {
					lua_pushstring(L, n.flag ? "power" : "soft");
					return 1;
				});
				break;

			case LUA_CB_ROM_LOADED:
				LuaEng.frame_count = LuaEng.lag_count = 0;
				LuaEng.clean_valid = false;
				Fire(LUA_CB_ROM_LOADED, LUA_CTX_SAFE, nullptr);
				break;

			case LUA_CB_STATE_LOADED:
				LuaEng.clean_valid = false;
				// fall through
			case LUA_CB_STATE_SAVED:
				Fire(n.kind, LUA_CTX_SAFE, [&](lua_State *L) {
					lua_pushstring(L, n.text.c_str());
					return 1;
				});
				break;
		}
	}
}

void PostNotice (int kind, const char *text, bool flag)
{
	if (!S9xLuaActive)
		return;
	std::lock_guard<std::mutex>	lock(notice_mutex);
	notices.push_back({ kind, text ? text : "", flag });
}

void Present (void)
{
	LuaEng.present_pending = false;
	int	w = LuaEng.present_width, h = LuaEng.present_height;
	if (w <= 0 || h <= 0)
		return;

	// Keep the bare picture so the overlay can change while paused.
	LuaEng.clean.resize((size_t) w * h);
	for (int y = 0; y < h; y++)
		memcpy(&LuaEng.clean[(size_t) y * w], GFX.Screen + (size_t) y * GFX.RealPPL, w * sizeof(uint16));
	LuaEng.clean_width = w;
	LuaEng.clean_height = h;
	LuaEng.clean_valid = true;

	LuaRenderOverlay(GFX.Screen, GFX.RealPPL, w, h);
	LuaEng.overlay_dirty = false;

	if (LuaEng.present_partial)
	{
		S9xContinueUpdate(w, h);
		return;
	}
	if (Settings.AutoDisplayMessages)
		S9xDisplayMessages(GFX.Screen, GFX.RealPPL, w, h, 1);
	S9xDeinitUpdate(w, h);
}

void RedrawPaused (void)
{
	LuaEng.overlay_dirty = false;
	if (!LuaEng.clean_valid || !Settings.Paused)
		return;

	int	w = LuaEng.clean_width, h = LuaEng.clean_height;
	for (int y = 0; y < h; y++)
		memcpy(GFX.Screen + (size_t) y * GFX.RealPPL, &LuaEng.clean[(size_t) y * w], w * sizeof(uint16));

	LuaRenderOverlay(GFX.Screen, GFX.RealPPL, w, h);
	if (Settings.AutoDisplayMessages)
		S9xDisplayMessages(GFX.Screen, GFX.RealPPL, w, h, 1);
	S9xDeinitUpdate(w, h);
}

// `address` is the folded form of `raw`; a hook covering either fires.
uint8 DispatchMemory (int kind, uint32 raw, uint32 address, uint8 value)
{
	LuaContext	ctx = kind == LUA_CB_EXEC ? LUA_CTX_EXEC : LUA_CTX_HOOK;
	auto		&refs = hook_refs[HookSlot(kind)];

	dispatch_depth++;
	for (size_t i = 0; i < refs.size(); i++)
	{
		LuaScript	*s = refs[i].script;
		if (s->paused || s->ending)
			continue;

		const LuaCallback	&cb = s->callbacks[refs[i].index];
		bool	covered = (address >= cb.first && address <= cb.last) || (raw >= cb.first && raw <= cb.last);
		if (cb.removed || !cb.enabled || !covered)
			continue;

		lua_rawgeti(s->L, LUA_REGISTRYINDEX, cb.ref);
		lua_pushinteger(s->L, address);
		if (kind != LUA_CB_EXEC)
			lua_pushinteger(s->L, value);

		if (CallLua(s, kind == LUA_CB_EXEC ? 1 : 2, 1, ctx))
		{
			if (kind != LUA_CB_EXEC && lua_isinteger(s->L, -1))
				value = (uint8) lua_tointeger(s->L, -1);
			lua_pop(s->L, 1);
		}
	}
	dispatch_depth--;

	if (!dispatch_depth && hooks_dirty)
		LuaRebuildHooks();
	return value;
}

inline bool PageHooked (int slot, uint32 address)
{
	uint32	page = address >> 8;
	return hook_pages[slot][page >> 3] & (1 << (page & 7));
}

uint32 NextAddress (uint32 address, int wrap)
{
	switch (wrap)
	{
		case WRAP_PAGE:	return (address & 0xffff00) | ((address + 1) & 0xff);
		case WRAP_BANK:	return (address & 0xff0000) | ((address + 1) & 0xffff);
		default:		return (address + 1) & 0xffffff;
	}
}

const char	kPrelude[] = R"lua(
local dir = ...
local function resolve(path)
  if type(path) ~= "string" or dir == "" then return path end
  if path:match("^[/\\]") or path:match("^%a:") then return path end
  return dir .. path
end
local open, lines, dofile_, loadfile_ = io.open, io.lines, dofile, loadfile
io.open = function(path, ...) return open(resolve(path), ...) end
io.lines = function(path, ...)
  if path == nil then error("io.lines: scripts have no console input", 2) end
  return lines(resolve(path), ...)
end
dofile = function(path) return dofile_(resolve(path)) end
loadfile = function(path, ...) return loadfile_(resolve(path), ...) end
io.read = function() error("io.read: scripts have no console input", 2) end
local write, stdout, console_write = io.write, io.stdout, console.write
io.write = function(...)
  if io.output() ~= stdout then return write(...) end
  console_write(...)
  return stdout
end
if dir ~= "" then
  package.path = dir .. "?.lua;" .. dir .. "?/init.lua;" .. package.path
end
)lua";

int l_print (lua_State *L)
{
	LuaFlushOutput(LuaScriptOf(L));
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
	LuaEmit(LuaScriptOf(L)->id, S9X_LUA_EVENT_PRINT, line);
	return 0;
}

int l_exit (lua_State *L)
{
	LuaScriptOf(L)->interrupt = true;
	lua_pushlightuserdata(L, &kStopSentinel);
	return lua_error(L);
}

}	// namespace

LuaScript *LuaScriptOf (lua_State *L)
{
	return *(LuaScript **) lua_getextraspace(L);
}

void LuaEmit (int script, S9xLuaEventKind kind, const std::string &text, int line)
{
	if (echo_output && kind >= S9X_LUA_EVENT_PRINT && kind <= S9X_LUA_EVENT_ERROR)
	{
		FILE	*out = kind == S9X_LUA_EVENT_ERROR ? stderr : stdout;
		fprintf(out, "%s\n", text.c_str());
		fflush(out);
	}

	{
		std::lock_guard<std::mutex>	lock(event_mutex);
		if (pending_events.size() >= kMaxEvents)
			pending_events.pop_front();
		pending_events.push_back({ script, kind, text, line });
	}

	if (LuaEng.frontend.wake_ui)
		LuaEng.frontend.wake_ui();
}

void LuaWrite (LuaScript *s, const std::string &text)
{
	s->partial_line += text;
	size_t	newline;
	while ((newline = s->partial_line.find('\n')) != std::string::npos)
	{
		LuaEmit(s->id, S9X_LUA_EVENT_PRINT, s->partial_line.substr(0, newline));
		s->partial_line.erase(0, newline + 1);
	}
}

void LuaFlushOutput (LuaScript *s)
{
	if (s->partial_line.empty())
		return;
	LuaEmit(s->id, S9X_LUA_EVENT_PRINT, s->partial_line);
	s->partial_line.clear();
}

void LuaRequireSafe (lua_State *L, const char *what)
{
	if (LuaEng.context == LUA_CTX_HOOK || LuaEng.context == LUA_CTX_EXEC)
		luaL_error(L, "%s can't be used inside a memory hook", what);
}

bool LuaGameLoaded (void)
{
	return game_loaded;
}

void LuaRequireGame (lua_State *L)
{
	if (!game_loaded)
		luaL_error(L, "no game is loaded");
}

uint32 LuaNormalizeBus (uint32 address)
{
	address &= 0xffffff;
	if (!(address & 0x400000))
	{
		uint32	offset = address & 0xffff;
		if (offset < 0x2000)
			return 0x7e0000 | offset;	// low WRAM mirror
		if (offset < 0x6000)
			return offset;				// I/O registers live in every system bank
	}
	return address;
}

void LuaRebuildHooks (void)
{
	if (dispatch_depth)
	{
		hooks_dirty = true;
		return;
	}
	hooks_dirty = false;

	memset(hook_pages, 0, sizeof(hook_pages));
	uint8	mask = 0;
	for (int k = 0; k < 3; k++)
		hook_refs[k].clear();

	for (auto &s : LuaEng.scripts)
	{
		if (s->paused || s->ending)
			continue;
		for (size_t i = 0; i < s->callbacks.size(); i++)
		{
			const LuaCallback	&cb = s->callbacks[i];
			if (cb.removed || !cb.enabled || (cb.kind != LUA_CB_READ && cb.kind != LUA_CB_WRITE && cb.kind != LUA_CB_EXEC))
				continue;

			int	slot = HookSlot(cb.kind);
			hook_refs[slot].push_back({ s.get(), i });
			for (uint32 page = cb.first >> 8; page <= (cb.last >> 8); page++)
				hook_pages[slot][page >> 3] |= 1 << (page & 7);
			mask |= 1 << slot;
		}
	}
	S9xLuaHookMask = mask;
}

int LuaAddCallback (lua_State *L, int kind, int fn_index, uint32 first, uint32 last)
{
	LuaScript	*s = LuaScriptOf(L);
	luaL_checktype(L, fn_index, LUA_TFUNCTION);

	LuaCallback	cb;
	cb.handle = LuaEng.next_handle++;
	cb.kind = kind;
	lua_pushvalue(L, fn_index);
	cb.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	cb.first = first;
	cb.last = last;
	s->callbacks.push_back(cb);

	if (kind == LUA_CB_READ || kind == LUA_CB_WRITE || kind == LUA_CB_EXEC)
		LuaRebuildHooks();

	LuaPushHandle(L, cb.handle);
	return 1;
}

int LuaRaiseStop (lua_State *L)
{
	return l_exit(L);
}

//------------------------------------------------------------------------------
// Frontend API

void S9xLuaSetFrontend (const S9xLuaFrontend &frontend)
{
	LuaEng.frontend = frontend;
}

int S9xLuaNewScriptId (void)
{
	return next_script_id++;
}

void S9xLuaSetEcho (bool echo)
{
	echo_output = echo;
}

bool S9xLuaRun (int id, const std::string &code, const std::string &name, const std::string &path)
{
	auto	s = std::make_unique<LuaScript>();
	s->id = id;
	s->name = name.empty() ? "untitled.lua" : name;
	s->path = path;
	s->dir = DirectoryOf(path);

	LuaEmit(id, S9X_LUA_EVENT_STARTED, s->name);

	lua_State	*L = luaL_newstate();
	if (!L)
	{
		LuaEmit(id, S9X_LUA_EVENT_ERROR, "not enough memory to start a script");
		LuaEmit(id, S9X_LUA_EVENT_ENDED, "failed", 1);
		return false;
	}
	s->L = L;
	*(LuaScript **) lua_getextraspace(L) = s.get();

	luaL_openlibs(L);
	lua_register(L, "print", l_print);
	lua_getglobal(L, "os");
	lua_pushcfunction(L, l_exit);
	lua_setfield(L, -2, "exit");
	lua_pop(L, 1);

	LuaOpenEmu(L);
	LuaOpenMemory(L);
	LuaOpenGui(L);
	LuaOpenInput(L);

	if (luaL_loadbuffer(L, kPrelude, sizeof(kPrelude) - 1, "=prelude") == LUA_OK)
	{
		lua_pushstring(L, s->dir.c_str());
		lua_pcall(L, 1, 0, 0);
	}
	lua_settop(L, 0);

	std::string	chunk = "@" + s->name;
	if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK)
	{
		std::string	message = lua_tostring(L, -1);
		LuaEmit(id, S9X_LUA_EVENT_ERROR, message, ParseErrorLine(message, s->name));
		LuaEmit(id, S9X_LUA_EVENT_ENDED, "failed", std::max(ParseErrorLine(message, s->name), 1));
		lua_close(L);
		return false;
	}

	lua_sethook(L, CountHook, LUA_MASKCOUNT, kHookInterval);
	s->main = lua_newthread(L);
	s->main_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_xmove(L, s->main, 1);
	s->wait = LuaScript::WAIT_START;

	LuaEng.scripts.push_back(std::move(s));
	UpdateInfo();
	UpdateActive();
	return true;
}

bool S9xLuaRunFile (int id, const std::string &path)
{
	std::ifstream	file(path.c_str(), std::ios::binary);	// UTF-8 aware on win32
	if (!file)
	{
		LuaEmit(id, S9X_LUA_EVENT_STARTED, BaseName(path));
		LuaEmit(id, S9X_LUA_EVENT_ERROR, "can't open " + path);
		LuaEmit(id, S9X_LUA_EVENT_ENDED, "failed");
		return false;
	}

	std::stringstream	text;
	text << file.rdbuf();
	return S9xLuaRun(id, text.str(), BaseName(path), path);
}

void S9xLuaStop (int id)
{
	{
		// A request for a script that already ended mustn't linger.
		std::lock_guard<std::mutex>	lock(stop_mutex);
		if (id == 0)
			stop_requests.clear();
		else
			stop_requests.erase(id);
		any_stop_request = !stop_requests.empty();
	}
	for (auto &s : LuaEng.scripts)
		if (id == 0 || s->id == id)
			s->ending = true;
	Reap();
	if (LuaEng.overlay_dirty)
		RedrawPaused();
}

void S9xLuaPause (int id, bool paused)
{
	for (auto &s : LuaEng.scripts)
	{
		if ((id != 0 && s->id != id) || s->paused == paused)
			continue;
		s->paused = paused;
		LuaEmit(s->id, paused ? S9X_LUA_EVENT_PAUSED : S9X_LUA_EVENT_RESUMED, s->name);
	}
	LuaRebuildHooks();
	UpdateInfo();
}

void S9xLuaShutdown (void)
{
	S9xLuaStop(0);
	LuaEng.clean.clear();
	LuaEng.clean_valid = false;
}

void S9xLuaRequestStop (int id)
{
	std::vector<int>	ids;
	if (id == 0)
	{
		for (auto &info : S9xLuaScripts())
			ids.push_back(info.id);
	}
	else
		ids.push_back(id);

	std::lock_guard<std::mutex>	lock(stop_mutex);
	stop_requests.insert(ids.begin(), ids.end());
	any_stop_request = !stop_requests.empty();
}

std::vector<S9xLuaScriptInfo> S9xLuaScripts (void)
{
	std::lock_guard<std::mutex>	lock(info_mutex);
	return script_info;
}

bool S9xLuaTakeEvents (std::vector<S9xLuaEvent> &events)
{
	std::lock_guard<std::mutex>	lock(event_mutex);
	if (pending_events.empty())
		return false;
	events.insert(events.end(), pending_events.begin(), pending_events.end());
	pending_events.clear();
	return true;
}

void S9xLuaIdle (void)
{
	// A port that pumps messages mid-frame mustn't run scripts there.
	if (!S9xLuaActive || LuaEng.in_frame)
		return;

	auto	now = std::chrono::steady_clock::now();
	if (now - last_idle < std::chrono::milliseconds(15))
		return;
	last_idle = now;

	DispatchNotices();
	ClearLayers(true);
	Fire(LUA_CB_IDLE, LUA_CTX_SAFE, nullptr);
	ResumeScripts(false);
	Reap();
	for (auto &s : LuaEng.scripts)
		LuaFlushOutput(s.get());
	if (LuaEng.overlay_dirty)
		RedrawPaused();
}

//------------------------------------------------------------------------------
// Core hooks

void S9xLuaFrameStart (void)
{
	if (LuaEng.present_pending)
		Present();	// a frame that never reached S9xLuaFrameEnd

	// Run-ahead emulates frames that get rolled back: every frame after the
	// first one with Settings.InRunAhead set, up to the next such first one.
	bool	in_run_ahead = Settings.InRunAhead;
	LuaEng.speculative = LuaEng.last_in_run_ahead;
	LuaEng.last_in_run_ahead = in_run_ahead;
	LuaEng.in_frame = true;

	if (LuaEng.speculative)
		return;

	LuaRestoreInput();
	ClearLayers(false);
	DispatchNotices();
	Fire(LUA_CB_FRAME_START, LUA_CTX_SAFE, nullptr);
	Reap();
	LuaApplyInput();
}

void S9xLuaInputLatched (void)
{
	if (LuaEng.speculative)
		return;
	for (int i = 0; i < 8; i++)
		LuaEng.input[i] = MovieGetJoypad(i);
}

void S9xLuaFrameEnd (void)
{
	LuaEng.in_frame = false;
	if (!LuaEng.speculative)
	{
		LuaRestoreInput();
		LuaEng.frame_count++;
		LuaEng.lagged = !Settings.SuperGameBoy && !pad_read_last;
		if (LuaEng.lagged)
			LuaEng.lag_count++;

		Fire(LUA_CB_FRAME_END, LUA_CTX_SAFE, nullptr);
		ResumeScripts(true);
		Reap();
		for (auto &s : LuaEng.scripts)
			LuaFlushOutput(s.get());
	}

	if (LuaEng.present_pending)
		Present();
	UpdateActive();
}

bool S9xLuaDeferPresent (bool partial)
{
	if (!S9xLuaActive)
		return false;
	LuaEng.present_pending = true;
	LuaEng.present_partial = partial;
	LuaEng.present_width = IPPU.RenderedScreenWidth;
	LuaEng.present_height = IPPU.RenderedScreenHeight;
	return true;
}

uint8 S9xLuaHookRead (uint32 address, uint8 value)
{
	if (LuaEng.speculative || LuaEng.context != LUA_CTX_NONE)
		return value;
	uint32	raw = address & 0xffffff, folded = LuaNormalizeBus(raw);
	if (!PageHooked(0, folded) && !PageHooked(0, raw))
		return value;
	return DispatchMemory(LUA_CB_READ, raw, folded, value);
}

uint8 S9xLuaHookWrite (uint32 address, uint8 value)
{
	if (LuaEng.speculative || LuaEng.context != LUA_CTX_NONE)
		return value;
	uint32	raw = address & 0xffffff, folded = LuaNormalizeBus(raw);
	if (!PageHooked(1, folded) && !PageHooked(1, raw))
		return value;
	return DispatchMemory(LUA_CB_WRITE, raw, folded, value);
}

uint16 S9xLuaHookReadWord (uint32 address, int wrap, uint16 value)
{
	uint8	lo = S9xLuaHookRead(address, value & 0xff);
	uint8	hi = S9xLuaHookRead(NextAddress(address, wrap), value >> 8);
	return lo | (hi << 8);
}

uint16 S9xLuaHookWriteWord (uint32 address, int wrap, uint16 value)
{
	uint8	lo = S9xLuaHookWrite(address, value & 0xff);
	uint8	hi = S9xLuaHookWrite(NextAddress(address, wrap), value >> 8);
	return lo | (hi << 8);
}

void S9xLuaHookExec (uint32 address)
{
	if (LuaEng.speculative || LuaEng.context != LUA_CTX_NONE)
		return;
	uint32	raw = address & 0xffffff, folded = LuaNormalizeBus(raw);
	if (!PageHooked(2, folded) && !PageHooked(2, raw))
		return;

	DispatchMemory(LUA_CB_EXEC, raw, folded, 0);
	if (Registers.PBPC != address)
		S9xSetPCBase(Registers.PBPC);
}

void S9xLuaNotifyReset (bool power)
{
	PostNotice(LUA_CB_RESET, nullptr, power);
}

void S9xLuaNotifyStateSaved (const char *filename)
{
	PostNotice(LUA_CB_STATE_SAVED, filename, false);
}

void S9xLuaNotifyStateLoaded (const char *filename)
{
	PostNotice(LUA_CB_STATE_LOADED, filename, false);
}

void S9xLuaNotifyRomLoaded (void)
{
	game_loaded = true;
	LuaEng.frame_count = LuaEng.lag_count = 0;	// the emulation thread is held during loads
	PostNotice(LUA_CB_ROM_LOADED, nullptr, false);
}
