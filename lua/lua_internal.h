/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _LUA_INTERNAL_H_
#define _LUA_INTERNAL_H_

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Lua is built as C++ (see external/lua/README.snes9x), so no extern "C".
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "snes9x.h"
#include "lua/lua_engine.h"

enum LuaCallbackKind
{
	LUA_CB_FRAME_START,
	LUA_CB_FRAME_END,
	LUA_CB_IDLE,
	LUA_CB_RESET,
	LUA_CB_STATE_SAVED,
	LUA_CB_STATE_LOADED,
	LUA_CB_ROM_LOADED,
	LUA_CB_STOP,
	LUA_CB_READ,
	LUA_CB_WRITE,
	LUA_CB_EXEC
};

// What the running Lua code may do. Only SAFE may load states, reset or
// yield; EXEC sits on an instruction boundary, so it may still set registers.
enum LuaContext
{
	LUA_CTX_NONE,
	LUA_CTX_SAFE,
	LUA_CTX_EXEC,
	LUA_CTX_HOOK
};

struct LuaCallback
{
	int		handle;
	int		kind;
	int		ref;			// the function, in the script's registry
	uint32	first, last;	// memory hooks: normalized bus range
	bool	enabled = true;
	bool	removed = false;
};

struct LuaImage
{
	int		width = 0, height = 0;
	std::vector<uint32> argb;
};

struct LuaDrawCmd
{
	enum Type : uint8 { PIXEL, LINE, RECT, ELLIPSE, POLYGON, TEXT, IMAGE } type;
	int		x = 0, y = 0, w = 0, h = 0;	// LINE: x, y to w, h
	uint32	color = 0, fill = 0;		// ARGB; alpha 0 draws nothing
	uint32	outline = 0;				// TEXT
	uint8	font = 0, scale = 1;		// TEXT, IMAGE
	int8	align = -1, valign = -1;	// TEXT: -1 left/top, 0 centre, 1 right/bottom
	std::string	text;
	std::shared_ptr<LuaImage>	image;
	int		sx = 0, sy = 0;				// IMAGE: source rectangle origin
	int		alpha = 255;
	bool	smooth = false;				// LINE, ELLIPSE, POLYGON: anti-aliased
	// POLYGON vertices (whole pixels unless smooth). Smooth shapes are in
	// continuous coordinates: LINE x1, y1, x2, y2; ELLIPSE cx, cy, rx, ry.
	std::vector<double>	points;
};

struct LuaBreakpoint
{
	S9xLuaBreakpoint	spec;
	int			hits = 0;
};

struct LuaScript
{
	int			id = 0;
	std::string	name, path, dir;
	lua_State	*L = nullptr;
	lua_State	*main = nullptr;		// the body runs in this coroutine
	int			main_ref = LUA_NOREF;

	enum Wait { WAIT_START, WAIT_FRAMES, WAIT_TICK, WAIT_DONE } wait = WAIT_START;
	int			wait_frames = 0;
	bool		paused = false;
	bool		ending = false;			// close at the next safe point
	bool		failed = false;
	std::string	end_message;
	int			end_line = 0;
	std::atomic<bool>	interrupt { false };
	double		watchdog = 5.0;			// seconds a call may run without yielding

	std::vector<LuaCallback>	callbacks;

	std::vector<LuaDrawCmd>		layer;
	bool		auto_clear = true;
	float		opacity = 1.0f;
	bool		antialias = false;
	std::map<std::string, std::shared_ptr<LuaImage>>	images;

	uint16		pad_on[8] = {}, pad_off[8] = {};	// joypad.set for the next frame
	std::string	partial_line;			// console.write text not yet ended by '\n'

	// Debugger (lua_debug.cpp)
	std::map<int, LuaBreakpoint>	breakpoints;	// by line, in the script's own file
	uint32		breakpoints_version = 0;
	int			step = 0;
	lua_State	*step_thread = nullptr;
	int			step_depth = 0;
	bool		line_hook = false;		// its threads have the line hook on
	bool		evaluating = false;		// running a watch or a breakpoint condition
	int			threads_ref = LUA_NOREF;	// the coroutines it made, weakly
	int			baseline_ref = LUA_NOREF;	// the globals it started with
};

struct LuaEngine
{
	std::vector<std::unique_ptr<LuaScript>>	scripts;
	int			next_handle = 1;
	LuaScript	*running = nullptr;		// script whose code is on the stack
	LuaContext	context = LUA_CTX_NONE;
	std::chrono::steady_clock::time_point	call_started;

	bool		speculative = false;	// run-ahead frame that will be rolled back
	bool		last_in_run_ahead = false;
	bool		in_frame = false;		// between S9xLuaFrameStart and S9xLuaFrameEnd
	uint32		frame_count = 0, lag_count = 0;
	bool		lagged = false;

	uint16		input[8] = {};			// what the game saw last frame
	uint16		forced_mask[8] = {}, saved_bits[8] = {};

	bool		present_pending = false, present_partial = false;
	int			present_width = 0, present_height = 0;
	std::vector<uint16>	clean;			// last presented frame without the overlay
	int			clean_width = 0, clean_height = 0;
	bool		clean_valid = false;
	bool		overlay_dirty = false;

	S9xLuaFrontend	frontend;
};

extern LuaEngine	LuaEng;

LuaScript *LuaScriptOf (lua_State *L);
void LuaEmit (int script, S9xLuaEventKind kind, const std::string &text, int line = 0);
void LuaWrite (LuaScript *s, const std::string &text);	// console.write: no newline added
void LuaFlushOutput (LuaScript *s);
void LuaRequireSafe (lua_State *L, const char *what);
void LuaRequireGame (lua_State *L);
bool LuaGameLoaded (void);
void LuaPushHandle (lua_State *L, int handle);
int  LuaAddCallback (lua_State *L, int kind, int fn_index, uint32 first = 0, uint32 last = 0);
void LuaRebuildHooks (void);
uint32 LuaNormalizeBus (uint32 address);
int  LuaRaiseStop (lua_State *L);	// ends the calling script cleanly; doesn't return

// Every script thread's hook: the watchdog and stop requests on counts, the
// debugger on lines.
const int	kLuaHookInterval = 10000;	// VM instructions between count events
void LuaHook (lua_State *L, lua_Debug *ar);

// Debugger (lua_debug.cpp)
void LuaDebugInit (lua_State *L, LuaScript *s);			// before the prelude
void LuaDebugCaptureGlobals (lua_State *L, LuaScript *s);	// after it
void LuaDebugPrepare (LuaScript *s);		// before running the script's code
void LuaDebugCount (lua_State *L, LuaScript *s);
void LuaDebugLine (lua_State *L, lua_Debug *ar);
bool LuaDebugError (LuaScript *s, lua_State *L, int level, const char *message);	// true: the user ended it
bool LuaDebugStopped (void);
void LuaDebugAbort (int script);			// 0: every script
void LuaDebugForget (LuaScript *s);

// Modules, each registered into a fresh script state.
void LuaOpenEmu (lua_State *L);
void LuaOpenMemory (lua_State *L);
void LuaOpenGui (lua_State *L);
void LuaOpenInput (lua_State *L);

// gui: compositing the layers onto the finished frame.
void LuaRenderOverlay (uint16 *screen, int pitch, int width, int height);
bool LuaHasOverlay (void);
uint32 LuaCheckColor (lua_State *L, int index, uint32 fallback);

// Applied at frame start and undone at frame end.
void LuaApplyInput (void);
void LuaRestoreInput (void);

#endif
