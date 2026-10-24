/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The 65816's opcode handlers built a second time with the Lua memory hooks in
// their bus accesses, as sa1cpu.cpp builds them for the SA-1. The CPU switches
// to these tables only while a script has a memory hook.

#include <array>
#include <utility>

#include "snes9x.h"
#include "memmap.h"
#include "getset.h"
#include "lua/lua_hooks.h"

namespace
{

// The normal tables for the current mode, before the names below change.
void FixCyclesNormal (void)
{
	S9xFixCycles();
}

inline uint8 HookedGetByte (uint32 address)
{
	uint8	value = S9xGetByte(address);
	if (S9xLuaHookMask & S9X_LUA_HOOK_READ)
		value = S9xLuaHookRead(address, value);
	return value;
}

inline uint16 HookedGetWord (uint32 address, enum s9xwrap_t w = WRAP_NONE)
{
	uint16	value = S9xGetWord(address, w);
	if (S9xLuaHookMask & S9X_LUA_HOOK_READ)
		value = S9xLuaHookReadWord(address, w, value);
	return value;
}

inline void HookedSetByte (uint8 value, uint32 address)
{
	if (S9xLuaHookMask & S9X_LUA_HOOK_WRITE)
		value = S9xLuaHookWrite(address, value);
	S9xSetByte(value, address);
}

inline void HookedSetWord (uint16 value, uint32 address, enum s9xwrap_t w = WRAP_NONE, enum s9xwriteorder_t o = WRITE_01)
{
	if (S9xLuaHookMask & S9X_LUA_HOOK_WRITE)
		value = S9xLuaHookWriteWord(address, w, value);
	S9xSetWord(value, address, w, o);
}

void FixCyclesHooked (void);

}	// namespace

#define S9xGetByte		HookedGetByte
#define S9xGetWord		HookedGetWord
#define S9xSetByte		HookedSetByte
#define S9xSetWord		HookedSetWord
#define S9xFixCycles	FixCyclesHooked
#define S9xOpcode_NMI	S9xLuaOpcode_NMI
#define S9xOpcode_IRQ	S9xLuaOpcode_IRQ
#define S9xOpcodesM1X1	S9xLuaOpcodesM1X1
#define S9xOpcodesM1X0	S9xLuaOpcodesM1X0
#define S9xOpcodesM0X1	S9xLuaOpcodesM0X1
#define S9xOpcodesM0X0	S9xLuaOpcodesM0X0
#define S9xOpcodesE1	S9xLuaOpcodesE1
#define S9xOpcodesSlow	S9xLuaOpcodesSlow
#include "cpuops.cpp"

namespace
{

uint32	refetch_at = 0xffffffff;	// a hook patched this opcode: run it without asking again

// The loop has already fetched the opcode, so PC steps back for the hook, which
// may move it to skip the instruction or patch the opcode about to run.
template <struct SOpcodes *table, int op, bool fast>
void ExecOp (void)
{
	Registers.PCw--;
	uint32	at = Registers.PBPC;
	if (at == refetch_at)
		refetch_at = 0xffffffff;
	else
	{
		S9xLuaHookExec(at);
		if (Registers.PBPC != at)
			return;
		if (fast && CPU.PCBase && CPU.PCBase[Registers.PCw] != op)
		{
			refetch_at = at;
			return;
		}
	}
	Registers.PCw++;
	table[op].S9xOpcode();
}

template <struct SOpcodes *table, bool fast, size_t... op>
constexpr std::array<struct SOpcodes, 256> ExecTable (std::index_sequence<op...>)
{
	return {{ { ExecOp<table, (int) op, fast> }... }};
}

template <struct SOpcodes *table, bool fast>
std::array<struct SOpcodes, 256>	exec_table = ExecTable<table, fast>(std::make_index_sequence<256>());

struct SOpcodes	*const tables[2][5] =
{
	{ S9xLuaOpcodesE1, S9xLuaOpcodesM1X1, S9xLuaOpcodesM1X0, S9xLuaOpcodesM0X1, S9xLuaOpcodesM0X0 },
	{
		exec_table<S9xLuaOpcodesE1, true>.data(), exec_table<S9xLuaOpcodesM1X1, true>.data(),
		exec_table<S9xLuaOpcodesM1X0, true>.data(), exec_table<S9xLuaOpcodesM0X1, true>.data(),
		exec_table<S9xLuaOpcodesM0X0, true>.data()
	}
};

void FixCyclesHooked (void)
{
	FixCyclesNormal();	// the mode's instruction lengths
	int	mode = CheckEmulation() ? 0 : CheckMemory() ? (CheckIndex() ? 1 : 2) : (CheckIndex() ? 3 : 4);
	ICPU.S9xOpcodes = tables[(S9xLuaHookMask & S9X_LUA_HOOK_EXEC) != 0][mode];
}

}	// namespace

void S9xLuaSelectOpcodes (void)
{
	if (S9xLuaHookMask)
		FixCyclesHooked();
	else
		FixCyclesNormal();
}

struct SOpcodes *S9xLuaSlowOpcodes (void)
{
	if (S9xLuaHookMask & S9X_LUA_HOOK_EXEC)
		return exec_table<S9xLuaOpcodesSlow, false>.data();
	return S9xLuaOpcodesSlow;
}
