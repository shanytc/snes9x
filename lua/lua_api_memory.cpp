/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The memory module: named regions ("domains") read and written without the
// side effects or bus timing a CPU access would have, plus memory hooks.

#include <cstring>

#include "lua/lua_internal.h"
#include "memmap.h"
#include "ppu.h"
#include "apu/apu.h"
#include "sgb/sgb.h"

namespace
{

struct Domain
{
	const char	*name;
	uint32		(*size) (void);
	uint8		(*read) (uint32);
	void		(*write) (uint32, uint8);	// nullptr: read-only
};

bool GameBoyMode (void)
{
	// As the cheat engine: with or without the SGB BIOS, a GB game's memory.
	return Settings.SuperGameBoy || Settings.SGB_BIOSModeActive;
}

//------------------------------------------------------------------------------
// SNES

uint8 *SRAMPointer (uint32 address)
{
	int		block = (address & 0xffffff) >> MEMMAP_SHIFT;
	uint8	*p = Memory.Map[block];

	switch ((pint) p)
	{
		case CMemory::MAP_LOROM_SRAM:
		case CMemory::MAP_SA1RAM:
			return Memory.SRAM + ((((address & 0xff0000) >> 1) | (address & 0x7fff)) & Memory.SRAMMask);
		case CMemory::MAP_LOROM_SRAM_B:
			return Multi.sramB + ((((address & 0xff0000) >> 1) | (address & 0x7fff)) & Multi.sramMaskB);
		case CMemory::MAP_HIROM_SRAM:
		case CMemory::MAP_RONLY_SRAM:
			return Memory.SRAM + (((address & 0x7fff) - 0x6000 + ((address & 0x1f0000) >> 3)) & Memory.SRAMMask);
		case CMemory::MAP_BWRAM:
			return Memory.BWRAM + ((address & 0x7fff) - 0x6000);
		default:
			return nullptr;
	}
}

uint32 BusSize (void)	{ return 0x1000000; }

uint8 BusRead (uint32 address)
{
	address &= 0xffffff;
	uint8	*p = Memory.Map[address >> MEMMAP_SHIFT];
	if (p >= (uint8 *) CMemory::MAP_LAST)
		return p[address & 0xffff];

	if ((pint) p == CMemory::MAP_CPU || (pint) p == CMemory::MAP_PPU)
		return Memory.FillRAM[address & 0x7fff];	// the last value, not a live read

	if (uint8 *sram = SRAMPointer(address))
		return *sram;
	return OpenBus;
}

void BusWrite (uint32 address, uint8 value)
{
	address &= 0xffffff;
	uint8	*p = Memory.WriteMap[address >> MEMMAP_SHIFT];
	if (p >= (uint8 *) CMemory::MAP_LAST)
	{
		p[address & 0xffff] = value;
		return;
	}

	switch ((pint) p)
	{
		case CMemory::MAP_LOROM_SRAM:
		case CMemory::MAP_HIROM_SRAM:
		case CMemory::MAP_LOROM_SRAM_B:
		case CMemory::MAP_BWRAM:
			if (uint8 *sram = SRAMPointer(address))
			{
				*sram = value;
				CPU.SRAMModified = TRUE;
			}
			break;

		case CMemory::MAP_SA1RAM:
			Memory.SRAM[address & 0xffff] = value;
			break;

		default:
			break;	// ROM, I/O registers and coprocessors stay untouched
	}
}

uint32 WRAMSize (void)			{ return 0x20000; }
uint8 WRAMRead (uint32 a)		{ return Memory.RAM[a]; }
void WRAMWrite (uint32 a, uint8 v)	{ Memory.RAM[a] = v; }

uint32 SRAMSize (void)
{
	// A garbage header byte (Super FX hacks read $7FBD raw) mustn't reach past the buffer.
	if (!Memory.SRAMSize)
		return 0;
	return Memory.SRAMSize >= 9 ? (uint32) Memory.SRAM_SIZE : (1 << (Memory.SRAMSize + 3)) * 128;
}
uint8 SRAMRead (uint32 a)		{ return Memory.SRAM[a]; }
void SRAMWrite (uint32 a, uint8 v)
{
	Memory.SRAM[a] = v;
	CPU.SRAMModified = TRUE;
}

uint32 ROMSize (void)			{ return Memory.CalculatedSize; }
uint8 ROMRead (uint32 a)		{ return Memory.ROM[a]; }
void ROMWrite (uint32 a, uint8 v)	{ Memory.ROM[a] = v; }

uint32 VRAMSize (void)			{ return 0x10000; }
uint8 VRAMRead (uint32 a)		{ return Memory.VRAM[a]; }
void VRAMWrite (uint32 a, uint8 v)
{
	Memory.VRAM[a] = v;
	// As REGISTER_2118: drop every decoded tile this byte belongs to.
	IPPU.TileCached[TILE_2BIT][a >> 4] = FALSE;
	IPPU.TileCached[TILE_4BIT][a >> 5] = FALSE;
	IPPU.TileCached[TILE_8BIT][a >> 6] = FALSE;
	IPPU.TileCached[TILE_2BIT_EVEN][a >> 4] = FALSE;
	IPPU.TileCached[TILE_2BIT_EVEN][((a >> 4) - 1) & (MAX_2BIT_TILES - 1)] = FALSE;
	IPPU.TileCached[TILE_2BIT_ODD][a >> 4] = FALSE;
	IPPU.TileCached[TILE_2BIT_ODD][((a >> 4) - 1) & (MAX_2BIT_TILES - 1)] = FALSE;
	IPPU.TileCached[TILE_4BIT_EVEN][a >> 5] = FALSE;
	IPPU.TileCached[TILE_4BIT_EVEN][((a >> 5) - 1) & (MAX_4BIT_TILES - 1)] = FALSE;
	IPPU.TileCached[TILE_4BIT_ODD][a >> 5] = FALSE;
	IPPU.TileCached[TILE_4BIT_ODD][((a >> 5) - 1) & (MAX_4BIT_TILES - 1)] = FALSE;
}

uint32 OAMSize (void)			{ return 512 + 32; }
uint8 OAMRead (uint32 a)		{ return PPU.OAMData[a]; }
void OAMWrite (uint32 a, uint8 v)
{
	PPU.OAMData[a] = v;
	IPPU.OBJChanged = TRUE;

	// Keep PPU.OBJ, the decoded copy the renderer draws from, in step.
	if (a >= 512)
	{
		for (int i = 0; i < 4; i++)
		{
			struct SOBJ	*obj = &PPU.OBJ[(a & 0x1f) * 4 + i];
			obj->HPos = (obj->HPos & 0xff) | SignExtend[(v >> (i * 2)) & 1];
			obj->Size = v & (2 << (i * 2));
		}
		return;
	}

	struct SOBJ	*obj = &PPU.OBJ[a >> 2];
	uint8		*entry = &PPU.OAMData[a & ~3];
	obj->HPos = (obj->HPos & 0xff00) | entry[0];
	obj->VPos = entry[1];
	obj->Name = entry[2] | ((entry[3] & 1) << 8);
	obj->Palette = (entry[3] >> 1) & 7;
	obj->Priority = (entry[3] >> 4) & 3;
	obj->HFlip = (entry[3] >> 6) & 1;
	obj->VFlip = (entry[3] >> 7) & 1;
}

uint32 CGRAMSize (void)			{ return 512; }
uint8 CGRAMRead (uint32 a)		{ return (uint8) (PPU.CGDATA[a >> 1] >> ((a & 1) * 8)); }
void CGRAMWrite (uint32 a, uint8 v)
{
	int		index = a >> 1;
	uint16	color = PPU.CGDATA[index];
	color = (a & 1) ? ((color & 0x00ff) | ((v & 0x7f) << 8)) : ((color & 0xff00) | v);
	PPU.CGDATA[index] = color;

	// As REGISTER_2122: the renderer's own copy of the colour.
	uint8	r = IPPU.XB[color & 0x1f];
	uint8	g = IPPU.XB[(color >> 5) & 0x1f];
	uint8	b = IPPU.XB[(color >> 10) & 0x1f];
	S9xApplyColorAdjustments(r, g, b, 0x1f);
	IPPU.Red[index] = r;
	IPPU.Green[index] = g;
	IPPU.Blue[index] = b;
	IPPU.ScreenColors[index] = (uint16) BUILD_PIXEL(r, g, b);
	IPPU.ColorsChanged = TRUE;
}

uint32 ARAMSize (void)			{ return 0x10000; }
uint8 ARAMRead (uint32 a)		{ return S9xGetAPURAM()[a]; }
void ARAMWrite (uint32 a, uint8 v)	{ S9xGetAPURAM()[a] = v; }

const Domain	snes_domains[] =
{
	{ "bus",	BusSize,	BusRead,	BusWrite },
	{ "wram",	WRAMSize,	WRAMRead,	WRAMWrite },
	{ "sram",	SRAMSize,	SRAMRead,	SRAMWrite },
	{ "rom",	ROMSize,	ROMRead,	ROMWrite },
	{ "vram",	VRAMSize,	VRAMRead,	VRAMWrite },
	{ "oam",	OAMSize,	OAMRead,	OAMWrite },
	{ "cgram",	CGRAMSize,	CGRAMRead,	CGRAMWrite },
	{ "aram",	ARAMSize,	ARAMRead,	ARAMWrite }
};

//------------------------------------------------------------------------------
// Game Boy

uint32 GBBusSize (void)				{ return 0x10000; }
uint8 GBBusRead (uint32 a)			{ return S9xSGBCheatRead(a); }
void GBBusWrite (uint32 a, uint8 v)	{ S9xSGBCheatWrite(a, v); }

template <unsigned char *(*region) (unsigned int *)>
uint32 GBRegionSize (void)
{
	unsigned int	size = 0;
	return region(&size) ? size : 0;
}

template <unsigned char *(*region) (unsigned int *)>
uint8 GBRegionRead (uint32 a)
{
	unsigned int	size = 0;
	unsigned char	*p = region(&size);
	return p && a < size ? p[a] : 0;
}

template <unsigned char *(*region) (unsigned int *)>
void GBRegionWrite (uint32 a, uint8 v)
{
	unsigned int	size = 0;
	unsigned char	*p = region(&size);
	if (p && a < size)
		p[a] = v;
}

uint32 GBROMSize (void)
{
	unsigned int	size = 0;
	return S9xSGBCheatROMPtr(&size) ? size : 0;
}
uint8 GBROMRead (uint32 a)
{
	unsigned int	size = 0;
	const unsigned char	*p = S9xSGBCheatROMPtr(&size);
	return p && a < size ? p[a] : 0;
}

uint32 GBVRAMSize (void)		{ return S9xSGBGetVRAM() ? 0x4000 : 0; }
uint8 GBVRAMRead (uint32 a)		{ return S9xSGBGetVRAM()[a]; }
uint32 GBOAMSize (void)			{ return S9xSGBGetOAM() ? 0xa0 : 0; }
uint8 GBOAMRead (uint32 a)		{ return S9xSGBGetOAM()[a]; }

const Domain	gb_domains[] =
{
	{ "bus",	GBBusSize,	GBBusRead,	GBBusWrite },
	{ "wram",	GBRegionSize<S9xSGBCheatWRAMPtr>,	GBRegionRead<S9xSGBCheatWRAMPtr>,	GBRegionWrite<S9xSGBCheatWRAMPtr> },
	{ "sram",	GBRegionSize<S9xSGBCheatSRAMPtr>,	GBRegionRead<S9xSGBCheatSRAMPtr>,	GBRegionWrite<S9xSGBCheatSRAMPtr> },
	{ "hram",	GBRegionSize<S9xSGBCheatHRAMPtr>,	GBRegionRead<S9xSGBCheatHRAMPtr>,	GBRegionWrite<S9xSGBCheatHRAMPtr> },
	{ "rom",	GBROMSize,	GBROMRead,	nullptr },
	{ "vram",	GBVRAMSize,	GBVRAMRead,	nullptr },
	{ "oam",	GBOAMSize,	GBOAMRead,	nullptr }
};

const char	*all_domain_names[] = { "bus", "wram", "sram", "rom", "vram", "oam", "cgram", "aram", "hram" };

const Domain *FindDomain (const char *name)
{
	if (GameBoyMode())
	{
		for (auto &d : gb_domains)
			if (!strcmp(d.name, name))
				return &d;
	}
	else
	{
		for (auto &d : snes_domains)
			if (!strcmp(d.name, name))
				return &d;
	}
	return nullptr;
}

// The domain comes from the closure of memory.<domain>.fn, else from the
// optional argument at `arg` (default "bus").
const Domain *CheckDomain (lua_State *L, int arg)
{
	const char	*name = lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING
	                    ? lua_tostring(L, lua_upvalueindex(1))
	                    : luaL_optstring(L, arg, "bus");
	LuaRequireGame(L);
	const Domain	*d = FindDomain(name);
	if (!d)
	{
		bool	known = false;
		for (const char *n : all_domain_names)
			known = known || !strcmp(n, name);
		if (known)
			luaL_error(L, "memory domain '%s' isn't available for %s", name, GameBoyMode() ? "a Game Boy game" : "a SNES game");
		luaL_error(L, "no memory domain named '%s'", name);
	}
	return d;
}

uint32 CheckRange (lua_State *L, int arg, const Domain *d, lua_Integer count)
{
	lua_Integer	address = luaL_checkinteger(L, arg);
	lua_Integer	size = d->size();
	if (d->read == BusRead)
	{
		// The bus wraps, but one call never reads more than all of it.
		if (count > size)
			luaL_error(L, "%I bytes is more than the whole bus (0x1000000 bytes)", count);
		return (uint32) (address & 0xffffff);
	}
	// Written so a huge address or count can't overflow past the check.
	if (address < 0 || count < 0 || count > size || address > size - count)
	{
		char	text[160];
		snprintf(text, sizeof(text), "address 0x%llX is outside '%s' (0x%X bytes)",
		         (unsigned long long) address, d->name, (unsigned) size);
		luaL_error(L, "%s", text);
	}
	return (uint32) address;
}

uint32 Next (const Domain *d, uint32 address, int i)
{
	return d->read == BusRead ? (address + i) & 0xffffff : address + i;
}

template <int bytes, bool is_signed, bool big_endian>
int mem_read (lua_State *L)
{
	const Domain	*d = CheckDomain(L, 2);
	uint32	address = CheckRange(L, 1, d, bytes);
	uint32	value = 0;
	for (int i = 0; i < bytes; i++)
	{
		int	shift = big_endian ? (bytes - 1 - i) * 8 : i * 8;
		value |= (uint32) d->read(Next(d, address, i)) << shift;
	}

	lua_Integer	result = value;
	if (is_signed && (value & (1u << (bytes * 8 - 1))))
		result -= (lua_Integer) 1 << (bytes * 8);
	lua_pushinteger(L, result);
	return 1;
}

void RequireWritable (lua_State *L, const Domain *d)
{
	if (!d->write)
		luaL_error(L, "memory domain '%s' is read-only", d->name);
}

template <int bytes, bool big_endian>
int mem_write (lua_State *L)
{
	const Domain	*d = CheckDomain(L, 3);
	RequireWritable(L, d);
	uint32	address = CheckRange(L, 1, d, bytes);
	uint32	value = (uint32) luaL_checkinteger(L, 2);
	for (int i = 0; i < bytes; i++)
	{
		int	shift = big_endian ? (bytes - 1 - i) * 8 : i * 8;
		d->write(Next(d, address, i), (uint8) (value >> shift));
	}
	return 0;
}

int mem_read_bytes (lua_State *L)
{
	const Domain	*d = CheckDomain(L, 3);
	lua_Integer	count = luaL_checkinteger(L, 2);
	uint32	address = CheckRange(L, 1, d, count);
	lua_createtable(L, (int) std::max<lua_Integer>(count, 0), 0);
	for (lua_Integer i = 0; i < count; i++)
	{
		lua_pushinteger(L, d->read(Next(d, address, (int) i)));
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

int mem_read_string (lua_State *L)
{
	const Domain	*d = CheckDomain(L, 3);
	lua_Integer	count = luaL_checkinteger(L, 2);
	uint32	address = CheckRange(L, 1, d, count);
	luaL_Buffer	b;
	luaL_buffinit(L, &b);
	for (lua_Integer i = 0; i < count; i++)
		luaL_addchar(&b, (char) d->read(Next(d, address, (int) i)));
	luaL_pushresult(&b);
	return 1;
}

int mem_write_bytes (lua_State *L)
{
	const Domain	*d = CheckDomain(L, 3);
	RequireWritable(L, d);

	if (lua_type(L, 2) == LUA_TSTRING)
	{
		size_t		length;
		const char	*data = lua_tolstring(L, 2, &length);
		uint32		address = CheckRange(L, 1, d, (lua_Integer) length);
		for (size_t i = 0; i < length; i++)
			d->write(Next(d, address, (int) i), (uint8) data[i]);
		return 0;
	}

	luaL_checktype(L, 2, LUA_TTABLE);
	lua_Integer	length = luaL_len(L, 2);
	uint32		address = CheckRange(L, 1, d, length);
	for (lua_Integer i = 0; i < length; i++)
	{
		lua_geti(L, 2, i + 1);
		d->write(Next(d, address, (int) i), (uint8) luaL_checkinteger(L, -1));
		lua_pop(L, 1);
	}
	return 0;
}

int mem_size (lua_State *L)
{
	lua_pushinteger(L, CheckDomain(L, 1)->size());
	return 1;
}

int mem_domains (lua_State *L)
{
	lua_newtable(L);
	if (!LuaGameLoaded())
		return 1;

	int	n = 0;
	if (GameBoyMode())
	{
		for (auto &d : gb_domains)
			if (d.size())
			{
				lua_pushstring(L, d.name);
				lua_rawseti(L, -2, ++n);
			}
	}
	else
	{
		for (auto &d : snes_domains)
			if (d.size())
			{
				lua_pushstring(L, d.name);
				lua_rawseti(L, -2, ++n);
			}
	}
	return 1;
}

// Hooks take a bus address, or {first, last}; memory.wram.* takes WRAM offsets.
template <int kind>
int mem_on (lua_State *L)
{
	bool	wram = lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING &&
	               !strcmp(lua_tostring(L, lua_upvalueindex(1)), "wram");
	if (lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING && !wram &&
	    strcmp(lua_tostring(L, lua_upvalueindex(1)), "bus"))
		luaL_error(L, "memory hooks watch the bus or WRAM; use memory.on_* or memory.wram.on_*");
	if (GameBoyMode())
		luaL_error(L, "memory hooks aren't available for Game Boy games yet");

	lua_Integer	first, last;
	if (lua_istable(L, 1))
	{
		lua_geti(L, 1, 1);
		lua_geti(L, 1, 2);
		first = luaL_checkinteger(L, -2);
		last = luaL_checkinteger(L, -1);
		lua_pop(L, 2);
	}
	else
		first = last = luaL_checkinteger(L, 1);

	if (wram)
	{
		if (first < 0 || last > 0x1ffff || first > last)
			luaL_argerror(L, 1, "WRAM offsets run from 0 to 0x1FFFF");
		first += 0x7e0000;
		last += 0x7e0000;
	}

	if (first > last || last > 0xffffff || first < 0)
		luaL_argerror(L, 1, "expected an address or a {first, last} range on the 24-bit bus");

	// Fold a mirror range ($00:0100 is $7E:0100) when it folds in one piece.
	uint32	a = LuaNormalizeBus((uint32) first), b = LuaNormalizeBus((uint32) last);
	if (b >= a && b - a == (uint32) (last - first))
		return LuaAddCallback(L, kind, 2, a, b);
	return LuaAddCallback(L, kind, 2, (uint32) first, (uint32) last);
}

const luaL_Reg	access_functions[] =
{
	{ "read_u8",		mem_read<1, false, false> },
	{ "read_s8",		mem_read<1, true, false> },
	{ "read_u16",		mem_read<2, false, false> },
	{ "read_s16",		mem_read<2, true, false> },
	{ "read_u24",		mem_read<3, false, false> },
	{ "read_s24",		mem_read<3, true, false> },
	{ "read_u32",		mem_read<4, false, false> },
	{ "read_s32",		mem_read<4, true, false> },
	{ "read_u16_be",	mem_read<2, false, true> },
	{ "read_s16_be",	mem_read<2, true, true> },
	{ "read_u24_be",	mem_read<3, false, true> },
	{ "read_u32_be",	mem_read<4, false, true> },
	{ "read_s32_be",	mem_read<4, true, true> },
	{ "write_u8",		mem_write<1, false> },
	{ "write_u16",		mem_write<2, false> },
	{ "write_u24",		mem_write<3, false> },
	{ "write_u32",		mem_write<4, false> },
	{ "write_u16_be",	mem_write<2, true> },
	{ "write_u24_be",	mem_write<3, true> },
	{ "write_u32_be",	mem_write<4, true> },
	{ "read_bytes",		mem_read_bytes },
	{ "read_string",	mem_read_string },
	{ "write_bytes",	mem_write_bytes },
	{ "size",			mem_size },
	{ "on_read",		mem_on<LUA_CB_READ> },
	{ "on_write",		mem_on<LUA_CB_WRITE> },
	{ "on_exec",		mem_on<LUA_CB_EXEC> },
	{ nullptr, nullptr }
};

// memory.wram etc.: the same functions with the domain bound.
void PushDomainTable (lua_State *L, const char *name)
{
	lua_createtable(L, 0, (int) (sizeof(access_functions) / sizeof(access_functions[0])));
	for (const luaL_Reg *r = access_functions; r->name; r++)
	{
		lua_pushstring(L, name);
		lua_pushcclosure(L, r->func, 1);
		lua_setfield(L, -2, r->name);
	}
	lua_pushstring(L, name);
	lua_setfield(L, -2, "name");
}

int mem_domain (lua_State *L)
{
	const char	*name = luaL_checkstring(L, 1);
	lua_getglobal(L, "memory");
	lua_getfield(L, -1, name);
	if (!lua_istable(L, -1))
		luaL_error(L, "no memory domain named '%s'", name);
	return 1;
}

}	// namespace

void LuaOpenMemory (lua_State *L)
{
	luaL_newlib(L, access_functions);

	lua_pushcfunction(L, mem_domains);
	lua_setfield(L, -2, "domains");
	lua_pushcfunction(L, mem_domain);
	lua_setfield(L, -2, "domain");

	for (const char *name : all_domain_names)
	{
		PushDomainTable(L, name);
		lua_setfield(L, -2, name);
	}

	lua_setglobal(L, "memory");
}
