/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Behaviour follows ares' HG51B and HitachiDSP (ISC licence, (c) ares team,
// Near et al.), rewritten as a catch-up interpreter.

#include <string.h>
#include <stddef.h>
#include <math.h>
#include "snes9x.h"
#include "memmap.h"
#include "hg51b.h"
#include "hg51bn.h"

#define CX4_HZ		20000000	// the Cx4 carts' oscillator

enum { OP_NONE, OP_FILL, OP_DMA };	// a cache fill or DMA under way runs to its end
enum { CACHE_HIT, CACHE_FILL, CACHE_MISS };

typedef S9xHG51BRegs	Regs;
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
static bool8	native = FALSE;	// the built-in data ROM: each job is run whole when it starts
static bool8	halt_op;		// a HALT instruction stopped the chip

#define AHEAD_CAP		(1 << 24)	// clocks a job run ahead may take; a longer one runs as it goes
#define AHEAD_WINDOWS	4096
enum { AHEAD_JOB = 1, AHEAD_IDLE };	// run ahead (and r.fresh): a job, or a preload or DMA

// Run ahead, r holds the end, `start` the chip as the CPU sees it until its clocks have passed.
// The windows are in scaled clocks (elapsed master clocks * CX4_HZ + rem), as SyncTo counts them.
static struct
{
	bool	on;
	uint8	kind;
	uint8	status;				// what the status registers read meanwhile, but for a job's busy
	uint64	master;
	uint64	t_done;				// the master clock from which the CPU sees the end
	uint64	busy[AHEAD_WINDOWS][2];	// [from, to): a job's bus access in flight
	uint32	n, cur;
	Regs	start;
}	ahead;

// The status reads that SkipPolls learns the CPU's wait from, and the length of a turn of it at the
// sites seen to repeat one (by PB:PC, and the E flag, which can cost a branch a cycle).
static struct
{
	uint32	pbpc;
	int32	cycles, next, period;
	uint64	line;
	uint32	site[8];
	int32	turn[8];
}	poll;

// Idle, the chip changes only on a CPU write, so its syncs wait (to `parked_t`) until one comes.
// `view` is its 4K block as reads give it, for DMA, while `viewed`.
static bool		parked = false;
static uint64	parked_t;
static bool		viewed = false;
static uint8	view[0x1000];

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
	native = FALSE;
	S9xHG51BReset();
	return (TRUE);
}

// A table entry: the value's fixed-point floor, capped at $FFFFFF. Every entry that isn't an exact
// integer lies over 0.0006 from one, so the nudge only steadies tan(pi/4) against rounding.
static uint32 Fixed (double x)
{
	const double	v = floor(x + 1e-6);
	return (v >= 16777215.0 ? 0xffffff : (uint32) v);
}

// The data ROM's tables, worked out: $000 $800000 / n, $100 sqrt(n / 256), then quarter waves in
// 128 steps of pi / 256: $200 sin, $280 asin(n / 128) / pi, $300 tan (x $10000), $380 cos.
bool8 S9xHG51BLoadBuiltin (void)
{
	const double	pi = 3.14159265358979323846;

	for (uint32 n = 0; n < 256; n++)
	{
		drom[n] = n ? 0x800000 / n : 0xffffff;
		// floor(sqrt(n << 40)), squared back to settle the double's last bit
		uint64	s = (uint64) sqrt((double) ((uint64) n << 40));
		while (s * s > ((uint64) n << 40))
			s--;
		while ((s + 1) * (s + 1) <= ((uint64) n << 40))
			s++;
		drom[0x100 + n] = (uint32) s;
	}
	for (uint32 n = 0; n < 128; n++)
	{
		const double	a = n * pi / 256;
		drom[0x200 + n] = Fixed(sin(a) * 16777216.0);
		drom[0x280 + n] = Fixed(asin(n / 128.0) / pi * 16777216.0);
		drom[0x300 + n] = Fixed(tan(a) * 65536.0);
		drom[0x380 + n] = Fixed(cos(a) * 16777216.0);
	}

	loaded = TRUE;
	native = TRUE;
	S9xHG51BReset();
	return (TRUE);
}

void S9xHG51BUnload (void)
{
	loaded = active = FALSE;
	ahead.on = false;
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
	ahead.on = false;
	parked = viewed = false;
	memset(&poll, 0xff, sizeof(poll));
	S9xHG51BNativeReset();
	r.halt = 1;
	r.rom_cfg = 1;
	r.wait_rom = r.wait_ram = 3;
	CPU.IRQExternal = FALSE;
	active = TRUE;
}

void S9xHG51BSuspend (void)
{
	active = FALSE;
	ahead.on = false;
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

		case 0x3f: halt_op = TRUE; Halt(); break;

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

// Plain execution with the hot registers in locals, written back on the way out:
// after a bus access starts, a halt or a PB change, and before a page's last two.
static void RunFast (void)
{
	switch (Cache())
	{
		case CACHE_MISS: Halt(); return;
		case CACHE_FILL: return;
	}

	static const uint8	shifts[4] = { 0, 1, 8, 16 };
	const uint16	*page = r.prog[r.cache_page];
	int64	budget = r.budget;
	uint64	mul = r.mul;
	uint32	a = r.a, mdr = r.mdr, rom = r.rom, ram = r.ram, mar = r.mar, dpr = r.dpr;
	uint16	pb = r.pb, p = r.p;
	uint8	pc = r.pc, fn = r.n, fz = r.z, fc = r.c, fv = r.v;
	bool	leave = false;

	// ReadRegister / WriteRegister on the locals; a bus access starts as in the general path.
#define FAST_READ(reg, out)															\
	{																				\
		const uint32 _g = (reg);													\
		switch (_g)																	\
		{																			\
			case 0x01: out = (uint32) (mul >> 24) & 0xffffff; break;				\
			case 0x02: out = (uint32) mul & 0xffffff; break;						\
			case 0x03: out = mdr; break;											\
			case 0x08: out = rom; break;											\
			case 0x0c: out = ram; break;											\
			case 0x13: out = mar; break;											\
			case 0x1c: out = dpr; break;											\
			case 0x20: out = pc; break;												\
			case 0x28: out = p; break;												\
			case 0x2e: case 0x2f:													\
				r.bus_on = 1; r.bus_reading = 1;									\
				r.bus_pending = (_g == 0x2e) ? r.wait_rom : r.wait_ram;				\
				r.bus_addr = mar; out = 0; leave = true; break;						\
			default:																\
				out = (_g >= 0x50 && _g <= 0x5f) ? kConstants[_g & 15] : (_g >= 0x60) ? r.gpr[_g & 15] : 0; \
		}																			\
	}
#define FAST_WRITE(reg, value)														\
	{																				\
		const uint32 _g = (reg), _d = (value) & 0xffffff;							\
		switch (_g)																	\
		{																			\
			case 0x01: mul = (mul & 0xffffff) | ((uint64) _d << 24); break;			\
			case 0x02: mul = (mul & 0xffffff000000ull) | _d; break;					\
			case 0x03: mdr = _d; break;												\
			case 0x08: rom = _d; break;												\
			case 0x0c: ram = _d; break;												\
			case 0x13: mar = _d; break;												\
			case 0x1c: dpr = _d; break;												\
			case 0x20: pc = (uint8) _d; break;										\
			case 0x28: p = _d & 0x7fff; break;										\
			case 0x2e: case 0x2f:													\
				r.bus_on = 1; r.bus_writing = 1;									\
				r.bus_pending = (_g == 0x2e) ? r.wait_rom : r.wait_ram;				\
				r.bus_addr = mar; leave = true; break;								\
			default: if (_g >= 0x60) r.gpr[_g & 15] = _d;							\
		}																			\
	}
#define FAST_NZ(x)		(fn = (uint8) (((x) >> 23) & 1), fz = (uint8) ((x) == 0), (x))
#define FAST_ADD(x, y)	(_t = (int32) (x) + (int32) (y), fc = (uint8) (_t > 0xffffff),	\
						 fv = (uint8) ((~((x) ^ (y)) & ((x) ^ (uint32) _t) & 0x800000) != 0), FAST_NZ((uint32) _t & 0xffffff))
#define FAST_SUB(x, y)	(_t = (int32) (x) - (int32) (y), fc = (uint8) (_t >= 0),	\
						 fv = (uint8) ((~((x) ^ (y)) & ((x) ^ (uint32) _t) & 0x800000) != 0), FAST_NZ((uint32) _t & 0xffffff))

	while (budget > 0 && pc < 0xfe && !leave)
	{
		const uint16	op = page[pc];
		pc++;
		budget--;

		const uint32	reg = op & 0x7f;
		const uint32	imm = op & 0xff;
		const uint32	sub = (op >> 8) & 3;
		int32			_t;
		uint32			v, x;
#define SA	((a << shifts[sub]) & 0xffffff)

		switch (op >> 10)
		{
			case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
			case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
			{
				const uint32	k = (op >> 10) & 7;
				const bool		take = (k == 2) || (k == 3 && fz) || (k == 4 && fc) || (k == 5 && fn) || (k == 6 && fv);
				if (!take)
					break;
				if (op & 0x2000)	// CALL: Push
				{
					memmove(r.stack + 1, r.stack, 7 * sizeof(r.stack[0]));
					r.stack[0] = ((uint32) pb << 8) | pc;
				}
				if (op & 0x200)
				{
					pb = p;
					leave = true;	// the next fetch needs Cache()
				}
				pc = (uint8) op;
				budget -= 2;
				break;
			}

			case 0x07:	break;	// WAIT: no bus access in flight here

			case 0x09:		// SKIP
			{
				const uint8	flag = (sub == 0) ? fv : (sub == 1) ? fc : (sub == 2) ? fz : fn;
				if (flag == (op & 1))
				{
					pc++;		// pc < 0xff here, so no page crossing
					budget--;
				}
				break;
			}

			case 0x0f:		// RET: Pull
			{
				uint32	t = r.stack[0];
				memmove(r.stack, r.stack + 1, 7 * sizeof(r.stack[0]));
				r.stack[7] = 0;
				pb = (t >> 8) & 0x7fff;
				pc = (uint8) t;
				budget -= 2;
				leave = true;
				break;
			}

			case 0x10: mar = (mar + 1) & 0xffffff; break;

			case 0x12: FAST_READ(reg, x); FAST_SUB(x, SA); break;
			case 0x13: FAST_SUB(imm, SA); break;
			case 0x14: FAST_READ(reg, x); FAST_SUB(SA, x); break;
			case 0x15: FAST_SUB(SA, imm); break;

			case 0x16:
				if (sub == 1)
				{
					v = (uint32) (int32) (int8) a & 0xffffff;
					a = FAST_NZ(v);
				}
				else
				if (sub == 2)
				{
					v = (uint32) (int32) (int16) a & 0xffffff;
					a = FAST_NZ(v);
				}
				break;

			case 0x18:
				switch (sub)
				{
					case 0: FAST_READ(reg, a); break;
					case 1: FAST_READ(reg, mdr); break;
					case 2: FAST_READ(reg, mar); break;
					case 3: p = r.gpr[op & 15] & 0x7fff; break;
				}
				break;

			case 0x19:
				switch (sub)
				{
					case 0: a = imm; break;
					case 1: mdr = imm; break;
					case 2: mar = imm; break;
					case 3: p = imm; break;
				}
				break;

			case 0x1a: case 0x1b:
				if (sub != 3)
					SetByte(ram, sub, r.dram[DRAMIndex((op & 0x400) ? dpr + imm : a)]);
				break;

			case 0x1c: rom = drom[a & 0x3ff]; break;
			case 0x1d: rom = drom[op & 0x3ff]; break;

			case 0x1f:
				if (sub == 0)
					p = (p & 0x7f00) | imm;
				else
				if (sub == 1)
					p = (p & 0x00ff) | ((op & 0x7f) << 8);
				break;

			case 0x20: FAST_READ(reg, x); a = FAST_ADD(SA, x); break;
			case 0x21: a = FAST_ADD(SA, imm); break;
			case 0x22: FAST_READ(reg, x); a = FAST_SUB(x, SA); break;
			case 0x23: a = FAST_SUB(imm, SA); break;
			case 0x24: FAST_READ(reg, x); a = FAST_SUB(SA, x); break;
			case 0x25: a = FAST_SUB(SA, imm); break;
			case 0x26: FAST_READ(reg, x); mul = Mul(a, x); break;
			case 0x27: mul = Mul(a, imm); break;
			case 0x28: FAST_READ(reg, x); v = (~SA ^ x) & 0xffffff; a = FAST_NZ(v); break;
			case 0x29: v = (~SA ^ imm) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2a: FAST_READ(reg, x); v = (SA ^ x) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2b: v = (SA ^ imm) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2c: FAST_READ(reg, x); v = (SA & x) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2d: v = (SA & imm) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2e: FAST_READ(reg, x); v = (SA | x) & 0xffffff; a = FAST_NZ(v); break;
			case 0x2f: v = (SA | imm) & 0xffffff; a = FAST_NZ(v); break;

			case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
			{
				uint32	n;
				if (op & 0x400)
					n = op & 0x1f;
				else
					FAST_READ(reg, n);
				n &= 31;
				if (n > 24)
					n = 0;
				switch ((op >> 11) & 3)
				{
					case 0:  v = a >> n; break;
					case 1:  v = (uint32) ((int32) SignExtend24(a) >> n); break;
					case 2:  v = (a >> n) | (a << (24 - n)); break;
					default: v = a << n; break;
				}
				v &= 0xffffff;
				a = FAST_NZ(v);
				break;
			}

			case 0x38:
				if (sub == 0)
					FAST_WRITE(reg, a)
				else
				if (sub == 1)
					FAST_WRITE(reg, mdr)
				break;

			case 0x3a: case 0x3b:
				if (sub != 3)
					r.dram[DRAMIndex((op & 0x400) ? dpr + imm : a)] = (uint8) (ram >> (sub * 8));
				break;

			case 0x3c:
			{
				uint32	t = a;
				a = r.gpr[op & 15];
				r.gpr[op & 15] = t;
				break;
			}

			case 0x3e: a = p = 0; ram = dpr = 0; break;

			case 0x3f:
				r.pc = pc;	// Halt() reads none of the locals
				halt_op = TRUE;
				Halt();
				leave = true;
				break;

			default: break;
		}
#undef SA
	}
#undef FAST_READ
#undef FAST_WRITE
#undef FAST_NZ
#undef FAST_ADD
#undef FAST_SUB

	r.budget = budget; r.mul = mul;
	r.a = a; r.mdr = mdr; r.rom = rom; r.ram = ram; r.mar = mar; r.dpr = dpr;
	r.pb = pb; r.p = p;
	r.pc = pc; r.n = fn; r.z = fz; r.c = fc; r.v = fv;
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

static inline bool Overclocked (void)
{
	return (ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8);
}

// Runs a job from its first clock to its HALT; false if it went any other way. `clocks` is what
// it ran before the HALT, so the CPU sees it once the chip is owed more than that.
static bool RunJob (int64 &clocks)
{
	int64	bus_from = -1;
	r.budget = AHEAD_CAP;
	ahead.n = 0;
	halt_op = FALSE;

	while (r.budget > 0 && !r.halt)
	{
		if (r.op | r.lock | r.suspend_on | r.cache_on | r.dma_on)
		{
			Main();
			continue;
		}

		const int64	before = r.budget;
		const uint8	was = r.bus_on;
		if (!was && r.pc < 0xfe)
			RunFast();
		else
			Execute();

		// A bus access starts on an instruction's last clock and reads busy until the one it ends in.
		if (!was && r.bus_on)
			bus_from = r.bus_pending ? AHEAD_CAP - r.budget - 1 : -1;
		else
		if (was && !r.bus_on && bus_from >= 0)
		{
			if (ahead.n == AHEAD_WINDOWS)
				return (false);
			ahead.busy[ahead.n][0] = bus_from;
			ahead.busy[ahead.n][1] = AHEAD_CAP - before;
			ahead.n++;
			bus_from = -1;
		}
	}

	clocks = AHEAD_CAP - r.budget - 1;
	return (r.halt && halt_op && r.budget > 0 && r.op == OP_NONE && !r.bus_on);
}

// Runs a cache preload or a DMA to its end, busy all the while; `clocks` before its last step.
static bool RunToIdle (int64 &clocks)
{
	int64	before = r.budget = AHEAD_CAP;
	ahead.n = 0;

	// A preload as Cache() and FillByte() run it, without a step a byte.
	if (r.cache_on && r.cache_preload && r.op == OP_NONE)
	{
		const uint32	address = (r.cache_base + r.pb * 512) & 0xffffff;
		int64			c = 0;
		r.cache_preload = 0;
		r.cache_tag[r.cache_page] = address;
		r.fill_page = r.cache_page;
#ifdef LSB_FIRST
		const uint32	lin = ((address & 0x3f0000) >> 1) | (address & 0x7fff);
		if (IsROM(address) && (address & 0x7fff) <= 0x7e00 && lin + 512 <= Memory.CalculatedSize)
		{
			// one ROM page: all bytes at the same wait
			memcpy(r.prog[r.fill_page], Memory.ROM + lin, 512);
			clocks = 511 * (1 + r.wait_rom);
		}
		else
#endif
		for (uint32 i = 0; i < 512; i++)
		{
			const uint32	a = (address + i) & 0xffffff;
			clocks = c;
			c += Wait(a);
			const uint8		byte = BusRead(a);
			uint16			&word = r.prog[r.fill_page][i >> 1];
			word = (i & 1) ? (word & 0x00ff) | (byte << 8) : (word & 0xff00) | byte;
		}
		r.fill_addr = (address + 512) & 0xffffff;
		r.fill_pos = 512;
		r.cache_on = 0;
		hit_key = ~0u;
		return (true);
	}

	while (r.budget > 0 && !Idle())
	{
		before = r.budget;
		Main();
	}

	clocks = AHEAD_CAP - before;
	return (r.budget > 0 && r.halt && !r.lock && !Busy());
}

// A DMA run ahead mustn't reach the registers (or cart RAM, which natives run without).
static bool DmaAhead (void)
{
	for (uint32 i = 0; i < r.dma_len; i++)
		if (IsIO((r.dma_dst + i) & 0xffffff))
			return (false);
	return (true);
}

// Runs a job (r just out of halt), a preload or a DMA to its end, or leaves it to run as the CPU goes.
static void StartAhead (uint8 kind)
{
	ahead.start = r;
	const bool8	irq = CPU.IRQExternal;
	int64		clocks;
	bool		ok;

	if (kind == AHEAD_JOB)
	{
		S9xHG51BJob	job = { 0, ahead.busy, 0, AHEAD_WINDOWS };
		ok = S9xHG51BNativeJob(r, drom, job);
		hit_key = ~0u;
		if (ok)
		{
			clocks = job.clocks;
			ahead.n = job.n;
		}
		else
		{
			r = ahead.start;
			ok = RunJob(clocks);
		}
	}
	else
		ok = RunToIdle(clocks);

	CPU.IRQExternal = irq;
	// The overclocked chip finishes on the CPU's next access, unless it takes a million steps.
	if (Overclocked() && clocks >= 1000000)
		ok = false;
	if (!ok)
	{
		r = ahead.start;
		hit_key = ~0u;
		return;
	}

	// In scaled clocks: the chip has run past clock k once elapsed * CX4_HZ + rem >= (k + 1 - budget) * master.
	const int64		b = ahead.start.budget;
	const uint64	master = Settings.PAL ? 21281370 : 21477273;
	const uint64	done = (uint64) (clocks + 1 - b) * master;
	ahead.master = master;
	ahead.t_done = r.synced + (done - r.rem + CX4_HZ - 1) / CX4_HZ;
	for (uint32 i = 0; i < ahead.n; i++)
	{
		ahead.busy[i][0] = (ahead.busy[i][0] + 1 - b) * master;
		ahead.busy[i][1] = (ahead.busy[i][1] + 1 - b) * master;
	}
	ahead.cur = 0;
	ahead.kind = kind;
	ahead.status = ahead.start.suspend_on | (ahead.start.i << 1) | (kind == AHEAD_JOB ? 0x40 : 0xc0);
	r.budget = b;
	ahead.start.fresh = kind;
	ahead.on = true;
	poll.pbpc = ~0u;
}

// Back to the chip the CPU has seen so far, to run the job as it goes.
static void DropAhead (void)
{
	const uint64	line_base = r.line_base;
	ahead.on = viewed = false;
	r = ahead.start;
	r.fresh = 0;
	r.line_base = line_base;
	hit_key = ~0u;
}

// The CPU sees the end, at t: the chip as SyncTo would leave it there.
static void EndAhead (uint64 t)
{
	const uint64	x = (t - r.synced) * CX4_HZ + r.rem;
	ahead.on = viewed = false;
	r.synced = t;
	r.rem = x % ahead.master;
	r.budget = 0;
	if (ahead.kind == AHEAD_JOB)
		Halt();
	parked = true;
	parked_t = t;
}

// A status read meanwhile: a job reads busy while one of its bus accesses is in flight.
static uint8 StatusAhead (uint64 t)
{
	uint8	busy = 0;
	if (t > r.synced && ahead.n)
	{
		const uint64	x = (t - r.synced) * CX4_HZ + r.rem;
		while (ahead.cur < ahead.n && x >= ahead.busy[ahead.cur][1])
			ahead.cur++;
		busy = ahead.cur < ahead.n && x >= ahead.busy[ahead.cur][0];
	}
	return (ahead.status | (busy << 7));
}

static inline bool IsStatus (uint32 address)
{
	address = 0x7c00 | (address & 0x3ff);
	return (address >= 0x7f53 && address <= 0x7f5f && address != 0x7f58 && address != 0x7f5a);
}

// The CPU's wait for the chip: LDA $7F5E; AND #$40; BNE back, in WRAM, with an 8-bit A. PBPC is
// past the LDA's operand.
static bool PollLoop (uint32 pbpc)
{
	static const uint8	loop[8] = { 0xaf, 0x5e, 0x7f, 0x00, 0x29, 0x40, 0xd0, 0xf8 };
	const uint32	a = pbpc & 0x1ffff;
	return ((pbpc >> 17) == (0x7e >> 1) && a >= 4 && a <= 0x1fffc && CheckMemory() &&
			!memcmp(Memory.RAM + a - 4, loop, sizeof(loop)));
}

// While a job runs ahead, whole turns of that wait pass at once, short of the next event, IRQ timer
// or the job's end, and never with an interrupt or HDMA due; a turn is two reads apart, twice alike.
static void SkipPolls (int32 speed)
{
	const int32		c = CPU.Cycles;
	const uint32	pbpc = Registers.PBPC, key = pbpc | (CheckEmulation() ? 0x80000000 : 0);
	const int32		period = (pbpc == poll.pbpc && r.line_base == poll.line && CPU.NextEvent == poll.next) ? c - poll.cycles : 0;
	poll.pbpc = pbpc;
	poll.cycles = c;
	poll.line = r.line_base;
	poll.next = CPU.NextEvent;

	int	s = 0;
	while (s < 8 && poll.site[s] != key)
		s++;
	if (period > 0 && period == poll.period && s == 8)
	{
		memmove(poll.site + 1, poll.site, 7 * sizeof(poll.site[0]));
		memmove(poll.turn + 1, poll.turn, 7 * sizeof(poll.turn[0]));
		poll.site[0] = key;
		poll.turn[0] = period;
		s = 0;
	}
	poll.period = period;
	// a gap other than the turn had something more in it (an interrupt): not now
	if (s == 8 || (period > 0 && period != poll.turn[s]) || poll.turn[s] <= 0)
		return;
	const int32	turn = poll.turn[s];

	if (CPU.InDMAorHDMA || CPU.HDMAEdge || CPU.NMIPending || CPU.IRQDeferOne || Timings.IRQFlagChanging ||
		((CPU.IRQLine || CPU.IRQExternal) && !CheckFlag(IRQ)) || Settings.SA1 || !PollLoop(Registers.PBPC))
		return;
#ifdef DEBUGGER
	if (CPU.Flags & (BREAK_FLAG | TRACE_FLAG | SINGLE_STEP_FLAG | DEBUG_MODE_FLAG))
		return;
#endif

	int64	room = (int64) (CPU.NextEvent < Timings.NextIRQTimer ? CPU.NextEvent : Timings.NextIRQTimer) - 1 - c;
	const int64	end = (int64) (ahead.t_done - (r.line_base + (uint64) (int64) (c + speed))) - 1;
	if (end < room)
		room = end;
	if (room < turn)
		return;

	const int32	skip = (int32) (room / turn) * turn;
	CPU.Cycles += skip;
	CPU.LastBusStart += skip;
	CPU.LastRunStart += skip;
	poll.cycles = CPU.Cycles;
}

// The syncs an idle chip put off, as one: from idle it only owes clocks, and SyncTo's sums come
// out the same however the time between is split.
static void Unpark (void)
{
	if (!parked)
		return;
	parked = false;
	if (parked_t <= r.synced)
		return;
	const uint64	master = Settings.PAL ? 21281370 : 21477273;
	const uint64	acc = (parked_t - r.synced) * CX4_HZ + r.rem;
	r.synced = parked_t;
	r.budget += (int64) (acc / master);
	r.rem = acc % master;
	if (r.budget > 0)
		r.budget = 0;
}

static void SyncTo (int32 cycles)
{
	uint64	t = r.line_base + (uint64) (int64) cycles;
	if (t <= r.synced)
		return;
	if (parked)
	{
		if (!Overclocked())
		{
			if (t > parked_t)
				parked_t = t;
			return;
		}
		Unpark();
	}
	if (ahead.on)
	{
		if (t >= ahead.t_done || Overclocked())
			EndAhead(t);
		return;
	}

	viewed = false;
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
		if (!(r.op | r.lock | r.suspend_on | r.cache_on | r.dma_on | r.halt))
		{
			if (!r.bus_on && r.pc < 0xfe)
				RunFast();
			else
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
	if (Idle())
	{
		parked = true;
		parked_t = t;
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
	if (ahead.on)
	{
		if ((address & 0x0c00) == 0x0c00 && IsStatus(address))
		{
			if (speed >= 0)
				SkipPolls(speed);
			return (StatusAhead(r.line_base + CPU.Cycles + speed));
		}
		// a peek sees data RAM as the job found it
		if (speed < 0 && (address & 0x0c00) != 0x0c00)
			return (ahead.start.dram[address & 0xfff]);
		if (speed >= 0)
		{
			DropAhead();
			SyncTo(CPU.Cycles + speed);
		}
	}
	if ((address & 0x0c00) != 0x0c00)
		return (r.dram[address & 0xfff]);
	return (ReadIO(address));
}

void S9xHG51BWrite (uint8 byte, uint16 address, int32 speed)
{
	viewed = false;
	if ((address & 0x0c00) != 0x0c00)
	{
		if (speed >= 0)
			SyncTo(CPU.Cycles + speed);
		if (ahead.on)
		{
			DropAhead();
			if (speed >= 0)
				SyncTo(CPU.Cycles + speed);
		}
		r.dram[address & 0xfff] = byte;
		return;
	}

	// A cheat's poke reaches data RAM only.
	if (speed < 0)
		return;
	SyncTo(CPU.Cycles + speed);
	Unpark();
	if (ahead.on)
	{
		DropAhead();
		SyncTo(CPU.Cycles + speed);
	}

	// From an idle halt the natives run a job, a preload or a DMA ahead.
	const uint32	reg = 0x7c00 | (address & 0x3ff);
	const bool		idle = native && r.halt && r.op == OP_NONE && !r.bus_on && !r.cache_on && !r.dma_on &&
						   !r.lock && !r.suspend_on && !Memory.SRAMSize;
	WriteIO(address, byte);
	if (!idle)
		return;
	if (reg == 0x7f4f && !r.halt)
		StartAhead(AHEAD_JOB);
	else
	if ((reg == 0x7f48 && r.cache_on) || (reg == 0x7f47 && r.dma_on && DmaAhead()))
		StartAhead(AHEAD_IDLE);
}

uint8 * S9xHG51BDMABase (uint16 address)
{
	// the sync the DMA's first byte would make: idle, the chip parks there
	if (!parked && !ahead.on)
		SyncTo(CPU.Cycles);
	if (!parked)
		return (NULL);
	if (!viewed)
	{
		memcpy(view, r.dram, 0xc00);
		for (uint32 a = 0xc00; a < 0x1000; a++)
			view[a] = ReadIO(a);
		viewed = true;
	}
	return (view - (address & 0xf000));
}

uint32 S9xHG51BStateSize (void)
{
	return (sizeof(r));
}

// Run ahead, the chip is saved as it started, which runs on the same from there.
void S9xHG51BStateSave (uint8 *buf)
{
	Unpark();
	if (!ahead.on)
	{
		memcpy(buf, &r, sizeof(r));
		return;
	}
	memcpy(buf, &ahead.start, sizeof(r));
	memcpy(buf + offsetof(Regs, line_base), &r.line_base, sizeof(r.line_base));
}

bool8 S9xHG51BStateLoad (const uint8 *buf, uint32 size)
{
	if (!loaded || size != sizeof(r))
		return (FALSE);
	memcpy(&r, buf, sizeof(r));
	hit_key = ~0u;
	active = TRUE;
	ahead.on = false;
	parked = viewed = false;
	poll.pbpc = ~0u;
	const uint8	fresh = r.fresh;
	r.fresh = 0;
	if (native && fresh == AHEAD_JOB && !r.halt)
		StartAhead(AHEAD_JOB);
	else
	if (native && fresh == AHEAD_IDLE && (r.cache_on || r.dma_on))
		StartAhead(AHEAD_IDLE);
	return (TRUE);
}
