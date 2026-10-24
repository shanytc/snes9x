/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The script debugger. A stopped script waits inside its line hook (or its
// error handler) on the emulation thread, and the editor's requests are served
// from that wait. The line hook is on only while a script has breakpoints, is
// stepping or has a break pending, so other scripts run at full speed.

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include "lua/lua_internal.h"

namespace
{

enum Step { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT };

struct Request
{
	enum Kind { ACTION, VARIABLES, EVALUATE } kind = ACTION;
	int			script = 0;
	int			action = 0;
	int			ref = 0, frame = 0, id = 0;
	std::string	expression;
};

std::mutex		mutex;			// what the UI shares with the emulation thread
std::condition_variable	wake;
std::map<int, std::vector<S9xLuaBreakpoint>>	shared_breakpoints;	// by script id
std::atomic<uint32>	breakpoints_version { 1 };
std::atomic<bool>	break_on_error { false };
std::atomic<int>	break_request { 0 };	// a script to stop at its next line
std::atomic<int>	stopped_script { 0 };
std::deque<Request>	requests;
std::deque<S9xLuaDebugEvent>	events;

// The stop being served (emulation thread only).
struct Stop
{
	LuaScript	*script = nullptr;
	lua_State	*thread = nullptr;	// whose stack the frames are on
	int			level = 0;			// the Lua level of the first frame
	int			frames = 0;
	int			refs = LUA_NOREF;	// tables handed to the UI, by ref
} stop;

const int	kMaxFrames = 64, kMaxFields = 500, kMaxString = 200;
std::chrono::steady_clock::time_point	eval_deadline;

bool Available (void)
{
	return (bool) LuaEng.frontend.debug_break;
}

void Emit (S9xLuaDebugEvent &&event)
{
	{
		std::lock_guard<std::mutex>	lock(mutex);
		events.push_back(std::move(event));
	}
	if (LuaEng.frontend.wake_ui)
		LuaEng.frontend.wake_ui();
}

int Depth (lua_State *L)
{
	lua_Debug	ar;
	int			n = 0;
	while (lua_getstack(L, n, &ar))
		n++;
	return n;
}

std::string OwnSource (LuaScript *s)
{
	return "@" + s->name;
}

//------------------------------------------------------------------------------
// Hooks and breakpoints

void SetHook (lua_State *L, bool lines)
{
	lua_sethook(L, LuaHook, LUA_MASKCOUNT | (lines ? LUA_MASKLINE : 0), kLuaHookInterval);
}

bool WantsLines (LuaScript *s)
{
	if (!Available())
		return false;
	if (s->step != STEP_NONE || break_request == s->id)
		return true;
	for (auto &b : s->breakpoints)
		if (b.second.spec.enabled)
			return true;
	return false;
}

// Every thread of the script, its coroutines too; `X` lends its stack.
void ApplyHooks (LuaScript *s, lua_State *X)
{
	bool	lines = WantsLines(s);
	if (s->interrupt || lines == s->line_hook)
		return;
	s->line_hook = lines;
	SetHook(s->L, lines);
	if (s->main)
		SetHook(s->main, lines);
	if (s->threads_ref == LUA_NOREF || !lua_checkstack(X, 4))
		return;
	lua_rawgeti(X, LUA_REGISTRYINDEX, s->threads_ref);
	lua_pushnil(X);
	while (lua_next(X, -2))
	{
		lua_pop(X, 1);
		if (lua_State *co = lua_tothread(X, -1))
			SetHook(co, lines);
	}
	lua_pop(X, 1);
}

void SyncBreakpoints (LuaScript *s)
{
	uint32	version = breakpoints_version;
	if (s->breakpoints_version == version)
		return;

	std::map<int, LuaBreakpoint>	next;
	{
		std::lock_guard<std::mutex>	lock(mutex);
		auto	it = shared_breakpoints.find(s->id);
		if (it != shared_breakpoints.end())
		{
			for (auto &spec : it->second)
			{
				LuaBreakpoint	&b = next[spec.line];
				b.spec = spec;
				auto	old = s->breakpoints.find(spec.line);
				if (old != s->breakpoints.end())
					b.hits = old->second.hits;
			}
		}
	}
	s->breakpoints.swap(next);
	s->breakpoints_version = version;
}

void Track (lua_State *L, int index)
{
	lua_State	*co = lua_tothread(L, index);
	LuaScript	*s = LuaScriptOf(L);
	if (!co || s->threads_ref == LUA_NOREF)
		return;
	index = lua_absindex(L, index);
	lua_rawgeti(L, LUA_REGISTRYINDEX, s->threads_ref);
	lua_pushvalue(L, index);
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);
	if (!s->interrupt)
		SetHook(co, s->line_hook);
}

// coroutine.create and coroutine.wrap, noting each coroutine so the line
// hook can reach it later.
int co_create (lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	Track(L, -1);
	return 1;
}

int co_wrap (lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	if (lua_getupvalue(L, -1, 1))	// the wrapper keeps its coroutine there
	{
		Track(L, -1);
		lua_pop(L, 1);
	}
	return 1;
}

//------------------------------------------------------------------------------
// Values

std::string Quote (const char *p, size_t length)
{
	std::string	out = "\"";
	for (size_t i = 0; i < length && i < (size_t) kMaxString; i++)
	{
		unsigned char	c = p[i];
		switch (c)
		{
			case '"':	out += "\\\""; break;
			case '\\':	out += "\\\\"; break;
			case '\n':	out += "\\n"; break;
			case '\r':	out += "\\r"; break;
			case '\t':	out += "\\t"; break;
			default:
				if (c < 0x20 || c == 0x7f)
				{
					char	escape[8];
					snprintf(escape, sizeof(escape), "\\%u", c);
					out += escape;
				}
				else
					out += (char) c;
		}
	}
	out += "\"";
	if (length > (size_t) kMaxString)
		out += "... (" + std::to_string(length) + " bytes)";
	return out;
}

std::string Describe (lua_State *L, int index, std::string *type = nullptr)
{
	index = lua_absindex(L, index);
	if (type)
		*type = luaL_typename(L, index);

	char	text[160];
	switch (lua_type(L, index))
	{
		case LUA_TNIL:
			return "nil";

		case LUA_TBOOLEAN:
			return lua_toboolean(L, index) ? "true" : "false";

		case LUA_TNUMBER:
			if (lua_isinteger(L, index))
			{
				lua_Integer	v = lua_tointeger(L, index);
				if (v >= 10)
					snprintf(text, sizeof(text), "%lld (0x%llX)", (long long) v, (unsigned long long) v);
				else
					snprintf(text, sizeof(text), "%lld", (long long) v);
			}
			else
				snprintf(text, sizeof(text), "%.14g", lua_tonumber(L, index));
			return text;

		case LUA_TSTRING:
		{
			size_t		length;
			const char	*p = lua_tolstring(L, index, &length);
			return Quote(p, length);
		}

		case LUA_TTABLE:
		{
			int		count = 0;
			lua_pushnil(L);
			while (lua_next(L, index))
			{
				lua_pop(L, 1);
				if (++count >= 1000)
				{
					lua_pop(L, 1);
					break;
				}
			}
			if (count == 0)
				return "{}";
			snprintf(text, sizeof(text), "table (%d%s field%s)", count, count >= 1000 ? "+" : "", count == 1 ? "" : "s");
			return text;
		}

		case LUA_TFUNCTION:
		{
			lua_Debug	ar;
			lua_pushvalue(L, index);
			lua_getinfo(L, ">S", &ar);
			if (!strcmp(ar.what, "C"))
				return "function (C)";
			snprintf(text, sizeof(text), "function (%s:%d)", ar.short_src, ar.linedefined);
			return text;
		}

		case LUA_TUSERDATA:
		{
			std::string	name = "userdata";
			int		field = luaL_getmetafield(L, index, "__name");
			if (field != LUA_TNIL)
			{
				if (field == LUA_TSTRING)
					name = lua_tostring(L, -1);
				lua_pop(L, 1);
			}
			return name;
		}

		case LUA_TLIGHTUSERDATA:
			return "light userdata";

		case LUA_TTHREAD:
			return "thread";
	}
	return "?";
}

// For log messages: strings as they are, other values described.
std::string Text (lua_State *L, int index)
{
	if (lua_type(L, index) == LUA_TSTRING)
		return lua_tostring(L, index);
	return Describe(L, index);
}

int AddRef (lua_State *L, int index)
{
	index = lua_absindex(L, index);
	lua_rawgeti(L, LUA_REGISTRYINDEX, stop.refs);
	int		ref = (int) lua_rawlen(L, -1) + 1;
	lua_pushvalue(L, index);
	lua_rawseti(L, -2, ref);
	lua_pop(L, 1);
	return ref;
}

S9xLuaDebugVariable Variable (lua_State *L, int index, const std::string &name)
{
	S9xLuaDebugVariable	v;
	v.name = name;
	v.value = Describe(L, index, &v.type);
	if (lua_type(L, index) == LUA_TTABLE && stop.refs != LUA_NOREF)
		v.ref = AddRef(L, index);
	return v;
}

bool IsName (const char *p, size_t length)
{
	if (!length || !(isalpha((unsigned char) p[0]) || p[0] == '_'))
		return false;
	for (size_t i = 1; i < length; i++)
		if (!(isalnum((unsigned char) p[i]) || p[i] == '_'))
			return false;
	return true;
}

// The fields of the table at `index`: numbers in order, then names. A field
// equal to the same key in the table at `hide` is left out.
void ListFields (lua_State *L, int index, int hide, std::vector<S9xLuaDebugVariable> &out)
{
	index = lua_absindex(L, index);
	if (hide)
		hide = lua_absindex(L, hide);

	struct Field
	{
		bool		numeric;
		lua_Number	number;
		std::string	name;
		S9xLuaDebugVariable	var;
	};
	std::vector<Field>	fields;
	int		more = 0;

	lua_pushnil(L);
	while (lua_next(L, index))
	{
		if (hide)
		{
			lua_pushvalue(L, -2);
			lua_rawget(L, hide);
			bool	same = lua_rawequal(L, -1, -2);
			lua_pop(L, 1);
			if (same)
			{
				lua_pop(L, 1);
				continue;
			}
		}
		if ((int) fields.size() >= kMaxFields)
		{
			more++;
			lua_pop(L, 1);
			continue;
		}

		Field	f;
		f.numeric = lua_type(L, -2) == LUA_TNUMBER;
		f.number = f.numeric ? lua_tonumber(L, -2) : 0;
		if (lua_type(L, -2) == LUA_TSTRING)
		{
			size_t		length;
			const char	*p = lua_tolstring(L, -2, &length);
			f.name = IsName(p, length) ? std::string(p, length) : "[" + Quote(p, length) + "]";
		}
		else
			f.name = "[" + Describe(L, -2) + "]";
		f.var = Variable(L, -1, f.name);
		fields.push_back(std::move(f));
		lua_pop(L, 1);
	}

	std::stable_sort(fields.begin(), fields.end(), [](const Field &a, const Field &b) {
		if (a.numeric != b.numeric)
			return a.numeric;
		return a.numeric ? a.number < b.number : a.name < b.name;
	});
	for (auto &f : fields)
		out.push_back(std::move(f.var));
	if (more)
	{
		S9xLuaDebugVariable	v;
		v.name = "...";
		v.value = std::to_string(more) + " more";
		out.push_back(v);
	}
	if (lua_getmetatable(L, index))
	{
		out.push_back(Variable(L, -1, "(metatable)"));
		lua_pop(L, 1);
	}
}

//------------------------------------------------------------------------------
// Expressions

void EvalHook (lua_State *E, lua_Debug *)
{
	if (std::chrono::steady_clock::now() > eval_deadline)
		luaL_error(E, "the expression ran too long");
}

struct Evaluation
{
	lua_State	*host = nullptr;
	lua_State	*E = nullptr;
	int			ref = LUA_NOREF;
	int			results = 0;		// on E's stack, above the environment
	std::string	error;

	~Evaluation ()
	{
		if (host)
			luaL_unref(host, LUA_REGISTRYINDEX, ref);
	}
};

// Runs `expression` on a thread of its own, seeing the frame at `level` of T:
// its locals, then its upvalues, then its globals.
bool Evaluate (LuaScript *s, lua_State *T, int level, const std::string &expression, Evaluation &ev)
{
	ev.host = lua_status(T) == LUA_OK ? T : s->L;
	if (!lua_checkstack(T, 8) || !lua_checkstack(ev.host, 4))
	{
		ev.host = nullptr;
		ev.error = "no stack space left";
		return false;
	}
	ev.E = lua_newthread(ev.host);
	ev.ref = luaL_ref(ev.host, LUA_REGISTRYINDEX);
	lua_State	*E = ev.E;
	lua_sethook(E, EvalHook, LUA_MASKCOUNT, 1000);

	lua_newtable(E);			// 1: the names
	lua_pushglobaltable(E);		// 2: where the rest come from
	lua_Debug	ar;
	if (lua_getstack(T, level, &ar))
	{
		lua_getinfo(T, "f", &ar);
		for (int i = 1;; i++)
		{
			const char	*name = lua_getupvalue(T, -1, i);
			if (!name)
				break;
			lua_xmove(T, E, 1);
			if (!strcmp(name, "_ENV"))
				lua_replace(E, 2);
			else if (*name)
				lua_setfield(E, 1, name);
			else
				lua_pop(E, 1);
		}
		lua_pop(T, 1);
		for (int i = 1;; i++)
		{
			const char	*name = lua_getlocal(T, &ar, i);
			if (!name)
				break;
			lua_xmove(T, E, 1);
			if (name[0] != '(')
				lua_setfield(E, 1, name);
			else
				lua_pop(E, 1);
		}
	}
	lua_createtable(E, 0, 1);
	lua_insert(E, 2);
	lua_setfield(E, 2, "__index");
	lua_setmetatable(E, 1);

	// An expression, or failing that a statement (a call, say).
	std::string	chunk = "return " + expression;
	if (luaL_loadbufferx(E, chunk.data(), chunk.size(), "=expression", "t") != LUA_OK)
	{
		ev.error = lua_tostring(E, -1);
		lua_pop(E, 1);
		if (luaL_loadbufferx(E, expression.data(), expression.size(), "=expression", "t") != LUA_OK)
			return false;
		ev.error.clear();
	}
	lua_pushvalue(E, 1);
	lua_setupvalue(E, -2, 1);

	eval_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	s->evaluating = true;
	int		status = lua_pcall(E, 0, LUA_MULTRET, 0);
	s->evaluating = false;
	if (status != LUA_OK)
	{
		ev.error = lua_type(E, -1) == LUA_TSTRING ? lua_tostring(E, -1) : "error";
		return false;
	}
	ev.results = lua_gettop(E) - 1;
	if (!lua_checkstack(E, 20))
	{
		ev.error = "no stack space left";
		return false;
	}
	return true;
}

// "{expression}" in a log message, filled in.
std::string Interpolate (LuaScript *s, lua_State *L, const std::string &message)
{
	std::string	out;
	for (size_t i = 0; i < message.size();)
	{
		size_t	open = message.find('{', i);
		size_t	close = open == std::string::npos ? std::string::npos : message.find('}', open);
		if (close == std::string::npos)
		{
			out += message.substr(i);
			break;
		}
		out += message.substr(i, open - i);
		Evaluation	ev;
		if (Evaluate(s, L, 0, message.substr(open + 1, close - open - 1), ev))
			out += ev.results ? Text(ev.E, 2) : "nil";
		else
			out += "<" + ev.error + ">";
		i = close + 1;
	}
	return out;
}

bool HitConditionMet (const std::string &condition, int hits)
{
	const char	*p = condition.c_str();
	while (isspace((unsigned char) *p))
		p++;
	char	op = '=';
	if (!strncmp(p, ">=", 2))
		op = 'g', p += 2;
	else if (*p == '>')
		op = '>', p++;
	else if (!strncmp(p, "==", 2))
		p += 2;
	else if (*p == '%')
		op = '%', p++;

	char	*end;
	long	n = strtol(p, &end, 10);
	if (end == p)
		return true;
	switch (op)
	{
		case 'g':	return hits >= n;
		case '>':	return hits > n;
		case '%':	return n > 0 && hits % n == 0;
		default:	return hits == n;
	}
}

// A hit on an enabled breakpoint: true to stop.
bool Hit (LuaScript *s, lua_State *L, LuaBreakpoint &b, int line)
{
	if (!b.spec.condition.empty())
	{
		Evaluation	ev;
		if (!Evaluate(s, L, 0, b.spec.condition, ev))
		{
			LuaEmit(s->id, S9X_LUA_EVENT_WARNING, s->name + ":" + std::to_string(line) +
			        ": breakpoint condition failed: " + ev.error, line);
			return true;
		}
		if (!ev.results || !lua_toboolean(ev.E, 2))
			return false;
	}

	b.hits++;
	if (!b.spec.hit_condition.empty() && !HitConditionMet(b.spec.hit_condition, b.hits))
		return false;

	if (!b.spec.log_message.empty())
	{
		LuaFlushOutput(s);
		LuaEmit(s->id, S9X_LUA_EVENT_INFO, s->name + ":" + std::to_string(line) + ": " +
		        Interpolate(s, L, b.spec.log_message), line);
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------
// Stopping

void ListScope (int frame, int scope, std::vector<S9xLuaDebugVariable> &out)
{
	lua_State	*T = stop.thread;
	lua_Debug	ar;
	if (frame < 0 || frame >= stop.frames || !lua_getstack(T, stop.level + frame, &ar) || !lua_checkstack(T, 20))
		return;

	if (scope == 0)
	{
		for (int i = 1;; i++)
		{
			const char	*name = lua_getlocal(T, &ar, i);
			if (!name)
				break;
			if (name[0] != '(')
				out.push_back(Variable(T, -1, name));
			lua_pop(T, 1);
		}
		for (int i = -1;; i--)
		{
			const char	*name = lua_getlocal(T, &ar, i);
			if (!name)
				break;
			out.push_back(Variable(T, -1, "...[" + std::to_string(-i) + "]"));
			lua_pop(T, 1);
		}
		return;
	}

	lua_getinfo(T, "f", &ar);
	if (scope == 1)
	{
		for (int i = 1;; i++)
		{
			const char	*name = lua_getupvalue(T, -1, i);
			if (!name)
				break;
			if (*name && strcmp(name, "_ENV"))
				out.push_back(Variable(T, -1, name));
			lua_pop(T, 1);
		}
		lua_pop(T, 1);
		return;
	}

	// Globals: the frame's _ENV, less what every script starts with.
	bool	found = false;
	for (int i = 1;; i++)
	{
		const char	*name = lua_getupvalue(T, -1, i);
		if (!name)
			break;
		if (!strcmp(name, "_ENV"))
		{
			found = true;
			break;
		}
		lua_pop(T, 1);
	}
	if (!found)
		lua_pushglobaltable(T);
	lua_remove(T, -2);
	if (lua_type(T, -1) == LUA_TTABLE)
	{
		lua_rawgeti(T, LUA_REGISTRYINDEX, stop.script->baseline_ref);
		ListFields(T, -2, lua_type(T, -1) == LUA_TTABLE ? -1 : 0, out);
		lua_pop(T, 1);
	}
	lua_pop(T, 1);
}

void ListTable (int ref, std::vector<S9xLuaDebugVariable> &out)
{
	lua_State	*T = stop.thread;
	if (!lua_checkstack(T, 20))
		return;
	lua_rawgeti(T, LUA_REGISTRYINDEX, stop.refs);
	lua_rawgeti(T, -1, ref);
	if (lua_type(T, -1) == LUA_TTABLE)
		ListFields(T, -1, 0, out);
	lua_pop(T, 2);
}

S9xLuaDebugFrame Frame (LuaScript *s, lua_State *T, lua_Debug &ar)
{
	S9xLuaDebugFrame	f;
	lua_getinfo(T, "Sln", &ar);
	bool	c = !strcmp(ar.what, "C");
	if (c)
		f.function = ar.name ? std::string("[C] ") + ar.name : "[C]";
	else if (ar.name)
		f.function = ar.name;
	else if (!strcmp(ar.what, "main"))
		f.function = "main chunk";
	else
		f.function = "function (line " + std::to_string(ar.linedefined) + ")";

	f.line = ar.currentline > 0 ? ar.currentline : 0;
	f.own = OwnSource(s) == ar.source;
	if (c)
		f.source = "";
	else if (ar.source[0] == '@')
	{
		f.path = ar.source + 1;
		f.source = f.own ? s->name : f.path;
	}
	else if (ar.source[0] == '=')
		f.source = ar.source + 1;
	else
		f.source = "[string]";
	return f;
}

// T has stopped at `level`; serve the editor until it says to go on.
void StopHere (LuaScript *s, lua_State *T, int level, const char *reason, const std::string &message)
{
	if (!Available() || stop.script || !lua_checkstack(T, 20))
		return;

	S9xLuaDebugEvent	stopped;
	stopped.kind = S9X_LUA_DEBUG_STOPPED;
	stopped.script = s->id;
	stopped.reason = reason;
	stopped.message = message;
	lua_Debug	ar;
	for (int i = 0; i < kMaxFrames && lua_getstack(T, level + i, &ar); i++)
		stopped.frames.push_back(Frame(s, T, ar));

	stop.script = s;
	stop.thread = T;
	stop.level = level;
	stop.frames = (int) stopped.frames.size();
	lua_newtable(T);
	stop.refs = luaL_ref(T, LUA_REGISTRYINDEX);

	s->step = STEP_NONE;
	int		expected = s->id;
	break_request.compare_exchange_strong(expected, 0);
	{
		std::lock_guard<std::mutex>	lock(mutex);
		requests.clear();
	}
	stopped_script = s->id;
	Emit(std::move(stopped));
	LuaEng.frontend.debug_break(s->id, true);

	int		action = S9X_LUA_DEBUG_CONTINUE;
	for (bool done = false; !done;)
	{
		Request	r;
		bool	have = false;
		{
			std::unique_lock<std::mutex>	lock(mutex);
			if (requests.empty() && !LuaEng.frontend.debug_wait)
				wake.wait_for(lock, std::chrono::milliseconds(50));
			if (!requests.empty())
			{
				r = requests.front();
				requests.pop_front();
				have = true;
			}
		}
		if (!have)
		{
			if (LuaEng.frontend.debug_wait)
				LuaEng.frontend.debug_wait();
			continue;
		}
		if (r.script != s->id)
			continue;

		switch (r.kind)
		{
			case Request::ACTION:
				if (r.action != S9X_LUA_DEBUG_BREAK)
				{
					action = r.action;
					done = true;
				}
				break;

			case Request::VARIABLES:
			{
				S9xLuaDebugEvent	v;
				v.kind = S9X_LUA_DEBUG_VARIABLES;
				v.script = s->id;
				v.ref = r.ref;
				if (r.ref < 0)
					ListScope((-r.ref - 1) / 3, (-r.ref - 1) % 3, v.variables);
				else
					ListTable(r.ref, v.variables);
				Emit(std::move(v));
				break;
			}

			case Request::EVALUATE:
			{
				S9xLuaDebugEvent	v;
				v.kind = S9X_LUA_DEBUG_EVALUATED;
				v.script = s->id;
				v.request = r.id;
				S9xLuaDebugVariable	var;
				Evaluation	ev;
				if (r.frame < 0 || r.frame >= stop.frames)
				{
					v.failed = true;
					var.value = "no such frame";
				}
				else if (Evaluate(s, T, level + r.frame, r.expression, ev))
				{
					var = ev.results ? Variable(ev.E, 2, "") : S9xLuaDebugVariable();
					if (!ev.results)
						var.value = "(no value)";
					for (int i = 1; i < ev.results; i++)
						var.value += ", " + Describe(ev.E, 2 + i);
				}
				else
				{
					v.failed = true;
					var.value = ev.error;
				}
				var.name = r.expression;
				v.variables.push_back(var);
				Emit(std::move(v));
				break;
			}
		}
	}

	luaL_unref(T, LUA_REGISTRYINDEX, stop.refs);
	stop = Stop();
	stopped_script = 0;
	S9xLuaDebugEvent	resumed;
	resumed.kind = S9X_LUA_DEBUG_RESUMED;
	resumed.script = s->id;
	Emit(std::move(resumed));
	LuaEng.frontend.debug_break(s->id, false);
	LuaEng.call_started = std::chrono::steady_clock::now();	// the stop isn't the script's time

	switch (action)
	{
		case S9X_LUA_DEBUG_STEP_INTO:
		case S9X_LUA_DEBUG_STEP_OVER:
		case S9X_LUA_DEBUG_STEP_OUT:
			if (strcmp(reason, "error"))
			{
				s->step = action == S9X_LUA_DEBUG_STEP_INTO ? STEP_INTO : action == S9X_LUA_DEBUG_STEP_OVER ? STEP_OVER : STEP_OUT;
				s->step_thread = T;
				s->step_depth = Depth(T);
			}
			break;

		case S9X_LUA_DEBUG_STOP:
			s->interrupt = true;
			break;
	}
	ApplyHooks(s, T);
}

}	// namespace

//------------------------------------------------------------------------------
// Engine side

void LuaDebugInit (lua_State *L, LuaScript *s)
{
	lua_newtable(L);
	lua_createtable(L, 0, 1);
	lua_pushstring(L, "k");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L, -2);
	s->threads_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_getglobal(L, "coroutine");
	lua_getfield(L, -1, "create");
	lua_pushcclosure(L, co_create, 1);
	lua_setfield(L, -2, "create");
	lua_getfield(L, -1, "wrap");
	lua_pushcclosure(L, co_wrap, 1);
	lua_setfield(L, -2, "wrap");
	lua_pop(L, 1);
}

void LuaDebugCaptureGlobals (lua_State *L, LuaScript *s)
{
	lua_newtable(L);
	lua_pushglobaltable(L);
	lua_pushnil(L);
	while (lua_next(L, -2))
	{
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		lua_rawset(L, -5);
	}
	lua_pop(L, 1);
	s->baseline_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void LuaDebugPrepare (LuaScript *s)
{
	if (!Available() && !s->line_hook)
		return;
	SyncBreakpoints(s);
	ApplyHooks(s, s->L);
}

void LuaDebugCount (lua_State *L, LuaScript *s)
{
	if (!Available() || s->evaluating)
		return;
	SyncBreakpoints(s);
	if (WantsLines(s) != s->line_hook)
		ApplyHooks(s, L);
}

void LuaDebugLine (lua_State *L, lua_Debug *ar)
{
	LuaScript	*s = LuaScriptOf(L);
	if (!Available() || s->evaluating || s->interrupt || stop.script)
		return;
	SyncBreakpoints(s);

	const char	*reason = nullptr;
	if (break_request == s->id)
		reason = "break";
	else switch (s->step)
	{
		case STEP_INTO:
			reason = "step";
			break;
		case STEP_OVER:
			if (L == s->step_thread && Depth(L) <= s->step_depth)
				reason = "step";
			break;
		case STEP_OUT:
			if (L == s->step_thread && Depth(L) < s->step_depth)
				reason = "step";
			break;
	}

	int		line = ar->currentline;
	auto	it = s->breakpoints.find(line);
	if (it != s->breakpoints.end() && it->second.spec.enabled)
	{
		lua_getinfo(L, "S", ar);
		if (OwnSource(s) == ar->source && Hit(s, L, it->second, line) && !reason)
			reason = "breakpoint";
	}

	if (reason)
		StopHere(s, L, 0, reason, "");
	else if (!WantsLines(s))
		ApplyHooks(s, L);	// a finished step or a removed breakpoint: lines off again
}

bool LuaDebugError (LuaScript *s, lua_State *L, int level, const char *message)
{
	if (!Available() || !break_on_error || s->evaluating || stop.script)
		return false;
	StopHere(s, L, level, "error", message ? message : "");
	return s->interrupt;
}

bool LuaDebugStopped (void)
{
	return stop.script != nullptr;
}

void LuaDebugAbort (int script)
{
	int		expected = script;
	if (script)
		break_request.compare_exchange_strong(expected, 0);
	else
		break_request = 0;

	int		stopped = stopped_script;
	if (!stopped || (script && script != stopped))
		return;
	{
		std::lock_guard<std::mutex>	lock(mutex);
		Request	r;
		r.kind = Request::ACTION;
		r.script = stopped;
		r.action = S9X_LUA_DEBUG_STOP;
		requests.push_back(r);
	}
	wake.notify_all();
}

void LuaDebugForget (LuaScript *s)
{
	std::lock_guard<std::mutex>	lock(mutex);
	shared_breakpoints.erase(s->id);
}

//------------------------------------------------------------------------------
// Frontend API

bool S9xLuaDebugAvailable (void)
{
	return Available();
}

void S9xLuaDebugSetBreakpoints (int script, const std::vector<S9xLuaBreakpoint> &breakpoints)
{
	{
		std::lock_guard<std::mutex>	lock(mutex);
		if (breakpoints.empty())
			shared_breakpoints.erase(script);
		else
			shared_breakpoints[script] = breakpoints;
	}
	breakpoints_version++;
}

void S9xLuaDebugSetBreakOnError (bool enabled)
{
	break_on_error = enabled;
}

void S9xLuaDebugCommand (int script, S9xLuaDebugAction action)
{
	if (action == S9X_LUA_DEBUG_BREAK)
	{
		if (stopped_script != script)
			break_request = script;
		return;
	}
	{
		std::lock_guard<std::mutex>	lock(mutex);
		Request	r;
		r.kind = Request::ACTION;
		r.script = script;
		r.action = action;
		requests.push_back(r);
	}
	wake.notify_all();
}

int S9xLuaDebugStoppedScript (void)
{
	return stopped_script;
}

int S9xLuaDebugScopeRef (int frame, int scope)
{
	return -(frame * 3 + scope + 1);
}

void S9xLuaDebugRequestVariables (int script, int ref)
{
	{
		std::lock_guard<std::mutex>	lock(mutex);
		Request	r;
		r.kind = Request::VARIABLES;
		r.script = script;
		r.ref = ref;
		requests.push_back(r);
	}
	wake.notify_all();
}

void S9xLuaDebugEvaluate (int script, int frame, const std::string &expression, int request)
{
	{
		std::lock_guard<std::mutex>	lock(mutex);
		Request	r;
		r.kind = Request::EVALUATE;
		r.script = script;
		r.frame = frame;
		r.expression = expression;
		r.id = request;
		requests.push_back(r);
	}
	wake.notify_all();
}

bool S9xLuaDebugTakeEvents (std::vector<S9xLuaDebugEvent> &out)
{
	std::lock_guard<std::mutex>	lock(mutex);
	if (events.empty())
		return false;
	for (auto &e : events)
		out.push_back(std::move(e));
	events.clear();
	return true;
}
