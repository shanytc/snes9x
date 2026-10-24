/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _LUA_HOOKS_H_
#define _LUA_HOOKS_H_

#include "port.h"

// The emulation core's side of Lua scripting. All of it runs on the
// emulation thread; with no script loaded it costs a byte test per frame.

enum
{
	S9X_LUA_HOOK_READ  = 1,
	S9X_LUA_HOOK_WRITE = 2,
	S9X_LUA_HOOK_EXEC  = 4
};

extern uint8	S9xLuaActive;	// a script is loaded: frame hooks run
extern uint8	S9xLuaHookMask;	// S9X_LUA_HOOK_* kinds with a live memory hook

// Top and bottom of S9xMainLoop: one call each per emulated frame, plus one
// once S9xMovieUpdate has settled the frame's input.
void S9xLuaFrameStart (void);
void S9xLuaInputLatched (void);
void S9xLuaFrameEnd (void);

// S9xEndScreenRefresh asks before presenting; true means the engine draws its
// overlay and presents the frame itself from S9xLuaFrameEnd. `partial` is an
// interlaced frame's first field, which goes out through S9xContinueUpdate.
bool S9xLuaDeferPresent (bool partial = false);

// While S9xLuaHookMask is set the 65816 runs a hooked copy of its opcode
// handlers (lua_cpuops.cpp), so the normal ones carry no Lua code at all.
struct SOpcodes;
void S9xLuaSelectOpcodes (void);			// after S9xLuaHookMask or the CPU state changes
struct SOpcodes *S9xLuaSlowOpcodes (void);	// the hooked S9xOpcodesSlow
void S9xLuaOpcode_NMI (void);
void S9xLuaOpcode_IRQ (void);

uint8 S9xLuaHookRead (uint32 address, uint8 value);
uint8 S9xLuaHookWrite (uint32 address, uint8 value);
uint16 S9xLuaHookReadWord (uint32 address, int wrap, uint16 value);
uint16 S9xLuaHookWriteWord (uint32 address, int wrap, uint16 value);
void S9xLuaHookExec (uint32 address);

// Lifecycle notices; safe from any thread, delivered to scripts at frame start.
void S9xLuaNotifyReset (bool power);
void S9xLuaNotifyStateSaved (const char *filename);
void S9xLuaNotifyStateLoaded (const char *filename);
void S9xLuaNotifyRomLoaded (void);

#endif
