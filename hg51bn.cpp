/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native Cx4 jobs (Mega Man X2, X3): each program's work in plain code, every register it leaves
// and its clock count as the chip's, checked against the interpreter job by job.

#include <string.h>
#include "snes9x.h"
#include "memmap.h"
#include "hg51bn.h"

typedef S9xHG51BRegs	Regs;

// The ALU's flags, set as the chip sets them.
struct Flags
{
	uint8	n, z, c, v;
};

static inline uint32 NZ (Flags &f, uint32 x)
{
	x &= 0xffffff;
	f.n = (uint8) (x >> 23);
	f.z = (x == 0);
	return (x);
}

static inline uint32 Add (Flags &f, uint32 x, uint32 y)
{
	const int32	s = (int32) x + (int32) y;
	f.c = (s > 0xffffff);
	f.v = (~(x ^ y) & (x ^ (uint32) s) & 0x800000) != 0;
	return (NZ(f, (uint32) s));
}

static inline uint32 Sub (Flags &f, uint32 x, uint32 y)
{
	const int32	s = (int32) x - (int32) y;
	f.c = (s >= 0);
	f.v = (~(x ^ y) & (x ^ (uint32) s) & 0x800000) != 0;
	return (NZ(f, (uint32) s));
}

// SHR by a register: counts past 24 shift by nothing.
static inline uint32 Shr (Flags &f, uint32 x, uint32 s)
{
	s &= 31;
	return (NZ(f, s > 24 ? x : x >> s));
}

static inline uint32 Sext8 (uint32 x)
{
	return ((uint32) (int32) (int8) x & 0xffffff);
}

static inline uint32 Sext16 (uint32 x)
{
	return ((uint32) (int32) (int16) x & 0xffffff);
}

// Data RAM by a 12-bit address: $C00-$FFF mirror $800-$BFF.
static inline uint32 Index (uint32 a)
{
	a &= 0xfff;
	return (a >= 0xc00 ? a - 0x400 : a);
}

// One job's run: the chip, the clocks so far, and whether it stayed on paths the natives know.
struct Run
{
	Regs		&r;
	S9xHG51BJob	&job;
	const uint32	*drom;
	int64		clk;
	bool		ok;
};

// The chip's own bus, as hg51b.cpp maps it. Natives run only without cart RAM, and its
// registers aren't modelled here.
static uint8 Bus (Run &k, uint32 a)
{
	if ((a & 0x408000) == 0x008000 || (a & 0xc00000) == 0xc00000)
	{
		uint32	lin = ((a & 0x3f0000) >> 1) | (a & 0x7fff);
		if (lin >= Memory.CalculatedSize)
			lin = Memory.map_mirror(Memory.CalculatedSize, lin);
		return (Memory.ROM[lin]);
	}
	if ((a & 0xf88000) == 0x700000)
		return (0);
	if ((a & 0x40e000) == 0x006000 && (a & 0x0c00) != 0x0c00)
		return (k.r.dram[a & 0xfff]);
	if ((a & 0x40ec00) == 0x006c00)
		k.ok = false;
	return (0);
}

// LD MDR,BUSROM; INC MAR; WAIT: the byte, after 3 clocks plus what the ROM's wait states add.
// The status reads busy from the LD's end to the instruction the read ends in.
static inline uint8 Read (Run &k, uint32 a)
{
	const uint32	w = k.r.wait_rom;
	if (w)
	{
		if (k.job.n == k.job.max)
			k.ok = false;
		else
		{
			k.job.busy[k.job.n][0] = (uint64) k.clk;
			k.job.busy[k.job.n][1] = (uint64) k.clk + (w >= 2 ? 2 : 1);
			k.job.n++;
		}
	}
	k.clk += 3 + (w > 2 ? w - 2 : 0);
	return (Bus(k, a));
}

// Into page pb: found in the cache, or filled into the page not in use, a byte at a time.
// False where the chip would halt instead, both pages locked.
static bool Enter (Run &k, uint32 pb)
{
	Regs			&r = k.r;
	const uint32	address = (r.cache_base + pb * 512) & 0xffffff;

	if (r.cache_tag[r.cache_page] == address)
		return (true);
	r.cache_page ^= 1;
	if (r.cache_tag[r.cache_page] == address)
		return (true);
	if (r.cache_lock[r.cache_page])
		r.cache_page ^= 1;
	if (r.cache_lock[r.cache_page])
		return (false);

	r.cache_tag[r.cache_page] = address;
	r.fill_page = r.cache_page;
	r.fill_addr = (address + 512) & 0xffffff;
	r.fill_pos = 512;
#ifdef LSB_FIRST
	// one ROM page: all bytes at the same wait
	const uint32	lin = ((address & 0x3f0000) >> 1) | (address & 0x7fff);
	if (((address & 0x408000) == 0x008000 || (address & 0xc00000) == 0xc00000) && (address & 0x7fff) <= 0x7e00 &&
		lin + 512 <= Memory.CalculatedSize)
	{
		memcpy(r.prog[r.cache_page], Memory.ROM + lin, 512);
		k.clk += 512 * (1 + r.wait_rom);
		return (true);
	}
#endif
	for (uint32 i = 0; i < 512; i++)
	{
		const uint32	a = (address + i) & 0xffffff;
		if ((a & 0x408000) == 0x008000 || (a & 0xc00000) == 0xc00000)
			k.clk += 1 + r.wait_rom;
		else
		if ((a & 0xf88000) == 0x700000)
			k.clk += 1 + r.wait_ram;
		const uint8	byte = Bus(k, a);
		uint16	&word = r.prog[r.cache_page][i >> 1];
		word = (i & 1) ? (word & 0x00ff) | (byte << 8) : (word & 0xff00) | byte;
	}
	return (true);
}

static inline void Halt (Regs &r)
{
	r.halt = 1;
	if (!r.irq)
		r.i = 1;
}

#define D(x)	r.dram[Index(x)]

// 00:00, the sprite builder: the $620 sprites at $220 to OAM entries from $627 through their ROM tile
// lists, high-table bits from $200 + $629, the rest of the 128 off screen.
static void Sprites (Run &k)
{
	Regs	&r = k.r;
	uint32	*g = r.gpr;
	Flags	f = { r.n, r.z, r.c, r.v };
	uint32	a, ram = r.ram, mdr = r.mdr, mar = r.mar, dpr, bus = r.bus_addr;
	int64	&clk = k.clk;

	a = Add(f, 0x600, 0x20);
	dpr = a;
	ram = (ram & 0xff0000) | D(0x627) | (D(0x628) << 8);
	a = NZ(f, ram & 0xffff);
	g[2] = a;
	ram = (ram & 0xffff00) | D(0x620);
	a = NZ(f, ram & 0xff);
	clk += 13;
	if (f.z)
	{
		clk += 2;
		goto hide;
	}

	g[13] = a;
	ram = (ram & 0xffff00) | D(0x629);
	g[3] = NZ(f, ram & 0xff);
	g[3] = Add(f, 0x200, g[3]);
	ram = (ram & 0xffff00) | D(0x625);
	g[4] = ram;
	ram = (ram & 0xffff00) | D(0x626);
	g[5] = ram;
	a = Add(f, 0x200, 0x20);
	dpr = a;
	g[15] = a;
	clk += 19;

	for (;;)	// a sprite at dpr
	{
		ram = (ram & 0xffff00) | D(dpr + 4);
		g[14] = NZ(f, ram & 0x3f);
		ram = (ram & 0xffff00) | D(dpr + 5);
		g[7] = NZ(f, ram & 0xff);
		ram = (ram & 0xffff00) | D(0x621);
		ram = (ram & 0xff00ff) | (D(0x622) << 8);
		g[8] = NZ(f, Sext16(ram));
		ram = (ram & 0xff0000) | D(dpr) | (D(dpr + 1) << 8);
		g[8] = Sub(f, NZ(f, Sext16(ram)), g[8]);
		ram = (ram & 0xffff00) | D(0x623);
		ram = (ram & 0xff00ff) | (D(0x624) << 8);
		g[9] = NZ(f, Sext16(ram));
		ram = (ram & 0xff0000) | D(dpr + 2) | (D(dpr + 3) << 8);
		g[9] = Sub(f, NZ(f, Sext16(ram)), g[9]);
		ram = D(dpr + 7) | (D(dpr + 8) << 8) | (D(dpr + 9) << 16);
		mar = ram;
		ram = (ram & 0xffff00) | D(dpr + 6);
		g[12] = ram;
		dpr = g[2];
		clk += 48;
		mdr = Read(k, bus = mar);
		mar = (mar + 1) & 0xffffff;
		g[1] = mar;
		a = g[10] = NZ(f, mdr & 0xff);
		clk += 5;

		for (;;)	// a tile at mar: attributes, x, y, tile number
		{
			mdr = Read(k, bus = mar);
			mar = (mar + 1) & 0xffffff;
			g[11] = mdr;
			NZ(f, mdr & 0x20);
			g[0] = NZ(f, Sext8(f.z ? 0xf8 : 0xf0));
			clk += 8;

			mdr = Read(k, bus = mar);
			mar = (mar + 1) & 0xffffff;
			Add(f, g[12] & 0x40, 0xffffff);
			a = NZ(f, Sext8(mdr));
			clk += 6;
			if (f.c)	// flipped: the tile's own width back
			{
				a = Add(f, NZ(f, a ^ 0xffffff), 1);
				a = Add(f, a, g[0]);
				clk += 4;
			}
			else
				clk += 3;
			a = Add(f, a, g[8]);
			g[6] = a;
			ram = a;
			D(dpr) = (uint8) ram;
			clk += 4;

			mdr = Read(k, bus = mar);
			mar = (mar + 1) & 0xffffff;
			Add(f, g[12] & 0x80, 0xffffff);
			a = NZ(f, Sext8(mdr));
			clk += 6;
			if (f.c)
			{
				a = Add(f, NZ(f, a ^ 0xffffff), 1);
				a = Add(f, a, g[0]);
				clk += 4;
			}
			else
				clk += 3;
			a = Add(f, a, g[9]);
			ram = a;
			D(dpr + 1) = (uint8) ram;
			a = Add(f, a, 0x0f);
			Sub(f, a, 0xef);
			clk += 6;
			if (f.c)	// off screen
			{
				clk += 2;
				goto next;
			}

			mdr = Read(k, bus = mar);
			mar = (mar + 1) & 0xffffff;
			ram = a = Add(f, mdr, g[7]);
			D(dpr + 2) = (uint8) ram;
			g[0] = NZ(f, g[12] & 0xc0);
			a = NZ(f, g[11] & 0xce);
			a = NZ(f, a | g[14]);
			ram = a = NZ(f, a ^ g[0]);
			D(dpr + 3) = (uint8) ram;
			a = NZ(f, g[4] >> 2);
			a = g[4] = NZ(f, a & 0x3f);
			NZ(f, g[6] & 0x100);
			if (!f.z)
				a = NZ(f, a | 0x40);
			a = g[4] = a;
			NZ(f, g[11] << 18);
			a = g[4];
			if (f.n)
				a = NZ(f, a | 0x80);
			g[4] = a;
			a = g[5] = Add(f, g[5], 1);
			a = NZ(f, a & 3);
			clk += 34;
			if (!f.z)
			{
				a = dpr;
				clk += 4;
			}
			else	// four entries' bits done: the high-table byte
			{
				a = g[3];
				g[3] = dpr;
				dpr = a;
				ram = g[4];
				D(dpr) = (uint8) ram;
				a = Add(f, dpr, 1);
				const uint32	t = a;
				a = g[3];
				g[3] = t;
				clk += 13;
			}
			a = Add(f, a, 4);
			dpr = a;
			g[2] = a;
			a = NZ(f, a << 14);
			clk += 5;
			if (f.n)	// OAM full
			{
				clk += 2;
				goto halt;
			}

		next:
			a = Add(f, g[1], 4);
			g[1] = a;
			mar = a;
			a = g[10] = Sub(f, g[10], 1);
			clk += 8;
			if (!f.z)
			{
				clk += 3;
				continue;
			}
			clk++;
			break;
		}

		a = Add(f, g[15], 0x10);
		g[15] = a;
		dpr = a;
		a = g[13] = Sub(f, g[13], 1);
		clk += 8;
		if (!f.z)
		{
			clk += 3;
			continue;
		}
		clk++;
		break;
	}

	// the high-table byte four entries didn't finish
	a = Sub(f, 4, g[5]);
	a = NZ(f, (a << 1) & 6);
	clk += 4;
	if (f.z)
		clk += 2;
	else
	{
		g[0] = a;
		ram = a = Shr(f, g[4], g[0]);
		a = g[3];
		dpr = a;
		D(dpr) = (uint8) ram;
		clk += 7;
	}

hide:
	a = g[2];
	dpr = a;
	a = Sub(f, 0x7f, NZ(f, a >> 2));
	clk += 5;
	if (!f.c)
	{
		clk += 3;
		goto halt;
	}
	clk++;
	g[0] = a;
	ram = a = 0xf0;
	clk += 3;
	for (;;)
	{
		D(dpr + 1) = 0xf0;
		a = Add(f, dpr, 4);
		dpr = a;
		a = g[0] = Sub(f, g[0], 1);
		clk += 8;
		if (f.n)
		{
			clk++;
			break;
		}
		clk += 3;
	}

halt:
	r.a = a;
	r.ram = ram;
	r.mdr = mdr;
	r.mar = mar;
	r.dpr = dpr;
	r.bus_addr = bus;
	r.n = f.n;
	r.z = f.z;
	r.c = f.c;
	r.v = f.v;
	r.pc = 0xed;
	Halt(r);
}

#undef D

static inline uint64 Mul (uint32 x, uint32 y)
{
	const int64	p = (int64) ((int32) (x << 8) >> 8) * ((int32) (y << 8) >> 8);
	return ((uint64) p & 0xffffffffffffull);
}

// A program's working registers, held apart from Regs while it runs, and its calls and returns.
struct Cpu
{
	Run		&k;
	Regs	&r;
	uint32	*g;
	int64	&clk;
	Flags	f;
	uint32	a, ram, mdr, mar, dpr, rom, bus;
	uint64	mul;
	uint32	p, pb;
	uint16	pbs[8];		// the pages calls return to
	int		depth, deepest;

	Cpu (Run &run) : k(run), r(run.r), g(run.r.gpr), clk(run.clk), depth(0), deepest(0)
	{
		f.n = r.n; f.z = r.z; f.c = r.c; f.v = r.v;
		a = r.a; ram = r.ram; mdr = r.mdr; mar = r.mar; dpr = r.dpr; rom = r.rom; bus = r.bus_addr;
		mul = r.mul; p = r.p; pb = r.pb;
	}

	// Back into Regs, halted by the HALT before pc.
	void Halt (uint8 pc)
	{
		r.n = f.n; r.z = f.z; r.c = f.c; r.v = f.v;
		r.a = a; r.ram = ram; r.mdr = mdr; r.mar = mar; r.dpr = dpr; r.rom = rom; r.bus_addr = bus;
		r.mul = mul; r.p = (uint16) p; r.pb = (uint16) pb; r.pc = pc;
		// calls d deep push the stack's bottom d entries out, and their returns pull in 0s
		for (int i = 8 - deepest; i < 8; i++)
			r.stack[i] = 0;
		::Halt(r);
	}

	uint8 &D (uint32 x)
	{
		return (r.dram[Index(x)]);
	}

	// RDRAM byte,DPR+x
	void Get (int byte, uint32 x)
	{
		ram = (ram & ~(0xffu << (byte * 8))) | ((uint32) D(x) << (byte * 8));
	}

	void Read (void)
	{
		bus = mar;
		mdr = ::Read(k, mar);
		mar = (mar + 1) & 0xffffff;
	}

	// CALL into page to (3 clocks), and RET (3). The return pc is the caller's to know.
	void Call (uint32 to, uint8 ret)
	{
		(void) ret;
		if (depth == 8)
			k.ok = false;
		else
			pbs[depth++] = (uint16) pb;
		if (depth > deepest)
			deepest = depth;
		clk += 3;
		if (to != pb && !Enter(k, pb = to))
			k.ok = false;
	}

	void Ret (void)
	{
		const uint32	to = depth ? pbs[--depth] : pb;
		clk += 3;
		if (to != pb && !Enter(k, pb = to))
			k.ok = false;
	}

	// LD A,x; MUL A,y; nop; LD A,MULL; SHR A,#10; ST t,A; LD A,MULH; ADD A<<8,t: the product's
	// bits 16-39 (8 clocks, counted by the caller)
	uint32 Fix (uint32 x, uint32 y, uint32 &t)
	{
		mul = Mul(x, y);
		t = NZ(f, ((uint32) mul & 0xffffff) >> 16);
		return (a = Add(f, ((uint32) (mul >> 24) << 8) & 0xffffff, t));
	}

	uint32 Neg (uint32 x)
	{
		return (Add(f, NZ(f, x ^ 0xffffff), 1));
	}
};

// A:8c, an angle (A's low byte, 128 to the circle) to sine and cosine as 1.15 fractions in R10 and
// R11, from the data ROM's quarter waves: A = R10.
static void SinCos (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.p, ret);
	m.a = NZ(f, NZ(f, m.a & 0xff) << 2);
	g[15] = m.a;
	m.ram = m.a = NZ(f, m.a & 0x7f);
	m.a = NZ(f, g[15] & 0x80);
	m.clk += 8;
	if (f.z)
	{
		m.a = m.ram;
		m.clk += 3;
	}
	else	// the second eighth runs back
	{
		m.ram = m.a = Sub(f, 0x80, m.ram);
		m.clk += 6;
	}
	Sub(f, m.a, 0x80);
	m.clk += 2;
	if (f.z)	// a quarter turn: past the tables' end
	{
		g[10] = m.a = NZ(f, 0xffffff >> 8);
		g[11] = m.a = 0;
		m.clk += 7;
	}
	else
	{
		m.a = NZ(f, Add(f, 0x200, 0) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[10] = m.a = NZ(f, m.rom >> 8);
		m.a = NZ(f, Add(f, 0x300, 0x80) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[11] = m.a = NZ(f, m.rom >> 8);
		m.clk += 17;
	}
	m.a = NZ(f, g[15] << 15);
	m.clk += 3;
	if (f.n)
	{
		g[10] = m.Neg(g[10]);
		m.clk += 5;
	}
	else
		m.clk += 3;
	Sub(f, m.a = NZ(f, Add(f, NZ(f, g[15] >> 7), 1) & 3), 2);
	m.clk += 6;
	if (f.c)
	{
		g[11] = m.Neg(g[11]);
		m.clk += 5;
	}
	else
		m.clk += 3;
	m.a = g[10];
	m.clk++;
	m.Ret();
}

// A:c4 (a vertex number from the edge list) or A:cd (its address in A): the vertex at $28:xxxx
// read into R2-R4, each 16 bits << 8; MAR is kept in R15 meanwhile.
static void Vertex (Cpu &m, uint8 ret, bool from_list)
{
	uint32	*g = m.g;
	uint32	a = m.a, mar = m.mar, b;
	int64	&clk = m.clk;

	m.Call(m.p, ret);
	if (from_list)
	{
		clk++;
		b = Read(m.k, mar);
		mar = (mar + 1) & 0xffffff;
		clk++;
		const uint32	lo = Read(m.k, mar);
		a = 0x280000 | (b << 8) | lo;
		b = lo;
		mar = (mar + 1) & 0xffffff;
		clk++;
	}
	g[15] = mar;
	mar = a;
	clk += 4;
	uint32	at = mar;
	for (int i = 2; i <= 4; i++)
	{
		const uint32	hi = Read(m.k, mar);
		mar = (mar + 1) & 0xffffff;
		clk++;
		at = mar;
		b = Read(m.k, mar);
		mar = (mar + 1) & 0xffffff;
		g[i] = ((hi << 8) | b) << 8;
		clk += 3;
	}
	// the last word's flags: its ADD carried nothing, then SHL
	m.f.n = (uint8) (g[4] >> 23);
	m.f.z = (g[4] == 0);
	m.f.c = m.f.v = 0;
	m.mdr = b;
	m.bus = at;
	m.mar = m.a = g[15];
	clk += 2;
	m.Ret();
}

// 8:61, a vertex (R2-R4, R4 less the depth R12) turned about x, y and z by the angles' sines
// and cosines in R6/R7, R8/R9 and R10/R11, then scaled by R5: R15 = $30 + x, A = $30 + y.
static void Project (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	uint32	x = g[2], y = g[3], z, t14 = g[14], t15 = g[15], x1, y1;
	uint64	mul;

	m.Call(m.pb, ret);
	// LD A,a; MUL A,b; nop; LD A,MULL; SHR A,#10; ST t,A; LD A,MULH; ADD A<<8,t
#define FIX(a, b, t)	(mul = Mul(a, b), t = ((uint32) mul & 0xffffff) >> 16, ((((uint32) (mul >> 24)) << 8) + t) & 0xffffff)
	z = (g[4] - g[12]) & 0xffffff;
	t14 = FIX(y, g[7], t14);
	y1 = (FIX(z, g[6], t15) + t14) & 0xffffff;
	t14 = FIX(y, g[6], t14);
	z = (FIX(z, g[7], t15) - t14) & 0xffffff;
	y = y1;
	t14 = FIX(x, g[9], t14);
	x1 = (t14 - FIX(z, g[8], t15)) & 0xffffff;
	t14 = FIX(x, g[8], t14);
	z = (FIX(z, g[9], t15) + t14) & 0xffffff;
	x = x1;
	t14 = FIX(x, g[11], t14);
	x1 = (FIX(y, g[10], t15) + t14) & 0xffffff;
	t14 = FIX(x, g[10], t14);
	y = (FIX(y, g[11], t15) - t14) & 0xffffff;
	x = x1;
	z = (z + g[12]) & 0xffffff;
#undef FIX
	g[2] = x;
	g[3] = y;
	g[4] = z;
	g[13] = x1;
	g[14] = t14;
	mul = Mul(x, g[5]);
	g[15] = (0x30 + ((uint32) (mul >> 24) & 0xffffff)) & 0xffffff;
	m.mul = Mul(y, g[5]);
	m.a = Add(m.f, 0x30, (uint32) (m.mul >> 24) & 0xffffff);
	m.clk += 135;
	m.Ret();
}

// A:f2, the minor axis' step per pixel along the major R13: R14 * ($800000 / R13 >> 15), in 8.8.
static void Slope (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.pb, ret);
	Sub(f, m.a = g[14], 0);
	m.clk += 3;
	if (f.z)
		m.clk += 2;
	else
	{
		m.a = Add(f, 0, g[13]);
		m.rom = m.k.drom[m.a & 0x3ff];
		m.mul = Mul(m.a = NZ(f, m.rom >> 15), g[14]);
		m.a = (uint32) m.mul & 0xffffff;
		m.clk += 8;
	}
	m.Ret();
}

// 8:e9: R6-R8 back from DPR+$0C-$14, MAR from R4.
static void Restore (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	const uint8	*d = m.r.dram;
	const uint32	dpr = m.dpr;

	m.Call(m.p, ret);
	m.mar = g[4];
	for (int i = 6; i <= 8; i++)
	{
		const uint32	at = dpr + 0x0c + (i - 6) * 3;
		g[i] = d[Index(at)] | (d[Index(at + 1)] << 8) | (d[Index(at + 2)] << 16);
	}
	m.ram = m.a = g[8];
	m.clk += 17;
	m.Ret();
}

// A:00, a line from (DPR+0, DPR+3) to (DPR+6, DPR+9) ORed into the 2bpp tiles at $300 (columns of
// $C0 bytes) in the colour bits R1 holds; R6-R8 and MAR are kept through it.
static void Line (Cpu &m, uint8 ret)
{
	uint32			*g = m.g;
	Flags			&f = m.f;
	uint8			*dram = m.r.dram;
	const uint32	dpr = m.dpr;
	int64			&clk = m.clk;

	m.Call(m.p, ret);
	for (int i = 6; i <= 8; i++)
	{
		const uint32	at = dpr + 0x0c + (i - 6) * 3;
		dram[Index(at)] = (uint8) g[i];
		dram[Index(at + 1)] = (uint8) (g[i] >> 8);
		dram[Index(at + 2)] = (uint8) (g[i] >> 16);
	}
	g[4] = m.mar;

	// |dy| in R14 and |dx| in R13, MAR's bits set where they run backwards; the major axis steps
	// by 1.0, the minor by the slope (A:f2)
	const uint32	y0 = dram[Index(dpr)], x0 = dram[Index(dpr + 3)], y1 = dram[Index(dpr + 6)], x1 = dram[Index(dpr + 9)];
	uint32			mar = 0, dy, dx;
	clk += 28;
	if (y0 >= y1)
	{
		dy = y0 - y1;
		clk += 2;
	}
	else
	{
		dy = y1 - y0;
		mar = 1;
		clk += 3;
	}
	g[14] = dy;
	g[15] = x1;
	clk += 10;
	if (x0 >= x1)
	{
		dx = x0 - x1;
		clk += 2;
	}
	else
	{
		dx = x1 - x0;
		mar += 2;
		clk += 4;
	}
	g[13] = dx;
	m.mar = mar;
	clk += 3;
	if (dx >= dy)
	{
		clk += 2;
		Slope(m, 0x38);
		g[2] = m.a;
		g[3] = 0x100;
		clk += 3;
	}
	else
	{
		g[14] = dx;
		g[13] = dy;
		clk += 2;
		Slope(m, 0x33);
		g[3] = m.a;
		g[2] = 0x100;
		clk += 6;
	}
	clk += 3;
	if (mar & 1)
		clk += 2;
	else
	{
		g[2] = (0 - g[2]) & 0xffffff;
		clk += 4;
	}
	clk += 3;
	if (mar & 2)
		clk += 2;
	else
	{
		g[3] = (0 - g[3]) & 0xffffff;
		clk += 4;
	}
	g[14] = (y0 << 8) & 0xffff;
	g[15] = (x0 << 8) & 0xffff;
	g[0] = m.a = 0xff;
	m.ram = (g[8] & 0xffff00) | x0;
	clk += 10;

	// The pixels: (R14, R15) step by (R2, R3), and R13 counts down past 0. A tile's address is
	// worked out when the pixel leaves the last one (R0); only the last pixel's registers outlive it.
	{
		uint32			y = g[14], x = g[15], n = g[13], key = g[0], tile = g[7], row, d, bits;
		const uint32	sy = g[2], sx = g[3], colour = g[1];
		uint64			mul = m.mul;
		uint8			*dram = m.r.dram;
		int64			clk = 0;
		for (;;)
		{
			row = (y >> 11) << 8;
			const uint32	k = row + (x >> 11);
			if (k == key)
				clk += 40;
			else
			{
				key = k;
				mul = Mul(x >> 11, 0xc0);
				tile = (((((y >> 8) & 0xf8) << 1) + ((uint32) mul & 0xffffff)) + 0x300) & 0xffffff;
				clk += 50;
			}
			d = (((x >> 7) & 0x0e) + tile) & 0xffffff;
			bits = colour >> ((y >> 8) & 7);
			dram[Index(d)] |= (uint8) bits;
			dram[Index(d + 1)] |= (uint8) (bits >> 8);
			y = (y + sy) & 0xffffff;
			x = (x + sx) & 0xffffff;
			if (!n)
				break;
			n--;
			clk += 2;
		}
		g[6] = row;
		g[0] = key;
		g[7] = tile;
		g[8] = bits;
		g[14] = y;
		g[15] = x;
		m.mul = mul;
		m.dpr = d;
		m.ram = (m.ram & 0xff0000) | m.D(d) | (m.D(d + 1) << 8);
		m.a = g[13] = Sub(f, 0, 1);
		m.clk += clk;
	}

	m.dpr = m.a = Add(f, 0x200, 0x80);
	m.p = 0x08;
	m.clk += 4;
	Restore(m, 0x8a);
	m.p = 0x0a;
	m.clk++;
	m.Ret();
}

// 08:00, the wireframe (Mega Man X2's intro): the model's edges at R0 turned by R2's three angles and
// drawn as lines; 08:01 clears the tiles first.
static void Wireframe (Run &k)
{
	Cpu		m(k);
	uint32	*g = m.g;
	Flags	&f = m.f;

	if (k.r.pc == 0x01)
	{
		m.dpr = m.a = Add(f, 0x300, 0);
		g[13] = m.a = Add(f, 0x200, 0x40);
		m.ram = m.a = 0;
		m.clk += 8;
		for (;;)
		{
			m.D(m.dpr) = m.D(m.dpr + 1) = m.D(m.dpr + 2) = m.D(m.dpr + 3) = 0;
			m.dpr = m.a = Add(f, m.dpr, 4);
			m.a = g[13] = Sub(f, g[13], 1);
			m.clk += 11;
			if (f.z)
			{
				m.clk++;
				break;
			}
			m.clk += 3;
		}
	}
	else
		m.clk += 3;

	m.p = 0x0a;
	m.dpr = m.a = Add(f, 0x200, 0x80);
	m.mar = m.a = g[0];
	m.p &= 0x00ff;
	m.a = g[2];
	m.clk += 8;
	SinCos(m, 0x1e);
	g[6] = m.a;
	g[7] = m.a = g[11];
	m.a = NZ(f, g[2] >> 8);
	m.clk += 5;
	SinCos(m, 0x24);
	g[8] = m.a;
	g[9] = m.a = g[11];
	m.a = NZ(f, g[2] >> 16);
	m.clk += 5;
	SinCos(m, 0x2a);

	for (;;)	// an edge at MAR
	{
		m.Read();
		m.a = Add(f, (m.a << 8) & 0xffffff, m.mdr);
		m.clk++;
		m.Read();
		m.a = Add(f, (m.a << 8) & 0xffffff, m.mdr);
		m.a = NZ(f, m.a << 8);
		Sub(f, m.a, 0xffff00);
		m.clk += 4;
		if (f.z)	// on from the last edge's end
		{
			m.Get(0, m.dpr + 6);
			m.D(m.dpr) = (uint8) m.ram;
			m.Get(0, m.dpr + 9);
			m.D(m.dpr + 3) = (uint8) m.ram;
			m.clk += 8;
		}
		else
		{
			m.a = NZ(f, m.a | 0x28);
			m.a = NZ(f, (m.a >> 8) | (m.a << 16));
			m.clk += 5;
			Vertex(m, 0x3e, false);
			Project(m, 0x3f);
			m.ram = m.a;
			m.D(m.dpr + 3) = (uint8) m.ram;
			m.ram = m.a = g[15];
			m.D(m.dpr) = (uint8) m.ram;
			m.clk += 5;
		}
		Vertex(m, 0x45, true);
		Project(m, 0x46);
		m.ram = m.a;
		m.D(m.dpr + 9) = (uint8) m.ram;
		m.ram = m.a = g[15];
		m.D(m.dpr + 6) = (uint8) m.ram;
		m.clk += 5;
		m.Read();
		g[1] = m.a = NZ(f, NZ(f, m.mdr & 1) << 7);
		g[1] = m.a = NZ(f, NZ(f, NZ(f, m.mdr & 2) << 14) | g[1]);
		m.clk += 9;
		Line(m, 0x58);
		m.Get(0, m.dpr + 0x15);
		m.ram = m.a = Sub(f, m.ram, 1);
		m.D(m.dpr + 0x15) = (uint8) m.ram;
		m.a = NZ(f, m.a & 0xff);
		m.clk += 7;
		if (f.z)
		{
			m.clk++;
			break;
		}
		m.clk += 3;
	}
	m.Halt(0x61);
}

// 03:00, a bitmap turned and scaled (Mega Man X2): 4bpp tiles from DPR sampling the nibbles at $600
// through the angle R0 about (R1, R2), scaled by R5/R6, R3 wide, rows from R7.
static void Rotate (Run &k)
{
	Cpu		m(k);
	uint32	*g = m.g;
	Flags	&f = m.f;

	g[8] = m.a = 0;
	m.mar = m.a = Add(f, NZ(f, NZ(f, g[7] >> 8) ^ 0xffffff), 1);
	g[7] = m.a = NZ(f, g[7] & 0xff);
	m.mul = Mul(NZ(f, m.a >> 3), g[3]);
	m.dpr = m.a = NZ(f, ((uint32) m.mul & 0xffffff) << 2);
	g[12] = m.a = g[0];
	m.ram = m.a = NZ(f, m.a & 0x7f);
	m.a = NZ(f, g[12] & 0x80);
	m.clk += 23;
	if (f.z)
	{
		m.a = NZ(f, m.ram & 0x7f);
		m.clk += 5;
		if (f.z)	// no turn
		{
			g[9] = m.a;
			g[10] = m.a = NZ(f, 1 << 12);
			m.clk += 8;
			goto turned;
		}
		m.a = NZ(f, Add(f, 0x200, 0) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[9] = m.a = NZ(f, m.rom >> 12);
		m.a = Add(f, 0x300, 0x80);
		m.clk += 12;
	}
	else
	{
		m.a = NZ(f, m.ram & 0x7f);
		m.clk += 3;
		if (f.z)	// a quarter turn
		{
			g[9] = m.a = NZ(f, 1 << 12);
			g[10] = m.a = 0;
			m.clk += 10;
			goto turned;
		}
		m.a = NZ(f, Add(f, 0x300, 0x80) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[9] = m.a = NZ(f, m.rom >> 12);
		m.a = Add(f, 0x200, 0);
		m.clk += 12;
	}
	m.a = NZ(f, m.a | m.ram);
	m.rom = m.k.drom[m.a & 0x3ff];
	g[10] = m.a = NZ(f, m.rom >> 12);
	m.clk += 5;

turned:
	m.a = NZ(f, g[12] << 15);
	m.clk += 3;
	if (f.n)
	{
		g[9] = m.a = m.Neg(g[9]);
		m.clk += 5;
	}
	else
		m.clk += 3;
	Sub(f, m.a = NZ(f, Add(f, NZ(f, g[12] >> 7), 1) & 3), 2);
	m.clk += 6;
	if (f.c)
	{
		g[10] = m.a = m.Neg(g[10]);
		m.clk += 5;
	}
	else
		m.clk += 3;

	// A row or a width of 0 runs the counters round 2^24 times: left to the interpreter.
	const uint32	w = g[3], h = g[4], cx = g[1], cy = g[2], sn = g[9], cs = g[10], kx = g[5], ky = g[6];
	if (w == 0 || w > 0x1000 || m.mar == 0 || (uint64) (0x1000000 - m.mar) * w > 0x100000)
	{
		k.ok = false;
		return;
	}

	// Only the last pixel's registers outlive the loops.
	uint32	x = g[8], y = g[7], mar = m.mar, ram = m.ram, dpr = 0, dx = 0, dy = 0, sx = 0, sy = 0, t14 = 0, nib = 0;
	uint64	mul = m.mul;
	uint8	*dram = m.r.dram;
	int64	clk = 0;
	for (;;)	// a row
	{
		dy = (y - cy) & 0xffffff;
		clk += 4;
		for (;;)	// a pixel
		{
			dx = (x - cx) & 0xffffff;
			const uint32	t = (uint32) Mul(dx, cs) & 0xffffff;
			mul = Mul((t - ((uint32) Mul(dy, sn) & 0xffffff)) & 0xffffff, kx);
			sx = ((uint32) (mul >> 24) + cx) & 0xffffff;
			const uint32	u = (uint32) Mul(dx, sn) & 0xffffff;
			mul = Mul((((uint32) Mul(dy, cs) & 0xffffff) + u) & 0xffffff, ky);
			sy = ((uint32) (mul >> 24) + cy) & 0xffffff;

			t14 = ((x & 0xf8) << 2) | ((y & 7) << 1);
			mul = Mul((y & 0xf8) >> 3, w);
			dpr = ((((uint32) mul << 2) & 0xffffff) + t14) & 0xffffff;
			int32	plot;
			if (sx >= w)
			{
				nib = 0;
				plot = 70;
			}
			else
			if (sy >= h)
			{
				nib = 0;
				plot = 72;
			}
			else
			{
				mul = Mul(sy, w);
				const uint32	i = (sx + ((uint32) mul & 0xffffff)) & 0xffffff;
				ram = (ram & 0xffff00) | dram[Index(((0xc00 + i) & 0xffffff) >> 1)];
				nib = ((i & 1) ? ram >> 4 : ram) & 0x0f;
				plot = 85;
			}
			// the nibble's bits into the tile's planes 3, 2, 1, 0
			static const uint8	at[4] = { 0x11, 0x10, 0x01, 0x00 };
			for (int b = 0; b < 4; b++)
			{
				uint8	&p = dram[Index(dpr + at[b])];
				ram = ((((ram & 0xffff00) | p) << 1) | ((nib >> (3 - b)) & 1)) & 0xffffff;
				p = (uint8) ram;
			}

			x = (x + 1) & 0xffffff;
			if (x != w)
			{
				clk += 41 + plot;
				continue;
			}
			clk += 39 + plot;
			break;
		}
		x = 0;
		y = (y + 1) & 0xffffff;
		mar = (mar + 1) & 0xffffff;
		clk += 9;
		if (!mar)
		{
			clk++;
			break;
		}
		clk += 3;
	}

	m.deepest = 1;	// the CALL each pixel

	g[7] = y;
	g[8] = x;
	g[11] = dx;
	g[12] = dy;
	g[13] = (nib << 23) & 0xffffff;
	g[14] = t14;
	g[15] = sy;
	m.mar = mar;
	m.ram = ram;
	m.dpr = dpr;
	m.mul = mul;
	m.a = Add(f, mar, 0);
	m.clk += clk;
	m.Halt(0x88);
}

static inline uint32 Asr (Flags &f, uint32 x, uint32 s)
{
	return (NZ(f, (uint32) ((int32) (x << 8) >> (8 + s))));
}

// 2:e3-e7: A = the data ROM's tan entry R0 (x $10000).
static void Tan (Cpu &m)
{
	m.a = Add(m.f, 0x300, m.g[0]);
	m.rom = m.k.drom[m.a & 0x3ff];
	m.a = m.rom;
	m.clk += 4;
}

// 2:d9, an edge's slope for an angle (A's low byte, R0 its index): tan, negated past $80.
static void Edge (Cpu &m, uint8 ret)
{
	Flags	&f = m.f;

	m.Call(m.p, ret);
	m.g[0] = m.a = NZ(f, m.a & 0xff);
	Sub(f, m.a, 0x80);
	m.clk += 4;
	if (!f.c)
	{
		m.g[0] = m.a;
		m.clk += 4;
		Tan(m);
		m.Ret();
		return;
	}
	m.g[0] = m.a = Sub(f, 0xff, m.a);
	m.clk += 4;
	m.Call(m.pb, 0xe2);
	Tan(m);
	m.Ret();
	m.a = Sub(f, 0, m.a);
	m.clk += 4;
	m.Ret();
}

// The searchlight's lines, page 4's routines in locals: each call is 3 clocks and its RET 3.
struct Lines
{
	uint8			*dram;
	const uint32	s8, s9, w;	// the edges' steps, the window's left (R14)
	uint32			l, rt, at10, at11;
	int64			clk;

	// 4:4d, a step held at 0 from below
	void Clamp (uint32 &x, uint32 step)
	{
		x = (x + step) & 0xffffff;
		if (x & 0x800000)
		{
			x = 0;
			clk += 11;
		}
		else
			clk += 8;
	}

	// 4:52, an 8.8 x to the line's byte: less R14, held to 0-$FF
	uint8 Column (uint32 x)
	{
		x = ((((uint32) ((int32) (x << 8) >> 16)) & 0xffffff) - w) & 0xffffff;
		if (x & 0x800000)
		{
			clk += 13;
			return (0);
		}
		if ((x - 0x100) & 0x800000)
		{
			clk += 14;
			return ((uint8) x);
		}
		clk += 16;
		return (0xff);
	}

	// 4:94, the line shut ($FF left, 0 right)
	void Shut (void)
	{
		dram[Index(at10)] = 0xff;
		dram[Index(at11)] = 0x00;
	}

	// 4:7f, n lines shut (none for 0, $E0 at most)
	void ShutLines (uint32 n, int &deepest)
	{
		clk += 5;
		if (!n)
		{
			clk += 5;
			return;
		}
		n = (n >= 0xdf) ? 0xdf : n;
		clk += 6;
		deepest = 3;
		for (;;)
		{
			Shut();
			at10 = (at10 + 1) & 0xffffff;
			at11 = (at11 + 1) & 0xffffff;
			clk += 22;
			n = (n - 1) & 0xffffff;
			if (n & 0x800000)
			{
				clk++;
				break;
			}
			clk += 3;
		}
		clk += 3;
	}

	// a line: both edges stepped and held, their bytes at R10/R11, shut (4:5c) where they miss the window
	void Line (void)
	{
		Clamp(l, s8);
		dram[Index(at10)] = Column(l);
		Clamp(rt, s9);
		dram[Index(at11)] = Column(rt);
		const uint32	x = (w << 8) & 0xffffff;
		bool	shut;
		if (x < l)
		{
			shut = l >= ((((w + 0x100) & 0xffffff) << 8) & 0xffffff);
			clk += 9;
		}
		else
		{
			shut = x >= rt;
			clk += 10;
		}
		if (shut)
		{
			Shut();
			clk += 11;
		}
		else
			clk += 3;
		at10 = (at10 + 1) & 0xffffff;
		at11 = (at11 + 1) & 0xffffff;
		clk += 20;
	}
};

// 02:22, a searchlight's window (Mega Man X3): two edges at the angles R4/R5 stepped down the screen,
// each line's left and right at $800/$900, shut where they miss the window.
static void Window (Run &k)
{
	Cpu		m(k);
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.p = 0x04;
	m.clk++;
	m.Call(m.p, 0x24);
	m.p = 0x02;
	g[14] = m.a = g[0];
	m.a = g[4];
	m.clk += 4;
	Edge(m, 0x05);
	g[8] = m.a = Asr(f, m.a, 8);
	m.a = g[5];
	m.clk += 3;
	Edge(m, 0x09);
	g[9] = Asr(f, m.a, 8);
	g[12] = (g[2] << 8) & 0xffffff;
	g[13] = (g[12] + g[6]) & 0xffffff;
	const uint32	top = (g[3] - g[1]) & 0xffffff;
	m.clk += 15;

	// Only R12/R13 and the window's bytes outlive the lines: the 33 shut below set the rest.
	Lines	s = { m.r.dram, g[8], g[9], g[14], g[12], g[13], 0x800, 0x900, 0 };
	uint32	n;
	if ((int32) g[3] < (int32) g[1])	// the top above the screen: step down to it
	{
		n = (0 - top) & 0xffffff;
		if (n > 0x10000)
		{
			k.ok = false;
			return;
		}
		s.clk += 2;
		for (;;)
		{
			s.Clamp(s.l, s.s8);
			s.Clamp(s.rt, s.s9);
			s.clk += 10;
			n = (n - 1) & 0xffffff;
			if (n & 0x800000)
			{
				s.clk++;
				break;
			}
			s.clk += 3;
		}
		n = 0xdf;
		s.clk += 4;
	}
	else
	{
		s.clk += 4;
		if (top > 0xdf)	// all of it below
		{
			s.ShutLines(top, m.deepest);
			s.clk += 6;
			goto bottom;
		}
		s.clk += 2;
		s.ShutLines(top, m.deepest);
		n = (0xe0 - top) & 0xffffff;
		s.clk += 3;
	}
	s.clk++;
	for (;;)
	{
		s.Line();
		n = (n - 1) & 0xffffff;
		if (n & 0x800000)
		{
			s.clk++;
			break;
		}
		s.clk += 3;
	}
	s.clk += 3;

bottom:
	// 4:66, 33 lines shut from $8DF/$9DF
	for (uint32 i = 0; i < 0x21; i++)
	{
		s.dram[Index(0x8df + i)] = 0xff;
		s.dram[Index(0x9df + i)] = 0x00;
	}
	s.clk += 9 + 0x21 * 14 + 0x20 * 3 + 1;
	g[10] = 0x900;
	g[11] = 0xa00;
	g[12] = s.l;
	g[13] = s.rt;
	g[15] = 0;
	m.ram = 0x00ffff;
	m.a = Sub(f, 1, 1);
	m.clk += s.clk;
	m.Ret();
	m.Halt(0x25);
}

// 6:30, as A:8c but into R9 and R10, A left as the quarter test leaves it.
static void SinCos6 (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.p, ret);
	m.a = NZ(f, NZ(f, m.a & 0xff) << 2);
	g[15] = m.a;
	m.ram = m.a = NZ(f, m.a & 0x7f);
	m.a = NZ(f, g[15] & 0x80);
	m.clk += 8;
	if (f.z)
	{
		m.a = m.ram;
		m.clk += 3;
	}
	else
	{
		m.ram = m.a = Sub(f, 0x80, m.ram);
		m.clk += 6;
	}
	Sub(f, m.a, 0x80);
	m.clk += 2;
	if (f.z)
	{
		g[9] = m.a = NZ(f, 0xffffff >> 8);
		g[10] = m.a = 0;
		m.clk += 7;
	}
	else
	{
		m.a = NZ(f, Add(f, 0x200, 0) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[9] = m.a = NZ(f, m.rom >> 8);
		m.a = NZ(f, Add(f, 0x300, 0x80) | m.ram);
		m.rom = m.k.drom[m.a & 0x3ff];
		g[10] = m.a = NZ(f, m.rom >> 8);
		m.clk += 17;
	}
	m.a = NZ(f, g[15] << 15);
	m.clk += 3;
	if (f.n)
	{
		g[9] = m.a = m.Neg(g[9]);
		m.clk += 5;
	}
	else
		m.clk += 3;
	Sub(f, m.a = NZ(f, Add(f, NZ(f, g[15] >> 7), 1) & 3), 2);
	m.clk += 6;
	if (f.c)
	{
		g[10] = m.a = m.Neg(g[10]);
		m.clk += 5;
	}
	else
		m.clk += 3;
	m.Ret();
}

// 6:00, R12 = $800000 / A roughly, from the data ROM's reciprocals and three Newton steps (the later
// ones double the step count where the estimate should be: the program's own way).
static void Reciprocal (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.p, ret);
	g[11] = m.a;
	m.a = NZ(f, m.a >> 8);
	m.clk += 3;
	if (f.z)	// under $100: the table alone
	{
		m.rom = m.k.drom[g[11] & 0x3ff];
		g[12] = m.a = m.rom;
		m.clk += 5;
		m.Ret();
		return;
	}
	m.clk += 3;
	Sub(f, m.a, 0x100);
	m.clk += 2;
	if (f.c)
	{
		m.a = NZ(f, m.a >> 8);
		m.rom = m.k.drom[m.a & 0x3ff];
		m.a = NZ(f, m.rom >> 16);
		m.clk += 1 + 7;
	}
	else
	{
		m.clk += 3;
		m.rom = m.k.drom[m.a & 0x3ff];
		m.a = NZ(f, m.rom >> 8);
		m.clk += 3;
	}
	g[12] = m.a;
	g[13] = m.a = 2;
	m.a = g[12];
	m.clk += 4;
	for (;;)
	{
		g[15] = m.a = NZ(f, m.a << 1);
		m.mul = Mul(g[12], g[11]);
		m.mul = Mul(NZ(f, ((uint32) m.mul & 0xffffff) >> 1), g[12]);
		const uint32	lo = (uint32) m.mul & 0xffffff;
		g[14] = m.a = NZ(f, NZ(f, (lo >> 22) | (lo << 2)) & 3);
		m.a = NZ(f, NZ(f, (((uint32) (m.mul >> 24) & 0xffffff) << 2)) | g[14]);
		g[12] = m.a = Sub(f, g[15], m.a);
		m.a = g[13] = Sub(f, g[13], 1);
		m.clk += 22;
		if (!f.c)
			break;
		m.clk += 2;
	}
	m.a = g[12];
	m.clk++;
	m.Ret();
}

// 6:da, the minor axis' step per pixel along the major R7: R6 * ($800000 / R7 >> 7), in 8.16.
static void Step16 (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.pb, ret);
	Sub(f, m.a = g[6], 0);
	m.clk += 3;
	if (f.z)
		m.clk += 2;
	else
	{
		m.a = Add(f, 0, g[7]);
		m.rom = m.k.drom[m.a & 0x3ff];
		m.mul = Mul(m.a = NZ(f, m.rom >> 7), g[6]);
		m.a = (uint32) m.mul & 0xffffff;
		m.clk += 8;
	}
	m.Ret();
}

// 6:67: the lines (a count at $B00, then two point numbers each) as descriptors from $600: the
// major axis' length + 1 and the 8.16 steps in y and x, from the points' screen bytes (+1, +5).
static void LineList (Cpu &m, uint8 ret)
{
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.Call(m.p, ret);
	g[1] = m.a = Add(f, 0x600, 0);
	m.dpr = m.a = Add(f, 0xb00, 0);
	m.Get(0, m.dpr);
	g[0] = m.a = NZ(f, m.ram & 0xff);
	m.dpr = m.a = Add(f, m.dpr, 2);
	m.clk += 13;
	for (;;)
	{
		for (int e = 0; e < 2; e++)	// the two ends' screen bytes
		{
			m.Get(0, m.dpr + e);
			m.a = Add(f, NZ(f, NZ(f, m.ram & 0xff) << 4), 1);
			m.Get(0, m.a);
			m.a = Add(f, m.a, 4);
			m.Get(1, m.a);
			g[2 + e * 2] = m.a = NZ(f, m.ram & 0xff);
			g[3 + e * 2] = m.a = NZ(f, NZ(f, m.ram >> 8) & 0xff);
		}
		m.mar = m.a = 0;
		m.a = Sub(f, g[2], g[4]);
		m.clk += 30 + 5;
		if (f.c)
			m.clk += 2;
		else
		{
			m.a = m.Neg(m.a);
			m.mar = (m.mar + 1) & 0xffffff;
			m.clk += 3;
		}
		g[6] = m.a;
		m.a = Sub(f, g[3], g[5]);
		m.clk += 4;
		if (f.c)
			m.clk += 2;
		else
		{
			m.a = m.Neg(m.a);
			m.mar = (m.mar + 2) & 0xffffff;
			m.clk += 4;
		}
		g[7] = m.a;
		Sub(f, m.a, g[6]);
		m.clk += 3;
		if (f.c)
		{
			m.clk += 2;
			Step16(m, 0xad);
			g[8] = m.a;
			g[9] = m.a = 0x010000;
			m.clk += 3;
		}
		else
		{
			const uint32	t = m.a;
			m.a = g[6];
			g[6] = t;
			g[7] = m.a;
			m.clk += 2;
			Step16(m, 0xa8);
			g[9] = m.a;
			g[8] = m.a = 0x010000;
			m.clk += 6;
		}
		m.a = NZ(f, m.mar << 23);
		m.clk += 3;
		if (f.n)
			m.clk += 2;
		else
		{
			g[8] = m.a = m.Neg(g[8]);
			m.clk += 4;
		}
		m.a = NZ(f, m.mar << 22);
		m.clk += 3;
		if (f.n)
			m.clk += 2;
		else
		{
			g[9] = m.a = m.Neg(g[9]);
			m.clk += 4;
		}
		{
			const uint32	t = m.dpr;
			m.a = g[1];
			g[1] = t;
		}
		m.dpr = m.a;
		m.ram = m.a = Add(f, g[7], 1);
		m.D(m.dpr) = (uint8) m.ram;
		m.ram = m.a = g[8];
		m.D(m.dpr + 1) = (uint8) m.ram;
		m.D(m.dpr + 2) = (uint8) (m.ram >> 8);
		m.D(m.dpr + 3) = (uint8) (m.ram >> 16);
		m.ram = m.a = g[9];
		m.D(m.dpr + 4) = (uint8) m.ram;
		m.D(m.dpr + 5) = (uint8) (m.ram >> 8);
		m.D(m.dpr + 6) = (uint8) (m.ram >> 16);
		m.a = Add(f, m.dpr, 8);
		{
			const uint32	t = m.a;
			m.a = g[1];
			g[1] = t;
		}
		m.dpr = m.a = Add(f, m.a, 2);
		m.a = g[0] = Sub(f, g[0], 1);
		m.clk += 26;
		if (f.z)
		{
			m.clk++;
			break;
		}
		m.clk += 3;
	}
	m.Ret();
}

// The 24-bit words of a point (dd: +0 y, e7: +4 x, f1: +8 z), read and written.
static uint32 Word (Cpu &m, uint32 at)
{
	m.Get(0, m.dpr + at);
	m.Get(1, m.dpr + at + 1);
	m.Get(2, m.dpr + at + 2);
	m.a = m.ram;
	m.clk += 4;
	m.Ret();
	return (m.a);
}

static void PutWord (Cpu &m, uint32 at)
{
	m.ram = m.a;
	m.D(m.dpr + at) = (uint8) m.ram;
	m.D(m.dpr + at + 1) = (uint8) (m.ram >> 8);
	m.D(m.dpr + at + 2) = (uint8) (m.ram >> 16);
	m.clk += 4;
	m.Ret();
}

// 05:00, transform lines (Mega Man X2): the R0 points at 0 turned by the angles R1-R3, projected
// through $800000 / z times R4, then the lines' descriptors (6:67).
static void TransformLines (Run &k)
{
	Cpu		m(k);
	uint32	*g = m.g;
	Flags	&f = m.f;

	m.a = g[1];
	m.p = 0x0006;
	m.clk += 3;
	SinCos6(m, 0x04);
	g[5] = m.a = g[9];
	g[6] = m.a = g[10];
	m.a = g[2];
	m.p = 0x0006;
	m.clk += 7;
	SinCos6(m, 0x0c);
	g[7] = m.a = g[9];
	g[8] = m.a = g[10];
	m.a = g[3];
	m.p = 0x0006;
	m.clk += 7;
	SinCos6(m, 0x14);
	m.dpr = m.a = 0;
	g[3] = m.a = Asr(f, g[3], 8);
	m.clk += 5;

	for (;;)	// a point at DPR
	{
		m.Get(1, m.dpr + 9);
		m.Get(2, m.dpr + 0x0a);
		m.ram = m.a = Sub(f, Asr(f, m.ram, 8), g[3]);
		m.D(m.dpr + 9) = (uint8) m.ram;
		m.D(m.dpr + 0x0a) = (uint8) (m.ram >> 8);
		m.clk += 8;
		m.Call(m.pb, 0x22); g[11] = Word(m, 4); m.clk++;
		m.Call(m.pb, 0x24); g[12] = Word(m, 8); m.clk++;
		g[13] = m.Fix(g[11], g[6], g[13]);
		m.a = Add(f, m.Fix(g[12], g[5], g[14]), g[13]);
		m.clk += 18;
		m.Call(m.pb, 0x38); PutWord(m, 4);
		g[13] = m.Fix(g[11], g[5], g[13]);
		m.a = Sub(f, m.Fix(g[12], g[6], g[14]), g[13]);
		m.clk += 18;
		m.Call(m.pb, 0x4b); PutWord(m, 8);
		m.Call(m.pb, 0x4c); g[11] = Word(m, 0); m.clk++;
		m.Call(m.pb, 0x4e); g[12] = Word(m, 8); m.clk++;
		g[13] = m.Fix(g[11], g[8], g[13]);
		m.a = Sub(f, g[13], m.Fix(g[12], g[7], g[14]));
		m.clk += 18;
		m.Call(m.pb, 0x62); PutWord(m, 0);
		g[13] = m.Fix(g[11], g[7], g[13]);
		m.a = Add(f, m.Fix(g[12], g[8], g[14]), g[13]);
		m.clk += 18;
		m.Call(m.pb, 0x75); PutWord(m, 8);
		m.Call(m.pb, 0x76); g[11] = Word(m, 0); m.clk++;
		m.Call(m.pb, 0x78); g[12] = Word(m, 4); m.clk++;
		g[13] = m.Fix(g[11], g[10], g[13]);
		m.a = Add(f, m.Fix(g[12], g[9], g[14]), g[13]);
		m.clk += 18;
		m.Call(m.pb, 0x8c); PutWord(m, 0);
		g[13] = m.Fix(g[11], g[9], g[13]);
		m.a = Sub(f, m.Fix(g[12], g[10], g[14]), g[13]);
		m.clk += 18;
		m.Call(m.pb, 0x9f); PutWord(m, 4);

		m.Get(1, m.dpr + 9);
		m.Get(2, m.dpr + 0x0a);
		m.ram = m.a = Add(f, Asr(f, m.ram, 8), g[3]);
		m.D(m.dpr + 9) = (uint8) m.ram;
		m.D(m.dpr + 0x0a) = (uint8) (m.ram >> 8);
		m.clk += 8;
		m.Call(m.pb, 0xa8); g[15] = Word(m, 8); m.clk++;
		m.a = Asr(f, m.a, 8);
		m.clk += 2;
		if (f.n)
		{
			m.a = m.Neg(m.a);
			m.clk += 3;
		}
		else
			m.clk += 3;
		m.p = 0x0006;
		m.clk += 2;
		Reciprocal(m, 0xb1);
		m.mul = Mul(NZ(f, m.a >> 7), g[4]);
		m.clk += 2;
		m.Call(m.pb, 0xb4); Word(m, 0);
		Add(f, m.a = g[15], 0);
		m.a = (uint32) m.mul & 0xffffff;
		m.clk += 4;
		if (f.n)
		{
			m.a = m.Neg(m.a);
			m.clk += 3;
		}
		else
			m.clk += 3;
		g[11] = m.a;
		m.mul = Mul(m.a, m.ram);
		m.ram = m.a = Add(f, (uint32) (m.mul >> 24) & 0xffffff, 0x80);
		m.D(m.dpr + 1) = (uint8) m.ram;
		m.D(m.dpr + 2) = (uint8) (m.ram >> 8);
		m.clk += 8;
		m.Call(m.pb, 0xc4); Word(m, 4);
		m.mul = Mul(m.a, g[11]);
		m.ram = m.a = Add(f, (uint32) (m.mul >> 24) & 0xffffff, 0x50);
		m.D(m.dpr + 5) = (uint8) m.ram;
		m.D(m.dpr + 6) = (uint8) (m.ram >> 8);
		m.clk += 7;
		m.Call(m.pb, 0xcc); Word(m, 8);
		m.ram = m.a = Sub(f, Asr(f, m.a, 8), g[4]);
		m.D(m.dpr + 9) = (uint8) m.ram;
		m.D(m.dpr + 0x0a) = (uint8) (m.ram >> 8);
		m.dpr = m.a = Add(f, m.dpr, 0x10);
		m.a = g[0] = Sub(f, g[0], 1);
		m.clk += 12;
		if (f.z)
		{
			m.clk++;
			break;
		}
		m.clk += 3;
	}

	m.p = 0x0006;
	m.clk += 2;
	LineList(m, 0xdc);
	m.Halt(0xdd);
}

// The program library the carts hold, page by page: X2 and X3 differ in pages 8 and $A only.
static const uint32	kPages[16] = {
	0x6c06a161, 0x64561bc0, 0x42100def, 0x8cb549df, 0xec39520e, 0x852b21d7, 0x84ef0c3e, 0xb96a079c,
	0x69a45896, 0x0df23e12, 0x54667b45, 0x8fdb61ca, 0x94e196fe, 0xd4660ae5, 0xef3846a4, 0xbd7bc39f
};

static uint32	lib_base = ~0u;	// the cache base the pages below were looked at for
static uint32	lib_pages;		// which of them hold the library

static uint32 Crc32 (const uint8 *p, uint32 n)
{
	uint32	c = ~0u;
	while (n--)
	{
		c ^= *p++;
		for (int b = 0; b < 8; b++)
			c = (c >> 1) ^ (0xedb88320 & (0 - (c & 1)));
	}
	return (~c);
}

static bool Known (Run &k, uint32 pages)
{
	if (k.r.cache_base != lib_base)
	{
		lib_base = k.r.cache_base;
		lib_pages = 0;
		for (uint32 p = 0; p < 16; p++)
		{
			uint8	page[512];
			for (uint32 i = 0; i < 512; i++)
				page[i] = Bus(k, (lib_base + p * 512 + i) & 0xffffff);
			if (Crc32(page, 512) == kPages[p])
				lib_pages |= 1 << p;
		}
		k.ok = true;
	}
	return ((lib_pages & pages) == pages);
}

void S9xHG51BNativeReset (void)
{
	lib_base = ~0u;
}

bool8 S9xHG51BNativeJob (S9xHG51BRegs &r, const uint32 *drom, S9xHG51BJob &job)
{
	Run	k = { r, job, drom, 0, true };
	void	(*program) (Run &);
	uint32	pages;

	switch ((r.pb << 8) | r.pc)
	{
		case 0x0000: program = Sprites; pages = 1 << 0; break;
		case 0x0800: case 0x0801: program = Wireframe; pages = (1 << 8) | (1 << 0xa); break;
		case 0x0300: program = Rotate; pages = 1 << 3; break;
		case 0x0222: program = Window; pages = (1 << 2) | (1 << 4); break;
		case 0x0500: program = TransformLines; pages = (1 << 5) | (1 << 6); break;
		default: return (FALSE);
	}

	if (!Known(k, pages) || !Enter(k, r.pb))
		return (FALSE);
	job.n = 0;
	program(k);
	job.clocks = k.clk;
	return (k.ok);
}
