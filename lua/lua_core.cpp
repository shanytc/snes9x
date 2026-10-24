/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Lua itself (external/lua), built as one C++ translation unit the way the
// upstream onelua.c builds it. As C++, Lua errors unwind with exceptions, so
// the destructors in the scripting API run.

#if !defined(_WIN32)
#define LUA_USE_POSIX
#endif

#include "lprefix.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LUA_CORE
#define LUA_LIB
#define ltable_c
#define lvm_c
#include "luaconf.h"

// C modules are built against C Lua and can't link to this one.
#undef LUA_DL_DLL
#undef LUA_USE_DLOPEN

// Keep the internals private, as onelua.c does.
#undef LUAI_FUNC
#undef LUAI_DDEC
#undef LUAI_DDEF
#define LUAI_FUNC	static
#define LUAI_DDEC(def)	/* empty */
#define LUAI_DDEF	static

#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"

// Only Lua source loads: precompiled chunks aren't verified and a crafted one
// corrupts memory. Every loader (load, dofile, require) parses through here.
static LClosure *RejectBinaryChunk (lua_State *L)
{
	luaO_pushfstring(L, "attempt to load a binary chunk (scripts load Lua source only)");
	luaD_throw(L, LUA_ERRSYNTAX);
	return NULL;
}
#define luaU_undump(L, Z, name)	RejectBinaryChunk(L)
#include "ldo.c"
#undef luaU_undump
#include "lvm.c"
#include "lapi.c"

#include "lauxlib.c"

#include "lbaselib.c"
#include "lcorolib.c"
#include "ldblib.c"
#include "linit.c"
#include "liolib.c"
#include "lmathlib.c"
#include "loadlib.c"
#include "loslib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
