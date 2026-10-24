/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _LUA_ENGINE_H_
#define _LUA_ENGINE_H_

#include <functional>
#include <string>
#include <vector>

#include "lua/lua_hooks.h"

// The frontends' side of Lua scripting. Functions marked "emulation thread"
// must run where S9xMainLoop runs; the rest are safe from any thread.

enum S9xLuaScriptState
{
	S9X_LUA_RUNNING,
	S9X_LUA_PAUSED
};

enum S9xLuaEventKind
{
	S9X_LUA_EVENT_STARTED,	// text = script name
	S9X_LUA_EVENT_PRINT,
	S9X_LUA_EVENT_INFO,
	S9X_LUA_EVENT_WARNING,
	S9X_LUA_EVENT_ERROR,
	S9X_LUA_EVENT_PAUSED,
	S9X_LUA_EVENT_RESUMED,
	S9X_LUA_EVENT_ENDED,	// line > 0: it failed there (in its own file)
	S9X_LUA_EVENT_CLEAR		// console.clear()
};

struct S9xLuaEvent
{
	int				script;
	S9xLuaEventKind	kind;
	std::string		text;
	int				line = 0;
};

struct S9xLuaScriptInfo
{
	int					id;
	std::string			name;
	std::string			path;
	S9xLuaScriptState	state;
};

// What the engine asks of the port. Every member is optional.
struct S9xLuaFrontend
{
	std::function<void (bool)>				set_paused;	// emu.pause()/emu.unpause()
	std::function<void (bool)>				reset;		// true = power cycle
	std::function<std::string (int)>		state_slot_path;	// savestate.save(slot)
	std::function<void ()>					wake_ui;	// events are waiting

	// The debugger, which is off unless both are set. A script stopped at a
	// breakpoint waits on the emulation thread, calling debug_wait until an
	// action lets it go on; a port that runs emulation on its UI thread pumps
	// its messages there.
	std::function<void (int, bool)>			debug_break;	// script, stopped (true) or going on
	std::function<void ()>					debug_wait;
};

void S9xLuaSetFrontend (const S9xLuaFrontend &frontend);

// Ids are handed out here so a UI can track a script before it starts.
int  S9xLuaNewScriptId (void);

// Emulation thread. Compiles and starts `code`; its first lines run at the
// next frame end or idle tick. `path` may be empty for an unsaved buffer.
bool S9xLuaRun (int id, const std::string &code, const std::string &name, const std::string &path);
bool S9xLuaRunFile (int id, const std::string &path);
void S9xLuaStop (int id);		// 0 = every script
void S9xLuaPause (int id, bool paused);
void S9xLuaShutdown (void);

// While nothing is being emulated (paused, no game), call about 60 times a
// second so emu.yield() scripts keep running. Emulation thread.
void S9xLuaIdle (void);

// Any thread. Interrupts a script stuck in a loop; S9xLuaStop does the rest.
void S9xLuaRequestStop (int id);

std::vector<S9xLuaScriptInfo> S9xLuaScripts (void);
bool S9xLuaTakeEvents (std::vector<S9xLuaEvent> &events);
void S9xLuaSetEcho (bool echo);	// also print script output to stdout

// Debugger. Everything here is safe from any thread. Breakpoints are lines in
// the script's own file; frames and variables describe the stopped script.
struct S9xLuaBreakpoint
{
	int			line = 0;
	bool		enabled = true;
	std::string	condition;		// a Lua expression: stop only when it's true
	std::string	hit_condition;	// "5" (the 5th hit), ">= 5", "> 5" or "% 5" (every 5th)
	std::string	log_message;	// not empty: print it, {expressions} filled in, instead of stopping
};

enum S9xLuaDebugAction
{
	S9X_LUA_DEBUG_CONTINUE,
	S9X_LUA_DEBUG_STEP_INTO,
	S9X_LUA_DEBUG_STEP_OVER,
	S9X_LUA_DEBUG_STEP_OUT,
	S9X_LUA_DEBUG_BREAK,	// stop at the script's next line
	S9X_LUA_DEBUG_STOP		// end the stopped script
};

enum S9xLuaDebugEventKind
{
	S9X_LUA_DEBUG_STOPPED,		// reason, message, frames
	S9X_LUA_DEBUG_RESUMED,
	S9X_LUA_DEBUG_VARIABLES,	// ref, variables
	S9X_LUA_DEBUG_EVALUATED		// request, variables[0]; failed: its value is the error
};

struct S9xLuaDebugFrame
{
	std::string	function;	// "update", "main chunk", "[C] pcall"
	std::string	source;		// what to show: "game.lua", "lib/util.lua"
	std::string	path;		// the file Lua loaded it from, when it came from one
	int			line = 0;	// 0 in C functions
	bool		own = false;	// in the script's own file
};

struct S9xLuaDebugVariable
{
	std::string	name, value, type;
	int			ref = 0;	// > 0: a table whose fields S9xLuaDebugRequestVariables lists
};

struct S9xLuaDebugEvent
{
	S9xLuaDebugEventKind	kind;
	int						script = 0;
	std::string				reason;		// STOPPED: "breakpoint", "step", "break" or "error"
	std::string				message;	// STOPPED on an error: the error
	std::vector<S9xLuaDebugFrame>		frames;
	int						ref = 0, request = 0;
	std::vector<S9xLuaDebugVariable>	variables;
	bool					failed = false;
};

bool S9xLuaDebugAvailable (void);
void S9xLuaDebugSetBreakpoints (int script, const std::vector<S9xLuaBreakpoint> &breakpoints);
void S9xLuaDebugSetBreakOnError (bool enabled);
void S9xLuaDebugCommand (int script, S9xLuaDebugAction action);
int  S9xLuaDebugStoppedScript (void);	// 0 while no script is stopped
int  S9xLuaDebugScopeRef (int frame, int scope);	// scope: 0 locals, 1 upvalues, 2 globals
void S9xLuaDebugRequestVariables (int script, int ref);
void S9xLuaDebugEvaluate (int script, int frame, const std::string &expression, int request);
bool S9xLuaDebugTakeEvents (std::vector<S9xLuaDebugEvent> &events);

// Editor support: Lua syntax spans for highlighting and the API's names.
enum S9xLuaTokenKind
{
	S9X_LUA_TOKEN_TEXT,
	S9X_LUA_TOKEN_KEYWORD,
	S9X_LUA_TOKEN_BUILTIN,	// standard library and emulator modules
	S9X_LUA_TOKEN_NUMBER,
	S9X_LUA_TOKEN_STRING,
	S9X_LUA_TOKEN_COMMENT
};

struct S9xLuaToken
{
	int				start, length;	// in bytes
	S9xLuaTokenKind	kind;
};

// `state` carries an open long string or comment across lines: 0 at the
// start of a file, then pass back what the previous line returned.
int S9xLuaTokenize (const char *text, int length, int state, std::vector<S9xLuaToken> &tokens);
const std::vector<std::string> &S9xLuaApiNames (void);	// "emu.frame_advance", ...

struct S9xLuaApiDoc
{
	const char	*name;		// "emu.frame_advance"
	const char	*args;		// "([frames])"; empty for tables
	const char	*summary;
};

const S9xLuaApiDoc *S9xLuaApiDocs (size_t *count);
const S9xLuaApiDoc *S9xLuaFindApiDoc (const std::string &name);

struct S9xLuaExample
{
	const char	*title;
	const char	*filename;
	const char	*code;
};

const S9xLuaExample *S9xLuaExamples (size_t *count);

// The text a new script starts with.
const char *S9xLuaNewScriptTemplate (void);

#endif
