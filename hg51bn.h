/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native Cx4 jobs: the programs the Mega Man X2 and X3 carts run on the HG51B169, each as a whole
// job that leaves the chip as its program does, after the same number of clocks, with no dump.

#ifndef _HG51BN_H_
#define _HG51BN_H_

#include "port.h"

// The chip's state, as hg51b.cpp runs it; saved as-is, so the padding is explicit.
struct S9xHG51BRegs
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
	uint8	fresh;				// a job just started, its clocks not yet run (native mode runs it ahead)
	uint64	line_base;			// master clock at the start of the current scanline
	uint64	synced;				// master clock the chip has been run up to
	uint64	rem;				// what's left over, in master clocks * CX4_HZ
	int64	budget;				// chip clocks owed; below 0 when it ran ahead
};

// What a job leaves besides the chip's state: the clocks it ran before its HALT, and the windows
// (from, to] of those clocks in which one of its bus reads was in flight, as the status shows.
struct S9xHG51BJob
{
	int64	clocks;
	uint64	(*busy)[2];
	uint32	n, max;
};

// Runs the job r has just started (out of halt at pb:pc) through its HALT, if it's a program the
// natives know; FALSE otherwise, r then left anyhow. drom is the data ROM.
bool8	S9xHG51BNativeJob (S9xHG51BRegs &r, const uint32 *drom, S9xHG51BJob &job);
// A new cart: which programs it holds is looked at again.
void	S9xHG51BNativeReset (void);

#endif
