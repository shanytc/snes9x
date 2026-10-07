/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Behaviour follows ares' HG51B and HitachiDSP (ISC licence, (c) ares team,
// Near et al.), rewritten as a catch-up interpreter.

#include <string.h>
#include "snes9x.h"
#include "memmap.h"
#include "hg51b.h"

#define CX4_HZ		20000000	// the Cx4 carts' oscillator

enum { OP_NONE, OP_FILL, OP_DMA };	// a cache fill or DMA under way runs to its end
enum { CACHE_HIT, CACHE_FILL, CACHE_MISS };

// Saved as-is, so the padding is explicit and the layout the same everywhere.
struct Regs
{
	uint16	prog[2][256];		// the instruction cache's two pages
	uint8	dram[3072];			// data RAM
	uint32	gpr[16];
	uint32	stack[8];			// return addresses, pb << 8 | pc
	uint32	a, mdr, rom, ram, mar, dpr;
	uint32	cache_tag[2];		// bus address each page was filled from
	uint32	cache_base;
	uint32	dma_src, dma_dst;
	uint32	bus_addr;
	uint32	fill_addr;
	uint32	pad0;
	uint64	mul;				// 48-bit product
	uint16	pb, p, cache_pb, dma_len, dma_pos, fill_pos;
	uint8	pc, cache_pc;
	uint8	n, z, c, v, i;
	uint8	lock, halt, irq, rom_cfg, wait_rom, wait_ram;
	uint8	suspend_on, suspend_len;
	uint8	cache_on, cache_page, cache_preload, cache_lock[2];
	uint8	dma_on;
	uint8	bus_on, bus_reading, bus_writing, bus_pending;
	uint8	op, fill_page;
	uint8	vector[32];
	uint8	pad1[1];
	uint64	line_base;			// master clock at the start of the current scanline
	uint64	synced;				// master clock the chip has been run up to
	uint64	rem;				// what's left over, in master clocks * CX4_HZ
	int64	budget;				// chip clocks owed; below 0 when it ran ahead
};
static_assert(sizeof(Regs) == 4360, "savestate layout");

static const uint32	kConstants[16] = {
	0x000000, 0xffffff, 0x00ff00, 0xff0000, 0x00ffff, 0xffff00, 0x800000, 0x7fffff,
	0x008000, 0x007fff, 0xff7fff, 0xffff7f, 0x010000, 0xfeffff, 0x000100, 0x00feff
};

static uint32	drom[1024];
static Regs		r;
static uint32	hit_key = ~0u;	// pb | page << 15 of the last cache hit; ~0 once a tag or the base may have moved
static bool8	loaded = FALSE;
static bool8	active = FALSE;

static alwaysinline void Step (uint32 clocks);

static inline uint32 Word24 (const uint8 *p)
{
	return (p[0] | (p[1] << 8) | (p[2] << 16));
}

bool8 S9xHG51BIsDataROM (const uint8 *image, uint32 size)
{
	if (!image || size != HG51B_DATAROM_SIZE)
		return (FALSE);

	// It opens with a reciprocal table: $FFFFFF, then $800000 / n.
	return (Word24(image) == 0xffffff && Word24(image + 3) == 0x800000 &&
			Word24(image + 6) == 0x400000 && Word24(image + 12) == 0x200000);
}

bool8 S9xHG51BLoad (const uint8 *image, uint32 size)
{
	loaded = active = FALSE;
	if (!S9xHG51BIsDataROM(image, size))
		return (FALSE);

	for (int i = 0; i < 1024; i++)
		drom[i] = Word24(image + i * 3);

	loaded = TRUE;
	S9xHG51BReset();
	return (TRUE);
}

void S9xHG51BUnload (void)
{
	loaded = active = FALSE;
}

bool8 S9xHG51BLoaded (void)
{
	return (loaded);
}

bool8 S9xHG51BActive (void)
{
	return (active);
}

void S9xHG51BReset (void)
{
	if (!loaded)
		return;
	memset(&r, 0, sizeof(r));
	hit_key = ~0u;
	r.halt = 1;
	r.rom_cfg = 1;
	r.wait_rom = r.wait_ram = 3;
	CPU.IRQExternal = FALSE;
	active = TRUE;
}

void S9xHG51BSuspend (void)
{
	active = FALSE;
}

static inline void SetByte (uint32 &reg, int byte, uint8 data)
{
	reg = (reg & ~(0xffu << (byte * 8))) | ((uint32) data << (byte * 8));
}

static inline bool Busy (void)
{
	return (r.cache_on || r.dma_on || r.bus_pending);
}

static inline bool Running (void)
{
	return (Busy() || !r.halt);
}

// The IRQ goes up on every halt unless the game masked it at $7F51.
static void Halt (void)
{
	r.halt = 1;
	if (!r.irq)
	{
		r.i = 1;
		CPU.IRQExternal = TRUE;
	}
}

static uint8 ReadIO (uint32 address)
{
	address = 0x7c00 | (address & 0x3ff);

	switch (address)
	{
		case 0x7f40: return (r.dma_src);
		case 0x7f41: return (r.dma_src >> 8);
		case 0x7f42: return (r.dma_src >> 16);
		case 0x7f43: return (r.dma_len);
		case 0x7f44: return (r.dma_len >> 8);
		case 0x7f45: return (r.dma_dst);
		case 0x7f46: return (r.dma_dst >> 8);
		case 0x7f47: return (r.dma_dst >> 16);
		case 0x7f48: return (r.cache_page);
		case 0x7f49: return (r.cache_base);
		case 0x7f4a: return (r.cache_base >> 8);
		case 0x7f4b: return (r.cache_base >> 16);
		case 0x7f4c: return (r.cache_lock[0] | (r.cache_lock[1] << 1));
		case 0x7f4d: return (r.cache_pb);
		case 0x7f4e: return (r.cache_pb >> 8);
		case 0x7f4f: return (r.cache_pc);
		case 0x7f50: return (r.wait_ram | (r.wait_rom << 4));
		case 0x7f51: return (r.irq);
		case 0x7f52: return (r.rom_cfg);
		case 0x7f53: case 0x7f54: case 0x7f55: case 0x7f56: case 0x7f57:
		case 0x7f59: case 0x7f5b: case 0x7f5c: case 0x7f5d: case 0x7f5e:
		case 0x7f5f:
			return (r.suspend_on | (r.i << 1) | (Running() << 6) | (Busy() << 7));
	}

	if (address >= 0x7f60 && address <= 0x7f7f)
		return (r.vector[address & 0x1f]);

	if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef))
	{
		address &= 0x3f;
		return (r.gpr[address / 3] >> ((address % 3) * 8));
	}

	return (0);
}

static void WriteIO (uint32 address, uint8 data)
{
	address = 0x7c00 | (address & 0x3ff);

	switch (address)
	{
		case 0x7f40: SetByte(r.dma_src, 0, data); return;
		case 0x7f41: SetByte(r.dma_src, 1, data); return;
		case 0x7f42: SetByte(r.dma_src, 2, data); return;
		case 0x7f43: r.dma_len = (r.dma_len & 0xff00) | data; return;
		case 0x7f44: r.dma_len = (r.dma_len & 0x00ff) | (data << 8); return;
		case 0x7f45: SetByte(r.dma_dst, 0, data); return;
		case 0x7f46: SetByte(r.dma_dst, 1, data); return;
		case 0x7f47:
			SetByte(r.dma_dst, 2, data);
			if (r.halt)
				r.dma_on = 1;
			return;

		case 0x7f48:
			r.cache_page = data & 1;
			if (r.halt)
			{
				r.pb = r.cache_pb;
				r.cache_preload = 1;
				r.cache_on = 1;
			}
			return;

		case 0x7f49: SetByte(r.cache_base, 0, data); hit_key = ~0u; return;
		case 0x7f4a: SetByte(r.cache_base, 1, data); hit_key = ~0u; return;
		case 0x7f4b: SetByte(r.cache_base, 2, data); hit_key = ~0u; return;
		case 0x7f4c:
			r.cache_lock[0] = data & 1;
			r.cache_lock[1] = (data >> 1) & 1;
			return;
		case 0x7f4d: r.cache_pb = (r.cache_pb & 0x7f00) | data; return;
		case 0x7f4e: r.cache_pb = ((data << 8) | (r.cache_pb & 0xff)) & 0x7fff; return;

		case 0x7f4f:
			r.cache_pc = data;
			if (r.halt)
			{
				r.halt = 0;
				r.pb = r.cache_pb;
				r.pc = r.cache_pc;
			}
			return;

		case 0x7f50:
			r.wait_ram = data & 7;
			r.wait_rom = (data >> 4) & 7;
			return;

		case 0x7f51:
			r.irq = data & 1;
			if (r.irq)
			{
				r.i = 0;
				CPU.IRQExternal = FALSE;
			}
			return;

		case 0x7f52: r.rom_cfg = data & 1; return;

		case 0x7f53:
			r.lock = 0;
			r.halt = 1;
			return;

		// $7F55 suspends until $7F5D; the rest for 32 clocks a step up to 224.
		case 0x7f55: case 0x7f56: case 0x7f57: case 0x7f58:
		case 0x7f59: case 0x7f5a: case 0x7f5b: case 0x7f5c:
			r.suspend_on = 1;
			r.suspend_len = (address - 0x7f55) * 32;
			return;
		case 0x7f5d: r.suspend_on = 0; return;
		case 0x7f5e: r.i = 0; return;	// the CPU's IRQ line stays up
	}

	if (address >= 0x7f60 && address <= 0x7f7f)
	{
		r.vector[address & 0x1f] = data;
		return;
	}

	if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef))
	{
		address &= 0x3f;
		SetByte(r.gpr[address / 3], address % 3, data);
	}
}

// The chip's own bus: cart ROM and RAM as the LoROM board maps them, its data
// RAM and its registers.
static inline bool IsROM (uint32 a)
{
	return ((a & 0x408000) == 0x008000 || (a & 0xc00000) == 0xc00000);
}

static inline bool IsRAM (uint32 a)
{
	return ((a & 0xf88000) == 0x700000);
}

static inline bool IsDRAM (uint32 a)
{
	return ((a & 0x40e000) == 0x006000 && (a & 0x0c00) != 0x0c00);
}

static inline bool IsIO (uint32 a)
{
	return ((a & 0x40ec00) == 0x006c00);
}

static inline uint32 SRAMIndex (uint32 a)
{
	return ((((a & 0x070000) >> 1) | (a & 0x7fff)) & Memory.SRAMMask);
}

static uint8 BusRead (uint32 a)
{
	if (IsROM(a))
	{
		uint32	lin = ((a & 0x3f0000) >> 1) | (a & 0x7fff);
		if (lin >= Memory.CalculatedSize)
			lin = Memory.map_mirror(Memory.CalculatedSize, lin);
		return (Memory.ROM[lin]);
	}
	if (IsRAM(a))
		return (Memory.SRAMSize ? Memory.SRAM[SRAMIndex(a)] : 0);
	if (IsDRAM(a))
		return (r.dram[a & 0xfff]);
	if (IsIO(a))
		return (ReadIO(a));
	return (0);
}

static void BusWrite (uint32 a, uint8 data)
{
	if (IsROM(a))
		return;
	if (IsRAM(a))
	{
		if (Memory.SRAMSize)
			Memory.SRAM[SRAMIndex(a)] = data;
	}
	else
	if (IsDRAM(a))
		r.dram[a & 0xfff] = data;
	else
	if (IsIO(a))
		WriteIO(a, data);
}

static inline uint32 Wait (uint32 a)
{
	if (IsROM(a))
		return (1 + r.wait_rom);
	if (IsRAM(a))
		return (1 + r.wait_ram);
	return (0);
}

static alwaysinline uint32 ReadRegister (uint32 reg)
{
	switch (reg)
	{
		case 0x01: return ((uint32) (r.mul >> 24) & 0xffffff);
		case 0x02: return ((uint32) r.mul & 0xffffff);
		case 0x03: return (r.mdr);
		case 0x08: return (r.rom);
		case 0x0c: return (r.ram);
		case 0x13: return (r.mar);
		case 0x1c: return (r.dpr);
		case 0x20: return (r.pc);
		case 0x28: return (r.p);
		case 0x2e:		// a bus read into MDR, with ROM wait states
		case 0x2f:		// or RAM ones
			r.bus_on = 1;
			r.bus_reading = 1;
			r.bus_pending = (reg == 0x2e) ? r.wait_rom : r.wait_ram;
			r.bus_addr = r.mar;
			return (0);
	}

	if (reg >= 0x50 && reg <= 0x5f)
		return (kConstants[reg & 15]);
	if (reg >= 0x60)
		return (r.gpr[reg & 15]);
	return (0);
}

static alwaysinline void WriteRegister (uint32 reg, uint32 data)
{
	data &= 0xffffff;

	switch (reg)
	{
		case 0x01: r.mul = (r.mul & 0xffffff) | ((uint64) data << 24); return;
		case 0x02: r.mul = (r.mul & 0xffffff000000ull) | data; return;
		case 0x03: r.mdr = data; return;
		case 0x08: r.rom = data; return;
		case 0x0c: r.ram = data; return;
		case 0x13: r.mar = data; return;
		case 0x1c: r.dpr = data; return;
		case 0x20: r.pc = (uint8) data; return;
		case 0x28: r.p = data & 0x7fff; return;
		case 0x2e:		// a bus write of MDR's low byte
		case 0x2f:
			r.bus_on = 1;
			r.bus_writing = 1;
			r.bus_pending = (reg == 0x2e) ? r.wait_rom : r.wait_ram;
			r.bus_addr = r.mar;
			return;
	}

	if (reg >= 0x60)
		r.gpr[reg & 15] = data;
}

static inline uint32 SignExtend24 (uint32 x)
{
	return ((uint32) ((int32) (x << 8) >> 8));
}

static inline uint32 SetNZ (uint32 x)
{
	x &= 0xffffff;
	r.n = (x >> 23) & 1;
	r.z = (x == 0);
	return (x);
}

static alwaysinline uint32 Add (uint32 x, uint32 y)
{
	int32	z = (int32) x + (int32) y;
	r.c = (z > 0xffffff);
	r.v = (~(x ^ y) & (x ^ z) & 0x800000) != 0;
	return (SetNZ(z));
}

// The overflow test is the same as ADD's, as bsnes and ares have it.
static alwaysinline uint32 Sub (uint32 x, uint32 y)
{
	int32	z = (int32) x - (int32) y;
	r.c = (z >= 0);
	r.v = (~(x ^ y) & (x ^ z) & 0x800000) != 0;
	return (SetNZ(z));
}

static uint64 Mul (uint32 x, uint32 y)
{
	int64	p = (int64) (int32) SignExtend24(x) * (int32) SignExtend24(y);
	return ((uint64) p & 0xffffffffffffull);
}

// kind: 0 SHR, 1 ASR, 2 ROR, 3 SHL. Counts past 24 shift by nothing.
static alwaysinline uint32 Shift (int kind, uint32 a, uint32 s)
{
	s &= 31;
	if (s > 24)
		s = 0;

	switch (kind)
	{
		case 0:  a >>= s; break;
		case 1:  a = (uint32) ((int32) SignExtend24(a) >> s); break;
		case 2:  a = (a >> s) | (a << (24 - s)); break;
		default: a <<= s; break;
	}

	return (SetNZ(a));
}

static void Push (void)
{
	memmove(r.stack + 1, r.stack, 7 * sizeof(r.stack[0]));
	r.stack[0] = (r.pb << 8) | r.pc;
}

static void Pull (void)
{
	uint32	pc = r.stack[0];
	memmove(r.stack, r.stack + 1, 7 * sizeof(r.stack[0]));
	r.stack[7] = 0;
	r.pb = (pc >> 8) & 0x7fff;
	r.pc = (uint8) pc;
}

// Finds the page holding pb, or picks an unlocked one and starts filling it.
static alwaysinline int Cache (void)
{
	if (!r.cache_preload && hit_key == ((uint32) r.pb | ((uint32) r.cache_page << 15)))
		return (r.cache_on = 0, CACHE_HIT);
	hit_key = ~0u;

	uint32	address = (r.cache_base + r.pb * 512) & 0xffffff;

	if (!r.cache_preload)
	{
		if (r.cache_tag[r.cache_page] == address)
			return (hit_key = r.pb | (r.cache_page << 15), r.cache_on = 0, CACHE_HIT);
		r.cache_page ^= 1;
		if (r.cache_tag[r.cache_page] == address)
			return (hit_key = r.pb | (r.cache_page << 15), r.cache_on = 0, CACHE_HIT);
		if (r.cache_lock[r.cache_page])
			r.cache_page ^= 1;
		if (r.cache_lock[r.cache_page])
			return (r.cache_on = 0, CACHE_MISS);
	}

	r.cache_preload = 0;
	hit_key = ~0u;
	r.cache_tag[r.cache_page] = address;
	r.op = OP_FILL;
	r.fill_page = r.cache_page;
	r.fill_addr = address;
	r.fill_pos = 0;
	return (CACHE_FILL);
}

static void FillByte (void)
{
	Step(Wait(r.fill_addr));
	uint8	byte = BusRead(r.fill_addr);
	r.fill_addr = (r.fill_addr + 1) & 0xffffff;

	uint16	&word = r.prog[r.fill_page][r.fill_pos >> 1];
	word = (r.fill_pos & 1) ? (word & 0x00ff) | (byte << 8) : (word & 0xff00) | byte;

	if (++r.fill_pos == 512)
	{
		r.op = OP_NONE;
		r.cache_on = 0;
	}
}

// ROM to ROM or RAM to RAM can't share the bus: the chip locks up until $7F53.
static void DmaByte (void)
{
	if (r.dma_pos >= r.dma_len)
	{
		r.op = OP_NONE;
		r.dma_on = 0;
		return;
	}

	uint32	src = (r.dma_src + r.dma_pos) & 0xffffff;
	uint32	dst = (r.dma_dst + r.dma_pos) & 0xffffff;
	if ((IsROM(src) && IsROM(dst)) || (IsRAM(src) && IsRAM(dst)))
	{
		r.op = OP_NONE;
		r.lock = 1;
		return;
	}

	Step(Wait(src));
	uint8	data = BusRead(src);
	Step(Wait(dst));
	BusWrite(dst, data);

	if (++r.dma_pos >= r.dma_len)
	{
		r.op = OP_NONE;
		r.dma_on = 0;
	}
}

static alwaysinline void Advance (void)
{
	r.pc++;
	if (r.pc != 0)
		return;

	// Off the end of page 0 into page 1, which takes the bank in P.
	if (r.cache_page == 1)
	{
		Halt();
		return;
	}
	r.cache_page = 1;
	if (r.cache_lock[1])
	{
		Halt();
		return;
	}
	r.pb = r.p;
	if (Cache() == CACHE_MISS)
		Halt();
}

static alwaysinline void Jump (uint16 op, bool take, bool call)
{
	if (!take)
		return;
	if (call)
		Push();
	if (op & 0x200)
		r.pb = r.p;
	r.pc = (uint8) op;
	Step(2);
}

static inline uint32 DRAMIndex (uint32 a)
{
	a &= 0xfff;
	return (a >= 0xc00 ? a - 0x400 : a);
}

static alwaysinline void Instruction (uint16 op)
{
	static const uint8	shifts[4] = { 0, 1, 8, 16 };
	const uint32	reg = op & 0x7f;
	const uint32	imm = op & 0xff;
	const uint32	sub = (op >> 8) & 3;
	const uint32	a = (r.a << shifts[sub]) & 0xffffff;	// the ALU's shifted A

	switch (op >> 10)
	{
		case 0x02: Jump(op, true, false); break;
		case 0x03: Jump(op, r.z, false); break;
		case 0x04: Jump(op, r.c, false); break;
		case 0x05: Jump(op, r.n, false); break;
		case 0x06: Jump(op, r.v, false); break;

		case 0x07:		// WAIT: out the rest of a bus access
			if (r.bus_on)
				Step(r.bus_pending);
			break;

		case 0x09:		// SKIP: past the next instruction if V, C, Z or N matches
		{
			const uint8	flag = (sub == 0) ? r.v : (sub == 1) ? r.c : (sub == 2) ? r.z : r.n;
			if (flag == (op & 1))
			{
				Advance();
				Step(1);
			}
			break;
		}

		case 0x0a: Jump(op, true, true); break;
		case 0x0b: Jump(op, r.z, true); break;
		case 0x0c: Jump(op, r.c, true); break;
		case 0x0d: Jump(op, r.n, true); break;
		case 0x0e: Jump(op, r.v, true); break;
		case 0x0f: Pull(); Step(2); break;

		case 0x10: r.mar = (r.mar + 1) & 0xffffff; break;

		case 0x12: Sub(ReadRegister(reg), a); break;	// CMPR
		case 0x13: Sub(imm, a); break;
		case 0x14: Sub(a, ReadRegister(reg)); break;	// CMP
		case 0x15: Sub(a, imm); break;

		case 0x16:
			if (sub == 1)
				r.a = SetNZ((uint32) (int32) (int8) r.a);	// SXB
			else
			if (sub == 2)
				r.a = SetNZ((uint32) (int32) (int16) r.a);	// SXW
			break;

		case 0x18:		// LD A/MDR/MAR,reg; LD P,Rn
			switch (sub)
			{
				case 0: r.a = ReadRegister(reg); break;
				case 1: r.mdr = ReadRegister(reg); break;
				case 2: r.mar = ReadRegister(reg); break;
				case 3: r.p = r.gpr[op & 15] & 0x7fff; break;
			}
			break;

		case 0x19:		// LD A/MDR/MAR/P,imm
			switch (sub)
			{
				case 0: r.a = imm; break;
				case 1: r.mdr = imm; break;
				case 2: r.mar = imm; break;
				case 3: r.p = imm; break;
			}
			break;

		case 0x1a:		// RDRAM byte,A
		case 0x1b:		// RDRAM byte,DPR+imm
			if (sub != 3)
			{
				uint32	i = DRAMIndex((op & 0x400) ? r.dpr + imm : r.a);
				SetByte(r.ram, sub, r.dram[i]);
			}
			break;

		case 0x1c: r.rom = drom[r.a & 0x3ff]; break;
		case 0x1d: r.rom = drom[op & 0x3ff]; break;

		case 0x1f:
			if (sub == 0)
				r.p = (r.p & 0x7f00) | imm;					// LD PL
			else
			if (sub == 1)
				r.p = (r.p & 0x00ff) | ((op & 0x7f) << 8);	// LD PH
			break;

		case 0x20: r.a = Add(a, ReadRegister(reg)); break;
		case 0x21: r.a = Add(a, imm); break;
		case 0x22: r.a = Sub(ReadRegister(reg), a); break;	// SUBR
		case 0x23: r.a = Sub(imm, a); break;
		case 0x24: r.a = Sub(a, ReadRegister(reg)); break;
		case 0x25: r.a = Sub(a, imm); break;
		case 0x26: r.mul = Mul(r.a, ReadRegister(reg)); break;
		case 0x27: r.mul = Mul(r.a, imm); break;
		case 0x28: r.a = SetNZ(~a ^ ReadRegister(reg)); break;	// XNOR
		case 0x29: r.a = SetNZ(~a ^ imm); break;
		case 0x2a: r.a = SetNZ(a ^ ReadRegister(reg)); break;
		case 0x2b: r.a = SetNZ(a ^ imm); break;
		case 0x2c: r.a = SetNZ(a & ReadRegister(reg)); break;
		case 0x2d: r.a = SetNZ(a & imm); break;
		case 0x2e: r.a = SetNZ(a | ReadRegister(reg)); break;
		case 0x2f: r.a = SetNZ(a | imm); break;

		case 0x30: r.a = Shift(0, r.a, ReadRegister(reg)); break;
		case 0x31: r.a = Shift(0, r.a, op & 0x1f); break;
		case 0x32: r.a = Shift(1, r.a, ReadRegister(reg)); break;
		case 0x33: r.a = Shift(1, r.a, op & 0x1f); break;
		case 0x34: r.a = Shift(2, r.a, ReadRegister(reg)); break;
		case 0x35: r.a = Shift(2, r.a, op & 0x1f); break;
		case 0x36: r.a = Shift(3, r.a, ReadRegister(reg)); break;
		case 0x37: r.a = Shift(3, r.a, op & 0x1f); break;

		case 0x38:		// ST reg,A / ST reg,MDR
			if (sub == 0)
				WriteRegister(reg, r.a);
			else
			if (sub == 1)
				WriteRegister(reg, r.mdr);
			break;

		case 0x3a:		// WRRAM byte,A
		case 0x3b:		// WRRAM byte,DPR+imm
			if (sub != 3)
				r.dram[DRAMIndex((op & 0x400) ? r.dpr + imm : r.a)] = (uint8) (r.ram >> (sub * 8));
			break;

		case 0x3c:		// SWAP A,Rn
		{
			uint32	t = r.a;
			r.a = r.gpr[op & 15];
			r.gpr[op & 15] = t;
			break;
		}

		case 0x3e:		// CLEAR
			r.a = r.p = 0;
			r.ram = r.dpr = 0;
			break;

		case 0x3f: Halt(); break;

		default: break;	// NOP and the unused encodings
	}
}

static alwaysinline void Execute (void)
{
	switch (Cache())
	{
		case CACHE_MISS: Halt(); return;
		case CACHE_FILL: return;	// fetch once the page is in
	}

	uint16	op = r.prog[r.cache_page][r.pc];
	Advance();
	Step(1);
	Instruction(op);
}

static void Suspend (void)
{
	if (!r.suspend_len)
	{
		Step(1);	// until $7F5D
		return;
	}
	Step(r.suspend_len);
	r.suspend_len = 0;
	r.suspend_on = 0;
}

// A bus access the chip started finishes on the clocks after it.
static alwaysinline void Step (uint32 clocks)
{
	r.budget -= clocks;

	if (!r.bus_on)
		return;
	if (r.bus_pending > clocks)
	{
		r.bus_pending -= clocks;
		return;
	}

	r.bus_on = 0;
	r.bus_pending = 0;
	if (r.bus_reading)
	{
		r.bus_reading = 0;
		r.mdr = BusRead(r.bus_addr);
	}
	if (r.bus_writing)
	{
		r.bus_writing = 0;
		BusWrite(r.bus_addr, (uint8) r.mdr);
	}
}

static void Main (void)
{
	if (r.op == OP_FILL)
		FillByte();
	else
	if (r.op == OP_DMA)
		DmaByte();
	else
	if (r.lock)
		Step(1);
	else
	if (r.suspend_on)
		Suspend();
	else
	if (r.cache_on)
		Cache();	// a $7F48 preload: always fills
	else
	if (r.dma_on)
	{
		r.op = OP_DMA;
		r.dma_pos = 0;
		DmaByte();
	}
	else
	if (r.halt)
		Step(1);
	else
		Execute();
}

// Nothing would change however long it ran.
static bool Idle (void)
{
	if (r.op != OP_NONE || r.bus_on)
		return (false);
	if (r.lock)
		return (true);
	if (r.suspend_on)
		return (r.suspend_len == 0);
	if (r.cache_on || r.dma_on)
		return (false);
	return (r.halt != 0);
}

static void SyncTo (int32 cycles)
{
	uint64	t = r.line_base + (uint64) (int64) cycles;
	if (t <= r.synced)
		return;

	uint64	master = Settings.PAL ? 21281370 : 21477273;
	uint64	acc = (t - r.synced) * CX4_HZ + r.rem;
	r.synced = t;
	r.budget += (int64) (acc / master);
	r.rem = acc % master;

	// An overclocked CPU is there to cut slowdown, so the chip keeps up instantly then.
	if (ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8)
	{
		for (int i = 0; i < 1000000 && !Idle(); i++)
			Main();
		r.budget = 0;
		return;
	}

	while (r.budget > 0)
	{
		// Plain execution, what Main() would pick, without its state tests.
		if (!(r.op | r.lock | r.suspend_on | r.cache_on | r.dma_on | r.halt | r.bus_on))
		{
			Execute();
			continue;
		}
		if (Idle())
		{
			r.budget = 0;
			break;
		}
		Main();
	}
}

void S9xHG51BEndScanline (void)
{
	SyncTo(CPU.Cycles);
	r.line_base += Timings.H_Max;
}

// Data RAM at $6000-$6BFF and $7000-$7BFF, the registers in between.
uint8 S9xHG51BRead (uint16 address, int32 speed)
{
	if (speed >= 0)
		SyncTo(CPU.Cycles + speed);
	if ((address & 0x0c00) != 0x0c00)
		return (r.dram[address & 0xfff]);
	return (ReadIO(address));
}

void S9xHG51BWrite (uint8 byte, uint16 address, int32 speed)
{
	if ((address & 0x0c00) != 0x0c00)
	{
		if (speed >= 0)
			SyncTo(CPU.Cycles + speed);
		r.dram[address & 0xfff] = byte;
		return;
	}

	// A cheat's poke reaches data RAM only.
	if (speed < 0)
		return;
	SyncTo(CPU.Cycles + speed);
	WriteIO(address, byte);
}

uint32 S9xHG51BStateSize (void)
{
	return (sizeof(r));
}

void S9xHG51BStateSave (uint8 *buf)
{
	memcpy(buf, &r, sizeof(r));
}

bool8 S9xHG51BStateLoad (const uint8 *buf, uint32 size)
{
	if (!loaded || size != sizeof(r))
		return (FALSE);
	memcpy(&r, buf, sizeof(r));
	hit_key = ~0u;
	active = TRUE;
	return (TRUE);
}
