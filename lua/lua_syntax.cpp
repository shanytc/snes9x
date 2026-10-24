/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Editor support shared by the ports: a Lua tokenizer for syntax colouring
// and the list of API names for completion.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#include "lua/lua_internal.h"

namespace
{

const char	*keywords[] =
{
	"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
	"local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
};

const char	*modules[] =
{
	"emu", "cpu", "memory", "gui", "joypad", "savestate", "event", "movie", "console", "script"
};

const char	*builtins[] =
{
	"assert", "collectgarbage", "coroutine", "debug", "dofile", "error", "getmetatable", "io",
	"ipairs", "load", "loadfile", "math", "next", "os", "package", "pairs", "pcall", "print",
	"rawequal", "rawget", "rawlen", "rawset", "require", "select", "setmetatable", "string",
	"table", "tonumber", "tostring", "type", "utf8", "xpcall", "self", "_G", "_ENV"
};

bool InList (const char *const *list, size_t count, const char *word, size_t length)
{
	for (size_t i = 0; i < count; i++)
		if (strlen(list[i]) == length && !memcmp(list[i], word, length))
			return true;
	return false;
}

#define IN_LIST(list, word, length) InList(list, sizeof(list) / sizeof(list[0]), word, length)

bool IsIdentStart (char c)
{
	return isalpha((unsigned char) c) || c == '_';
}

bool IsIdent (char c)
{
	return isalnum((unsigned char) c) || c == '_';
}

// At text[i] == '[': the level of a long bracket "[==[" or -1.
int LongBracketLevel (const char *text, int length, int i)
{
	int	j = i + 1, level = 0;
	while (j < length && text[j] == '=')
	{
		j++;
		level++;
	}
	return j < length && text[j] == '[' ? level : -1;
}

// Scans for "]==]" of `level` from i; returns the index after it, or -1.
int CloseLongBracket (const char *text, int length, int i, int level)
{
	for (; i < length; i++)
	{
		if (text[i] != ']')
			continue;
		int	j = i + 1, n = 0;
		while (j < length && text[j] == '=')
		{
			j++;
			n++;
		}
		if (n == level && j < length && text[j] == ']')
			return j + 1;
	}
	return -1;
}

}	// namespace

// state: 0 = normal; otherwise (level + 1) of an open long bracket, plus
// 0x1000 when that bracket is a comment.
int S9xLuaTokenize (const char *text, int length, int state, std::vector<S9xLuaToken> &tokens)
{
	int	i = 0;

	if (state > 0)
	{
		int		level = (state & 0xfff) - 1;
		S9xLuaTokenKind	kind = (state & 0x1000) ? S9X_LUA_TOKEN_COMMENT : S9X_LUA_TOKEN_STRING;
		int		end = CloseLongBracket(text, length, 0, level);
		if (end < 0)
		{
			tokens.push_back({ 0, length, kind });
			return state;
		}
		tokens.push_back({ 0, end, kind });
		i = end;
	}

	bool	after_module = false;	// the previous tokens were "<module>."
	while (i < length)
	{
		char	c = text[i];
		int		start = i;

		if (c == '-' && i + 1 < length && text[i + 1] == '-')
		{
			int	level = i + 2 < length && text[i + 2] == '[' ? LongBracketLevel(text, length, i + 2) : -1;
			if (level >= 0)
			{
				int	end = CloseLongBracket(text, length, i + 4 + level, level);
				if (end < 0)
				{
					tokens.push_back({ start, length - start, S9X_LUA_TOKEN_COMMENT });
					return (level + 1) | 0x1000;
				}
				tokens.push_back({ start, end - start, S9X_LUA_TOKEN_COMMENT });
				i = end;
				continue;
			}
			while (i < length && text[i] != '\n')
				i++;
			tokens.push_back({ start, i - start, S9X_LUA_TOKEN_COMMENT });
			continue;
		}

		if (c == '[')
		{
			int	level = LongBracketLevel(text, length, i);
			if (level >= 0)
			{
				int	end = CloseLongBracket(text, length, i + 2 + level, level);
				if (end < 0)
				{
					tokens.push_back({ start, length - start, S9X_LUA_TOKEN_STRING });
					return level + 1;
				}
				tokens.push_back({ start, end - start, S9X_LUA_TOKEN_STRING });
				i = end;
				continue;
			}
		}

		if (c == '"' || c == '\'')
		{
			i++;
			while (i < length && text[i] != c && text[i] != '\n')
				i += text[i] == '\\' && i + 1 < length ? 2 : 1;
			if (i < length && text[i] == c)
				i++;
			tokens.push_back({ start, i - start, S9X_LUA_TOKEN_STRING });
			after_module = false;
			continue;
		}

		if (isdigit((unsigned char) c) || (c == '.' && i + 1 < length && isdigit((unsigned char) text[i + 1])))
		{
			bool	hex = c == '0' && i + 1 < length && (text[i + 1] == 'x' || text[i + 1] == 'X');
			i += hex ? 2 : 0;
			while (i < length)
			{
				char	d = text[i];
				bool	exponent = hex ? (d == 'p' || d == 'P') : (d == 'e' || d == 'E');
				if (exponent && i + 1 < length && (text[i + 1] == '+' || text[i + 1] == '-'))
					i += 2;
				else if (isalnum((unsigned char) d) || d == '.')
					i++;
				else
					break;
			}
			tokens.push_back({ start, i - start, S9X_LUA_TOKEN_NUMBER });
			after_module = false;
			continue;
		}

		if (IsIdentStart(c))
		{
			while (i < length && IsIdent(text[i]))
				i++;
			const char	*word = text + start;
			int			n = i - start;
			bool		member = start > 0 && text[start - 1] == '.' && (start < 2 || text[start - 2] != '.');

			if (!member && IN_LIST(keywords, word, n))
				tokens.push_back({ start, n, S9X_LUA_TOKEN_KEYWORD });
			else if (after_module || (!member && (IN_LIST(modules, word, n) || IN_LIST(builtins, word, n))))
				tokens.push_back({ start, n, S9X_LUA_TOKEN_BUILTIN });

			after_module = !member && IN_LIST(modules, word, n) && i < length && text[i] == '.';
			continue;
		}

		if (c != '.')
			after_module = false;
		i++;
	}

	return 0;
}

const std::vector<std::string> &S9xLuaApiNames (void)
{
	static std::vector<std::string>	names;
	static std::once_flag	once;

	std::call_once(once, [] {
		lua_State	*L = luaL_newstate();
		if (!L)
			return;
		LuaOpenEmu(L);
		LuaOpenMemory(L);
		LuaOpenGui(L);
		LuaOpenInput(L);

		for (const char *module : modules)
		{
			names.push_back(module);
			lua_getglobal(L, module);
			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				if (lua_type(L, -2) == LUA_TSTRING && (lua_isfunction(L, -1) || lua_istable(L, -1)))
				{
					std::string	name = std::string(module) + "." + lua_tostring(L, -2);
					names.push_back(name);
					// memory.wram and the other domains: their functions too.
					if (lua_istable(L, -1))
					{
						lua_pushnil(L);
						while (lua_next(L, -2))
						{
							if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1))
								names.push_back(name + "." + lua_tostring(L, -2));
							lua_pop(L, 1);
						}
					}
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_close(L);
		std::sort(names.begin(), names.end());
	});

	return names;
}
