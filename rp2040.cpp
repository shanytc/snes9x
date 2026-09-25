/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// RP2040 emulation for carts built around one (see rp2040.h). Timing is a
// cycle estimate per instruction; both cores run in short interleaved slices
// so the SIO FIFO and spinlock handshakes between them behave.

#include "rp2040.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

namespace RP2040
{

enum
{
	EXC_NMI			= 2,
	EXC_HARDFAULT	= 3,
	EXC_SVCALL		= 11,
	EXC_PENDSV		= 14,
	EXC_SYSTICK		= 15,
	EXC_IRQ0		= 16
};

enum
{
	IRQ_TIMER0		= 0,
	IRQ_PIO0_0		= 7,
	IRQ_PIO0_1		= 8,
	IRQ_PIO1_0		= 9,
	IRQ_PIO1_1		= 10,
	IRQ_DMA_0		= 11,
	IRQ_DMA_1		= 12,
	IRQ_IO_BANK0	= 13,
	IRQ_SIO_PROC0	= 15,
	IRQ_SIO_PROC1	= 16,
	IRQ_UART0		= 20,
	IRQ_UART1		= 21
};

#define XOSC_HZ			12000000u
#define ROSC_HZ			6500000u
#define SLICE			256
#define ROM_CORE1_DEAD	0x0000003c	// where a returning core 1 entry lands

// HLE ids: BKPT #id inside the ROM runs the native routine.
enum
{
	HLE_POPCOUNT = 1, HLE_REVERSE, HLE_CLZ, HLE_CTZ,
	HLE_MEMSET, HLE_MEMSET4, HLE_MEMCPY, HLE_MEMCPY44,
	HLE_USB_BOOT, HLE_DEBUG_TRAMPOLINE, HLE_DEBUG_TRAMPOLINE_END, HLE_WAIT_FOR_VECTOR,
	HLE_FLASH_CONNECT, HLE_FLASH_EXIT_XIP, HLE_FLASH_ERASE, HLE_FLASH_PROGRAM,
	HLE_FLASH_FLUSH, HLE_FLASH_ENTER_XIP,
	HLE_DEAD,
	HLE_SF = 64,		// 32 soft-float entries
	HLE_SD = 96			// 32 soft-double entries
};

#ifdef _MSC_VER
#include <intrin.h>
static inline int ctz32 (uint32_t v) { unsigned long i; _BitScanForward(&i, v); return (int) i; }
static inline int clz32 (uint32_t v) { unsigned long i; _BitScanReverse(&i, v); return 31 - (int) i; }
static inline uint32_t bswap32 (uint32_t v) { return _byteswap_ulong(v); }
#else
static inline int ctz32 (uint32_t v) { return __builtin_ctz(v); }
static inline int clz32 (uint32_t v) { return __builtin_clz(v); }
static inline uint32_t bswap32 (uint32_t v) { return __builtin_bswap32(v); }
#endif

static inline int popcount32 (uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555u);
	v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
	return (int) ((((v + (v >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24);
}

static inline uint32_t rd32 (const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16 (const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void wr32 (uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr16 (uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static inline uint32_t alias_apply (uint32_t old, uint32_t v, int alias)
{
	switch (alias)
	{
		case 1:  return old ^ v;
		case 2:  return old | v;
		case 3:  return old & ~v;
		default: return v;
	}
}

Chip::Chip ()
{
	log_fn = NULL;
	trace = false;
	profile[0] = profile[1] = NULL;
	trace_io = false;
	real_rom = false;
	flash_mask = 0;
	flash_dirty = false;
	memset(hle_calls, 0, sizeof(hle_calls));
	instr_count = 0;
	gpio_ext = 0x3fffffff;
	gpio_ext_drive = 0;
	PowerOn();
}

void Chip::Log (const char *fmt, ...)
{
	if (!log_fn)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_fn(buf);
}

bool Chip::LoadFlash (const uint8_t *data, size_t size)
{
	size_t cap = 1;
	while (cap < size)
		cap <<= 1;
	if (cap > 0x1000000 || size < 0x200)
		return false;
	flash.assign(cap, 0xff);
	memcpy(&flash[0], data, size);
	flash_mask = (uint32_t) cap - 1;
	flash_dirty = false;
	return true;
}

void Chip::LoadBootRom (const uint8_t *data, size_t size)
{
	memset(rom, 0, sizeof(rom));
	memcpy(rom, data, size < sizeof(rom) ? size : sizeof(rom));
	real_rom = true;
}

// A boot ROM with the real one's header and lookup tables, whose routines
// are BKPT stubs the core hands to Hle().
void Chip::BuildHleRom ()
{
	memset(rom, 0, sizeof(rom));
	wr32(rom + 0x00, 0x20042000);
	wr32(rom + 0x04, 0x00000201);
	wr32(rom + 0x08, 0x00000201);
	wr32(rom + 0x0c, 0x00000201);
	rom[0x10] = 'M';
	rom[0x11] = 'u';
	rom[0x12] = 0x01;
	rom[0x13] = 0x03;					// B2

	// rom_table_lookup(table, code)
	static const uint16_t lookup[] =
	{
		0x8802, 0x2a00, 0xd005, 0x8843, 0x3004, 0x4291, 0xd1f8, 0x0018,
		0x4770, 0x2000, 0x4770
	};
	for (unsigned i = 0; i < sizeof(lookup) / 2; i++)
		wr16(rom + 0x1c + i * 2, lookup[i]);
	wr16(rom + 0x18, 0x1d);

	// Stubs: BKPT #id ; BX LR, four bytes each from 0x400.
	uint32_t stub = 0x400;
	uint32_t stub_addr[256];
	for (int id = 0; id < 256; id++)
	{
		stub_addr[id] = stub;
		wr16(rom + stub, 0xbe00 | id);
		wr16(rom + stub + 2, 0x4770);
		stub += 4;
	}
	// The dead loop core 1 falls into if its entry returns.
	wr16(rom + ROM_CORE1_DEAD, 0xbe00 | HLE_DEAD);
	wr16(rom + ROM_CORE1_DEAD + 2, 0xe7fd);
	wr16(rom + 0x200, 0xbe00 | HLE_DEAD);
	wr16(rom + 0x200 + 2, 0xe7fd);

	struct { char a, b; int id; } funcs[] =
	{
		{ 'P','3', HLE_POPCOUNT }, { 'R','3', HLE_REVERSE }, { 'L','3', HLE_CLZ }, { 'T','3', HLE_CTZ },
		{ 'M','S', HLE_MEMSET }, { 'S','4', HLE_MEMSET4 }, { 'M','C', HLE_MEMCPY }, { 'C','4', HLE_MEMCPY44 },
		{ 'U','B', HLE_USB_BOOT }, { 'D','T', HLE_DEBUG_TRAMPOLINE }, { 'D','E', HLE_DEBUG_TRAMPOLINE_END },
		{ 'W','V', HLE_WAIT_FOR_VECTOR }, { 'I','F', HLE_FLASH_CONNECT }, { 'E','X', HLE_FLASH_EXIT_XIP },
		{ 'R','E', HLE_FLASH_ERASE }, { 'R','P', HLE_FLASH_PROGRAM }, { 'F','C', HLE_FLASH_FLUSH },
		{ 'C','X', HLE_FLASH_ENTER_XIP }
	};
	uint32_t t = 0x100;
	wr16(rom + 0x14, (uint16_t) t);
	for (unsigned i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++)
	{
		wr16(rom + t, (uint16_t) (funcs[i].a | (funcs[i].b << 8)));
		wr16(rom + t + 2, (uint16_t) (stub_addr[funcs[i].id] | 1));
		t += 4;
	}
	wr16(rom + t, 0);
	t += 4;

	// Soft float/double tables (32 entries each) at 0x800/0x880.
	for (int i = 0; i < 32; i++)
	{
		wr32(rom + 0x800 + i * 4, stub_addr[HLE_SF + i] | 1);
		wr32(rom + 0x880 + i * 4, stub_addr[HLE_SD + i] | 1);
	}
	static const char copyright[] = "(C) 2020 Raspberry Pi Trading Ltd";
	memcpy(rom + 0x900, copyright, sizeof(copyright));
	wr32(rom + 0x940, 0x7824f91d);

	uint32_t d = t;
	wr16(rom + 0x16, (uint16_t) d);
	struct { char a, b; uint32_t ptr; } data[] =
	{
		{ 'G','R', 0x940 }, { 'C','R', 0x900 }, { 'S','F', 0x800 }, { 'S','D', 0x880 },
		{ 'F','Z', 0x950 }, { 'F','S', 0x800 }, { 'F','E', 0x900 }
	};
	rom[0x950] = 32;
	for (unsigned i = 0; i < sizeof(data) / sizeof(data[0]); i++)
	{
		wr16(rom + d, (uint16_t) (data[i].a | (data[i].b << 8)));
		wr16(rom + d + 2, (uint16_t) data[i].ptr);
		d += 4;
	}
	wr16(rom + d, 0);
}

void Chip::ResetCore (int n)
{
	Core &c = core[n];
	uint64_t keep = c.cycles;
	memset(&c, 0, sizeof(c));
	c.cycles = keep;
	c.systick_last = keep;
	c.vtor = 0;
	c.msp = rd32(rom);
	c.r[13] = c.msp;
	c.r[14] = 0xffffffff;
	c.shpr2 = c.shpr3 = 0;
	c.launch_step = 0;
	c.halted = (n == 1);
}

void Chip::PowerOn ()
{
	if (!real_rom)
		BuildHleRom();

	memset(sram, 0, sizeof(sram));
	memset(xip_sram, 0, sizeof(xip_sram));
	memset(usb_ram, 0, sizeof(usb_ram));

	now = 0;
	sys_hz = ROSC_HZ;
	us_count = us_rem = 0;
	us_last_cycles = 0;

	memset(core, 0, sizeof(core));
	ResetCore(0);
	ResetCore(1);

	resets = 0x01ffffff;
	psm_frce_on = psm_frce_off = 0;
	memset(clk_ctrl, 0, sizeof(clk_ctrl));
	memset(clk_div, 0, sizeof(clk_div));
	for (int i = 0; i < 10; i++)
		clk_div[i] = 0x100;
	memset(pll_sys, 0, sizeof(pll_sys));
	memset(pll_usb, 0, sizeof(pll_usb));
	pll_sys[0] = pll_usb[0] = 1;
	pll_sys[1] = pll_usb[1] = 0x2d;
	pll_sys[3] = pll_usb[3] = 0x77000;
	xosc_ctrl = 0;
	xosc_startup = 0xc4;
	rosc_ctrl = 0xaa0;
	rosc_lfsr = 0xace1u;
	watchdog_ctrl = 0x07000000;
	watchdog_load = watchdog_reason = watchdog_tick = 0;
	memset(watchdog_scratch, 0, sizeof(watchdog_scratch));
	vreg = 0xb1;
	bod = 0x91;
	chip_reset = 0;
	busctrl_priority = 0;
	memset(syscfg, 0, sizeof(syscfg));
	for (int i = 0; i < NUM_GPIO; i++)
		gpio_ctrl[i] = 0x1f;
	RecalcGpioMux();
	for (int i = 0; i < NUM_GPIO + 2; i++)
		pads[i] = 0x56;
	memset(gpio_intr, 0, sizeof(gpio_intr));
	memset(gpio_inte, 0, sizeof(gpio_inte));
	memset(gpio_intf, 0, sizeof(gpio_intf));
	memset(qspi_ctrl, 0, sizeof(qspi_ctrl));
	memset(qspi_pads, 0, sizeof(qspi_pads));
	gpio_prev = gpio_ext;
	irq_check[0] = irq_check[1] = true;
	irq_dirty = true;
	reset_request = false;
	sio_out = sio_oe = 0;
	sio_hi_out = sio_hi_oe = 0;
	memset(fifo, 0, sizeof(fifo));
	memset(fifo_head, 0, sizeof(fifo_head));
	memset(fifo_level, 0, sizeof(fifo_level));
	memset(fifo_sticky, 0, sizeof(fifo_sticky));
	spinlocks = 0;
	memset(timer_alarm, 0, sizeof(timer_alarm));
	timer_armed = timer_intr = timer_inte = timer_intf = timer_pause = 0;
	timer_latched_hi = 0;
	for (int i = 0; i < 2; i++)
	{
		uart_cr[i] = 0x300;
		uart_ibrd[i] = uart_fbrd[i] = uart_lcr[i] = uart_imsc[i] = 0;
		uart_line[i].clear();
	}
	adc_cs = adc_result = adc_div = adc_fcs = 0;
	memset(pwm, 0, sizeof(pwm));
	memset(ssi_regs, 0, sizeof(ssi_regs));
	ssi_regs[0x10 >> 2] = 1;			// the boot ROM leaves slave 0 selected
	xip_ctrl = 3;
	xip_stream_addr = xip_stream_ctr = 0;
	spi_cmd = 0;
	spi_count = spi_addr = 0;
	spi_selected = spi_wel = false;
	ssi_idle_seen = false;
	ssi_rx.clear();

	memset(dma, 0, sizeof(dma));
	for (int i = 0; i < NUM_DMA; i++)
		dma[i].ctrl = (uint32_t) i << 11;
	dma_intr = 0;
	memset(dma_inte, 0, sizeof(dma_inte));
	memset(dma_intf, 0, sizeof(dma_intf));
	memset(dma_timer, 0, sizeof(dma_timer));
	dma_sniff_ctrl = dma_sniff_data = 0;
	dma_in_service = false;

	memset(pio, 0, sizeof(pio));
	for (int p = 0; p < 2; p++)
		for (int s = 0; s < 4; s++)
		{
			PioSm &sm = pio[p].sm[s];
			sm.clkdiv = 0x10000;
			sm.execctrl = 0x1f << 12;
			sm.shiftctrl = 0x000c0000;
			sm.pinctrl = 0x14000000;
			sm.osr_count = 32;
		}

	// Core 0 starts where boot stage 2 hands over: the flash vector table.
	if (!flash.empty())
	{
		Core &c = core[0];
		c.vtor = 0x10000100;
		c.msp = rd32(&flash[0x100]);
		c.r[13] = c.msp;
		c.r[15] = rd32(&flash[0x104]) & ~1u;
		c.r[14] = 0xffffffff;
	}
	UpdateClocks();
}

uint32_t Chip::Xpsr (int n)
{
	Core &c = core[n];
	return (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (1u << 24) | c.ipsr;
}

void Chip::SetSpSel (int n, bool psp)
{
	Core &c = core[n];
	bool cur = (c.control & 2) && !c.ipsr;
	if (cur)
		c.psp = c.r[13];
	else
		c.msp = c.r[13];
	if (psp)
		c.control |= 2;
	else
		c.control &= ~2u;
	bool nxt = (c.control & 2) && !c.ipsr;
	c.r[13] = nxt ? c.psp : c.msp;
}

void Chip::HardFault (int n, const char *why, uint32_t addr)
{
	Core &c = core[n];
	Log("rp2040: core%d HardFault (%s) at pc=%08x addr=%08x", n, why, c.r[15], addr);
	irq_check[n] = true;
	if (c.ipsr == EXC_HARDFAULT || c.ipsr == EXC_NMI)
	{
		c.lockup = true;
		return;
	}
	TakeException(n, EXC_HARDFAULT);
}

// ---------------------------------------------------------------------------
// Exceptions

int Chip::ExceptionPriority (int n, int exc)
{
	Core &c = core[n];
	if (exc == EXC_NMI)
		return -2;
	if (exc == EXC_HARDFAULT)
		return -1;
	if (exc == EXC_SVCALL)
		return (c.shpr2 >> 30) & 3;
	if (exc == EXC_PENDSV)
		return (c.shpr3 >> 22) & 3;
	if (exc == EXC_SYSTICK)
		return (c.shpr3 >> 30) & 3;
	if (exc >= EXC_IRQ0)
		return c.nvic_prio[exc - EXC_IRQ0] >> 6;
	return 4;
}

int Chip::ExecutionPriority (int n)
{
	Core &c = core[n];
	int p = 4;
	for (uint32_t m = c.exc_active; m; m &= m - 1)
	{
		int e = ctz32(m);
		int q = ExceptionPriority(n, e);
		if (q < p)
			p = q;
	}
	for (uint32_t m = c.nvic_active; m; m &= m - 1)
	{
		int q = ExceptionPriority(n, EXC_IRQ0 + ctz32(m));
		if (q < p)
			p = q;
	}
	if (c.primask && p > 0)
		p = 0;
	return p;
}

// The highest-priority pending exception that would preempt now, or 0.
int Chip::PendingException (int n)
{
	Core &c = core[n];
	int best = 0, best_p = 5;
	if (c.pendsv)
	{
		best = EXC_PENDSV;
		best_p = ExceptionPriority(n, EXC_PENDSV);
	}
	if (c.systick_pend)
	{
		int p = ExceptionPriority(n, EXC_SYSTICK);
		if (p < best_p)
		{
			best = EXC_SYSTICK;
			best_p = p;
		}
	}
	for (uint32_t m = c.nvic_pending & c.nvic_enable & ~c.nvic_active; m; m &= m - 1)
	{
		int irq = ctz32(m);
		int p = c.nvic_prio[irq] >> 6;
		if (p < best_p)
		{
			best = EXC_IRQ0 + irq;
			best_p = p;
		}
	}
	if (!best)
		return 0;
	if (best_p >= ExecutionPriority(n))
		return 0;
	return best;
}

void Chip::TakeException (int n, uint32_t exc)
{
	Core &c = core[n];
	c.spin_pc = 0;
	bool psp = !c.ipsr && (c.control & 2);
	uint32_t sp = c.r[13];
	uint32_t align = (sp & 4) ? 1 : 0;
	sp = (sp - 0x20) & ~7u;
	Write32(n, sp + 0x00, c.r[0]);
	Write32(n, sp + 0x04, c.r[1]);
	Write32(n, sp + 0x08, c.r[2]);
	Write32(n, sp + 0x0c, c.r[3]);
	Write32(n, sp + 0x10, c.r[12]);
	Write32(n, sp + 0x14, c.r[14]);
	Write32(n, sp + 0x18, c.r[15]);
	Write32(n, sp + 0x1c, (Xpsr(n) & ~(1u << 9)) | (align << 9));
	if (psp)
	{
		c.psp = sp;
		c.r[13] = c.msp;
	}
	else
	{
		c.msp = sp;
		c.r[13] = sp;
	}
	c.r[14] = c.ipsr ? 0xfffffff1 : (psp ? 0xfffffffd : 0xfffffff9);
	c.ipsr = exc;
	if (exc >= EXC_IRQ0)
	{
		uint32_t bit = 1u << (exc - EXC_IRQ0);
		c.nvic_active |= bit;
		c.nvic_pending &= ~bit;
	}
	else
	{
		c.exc_active |= 1u << exc;
		if (exc == EXC_PENDSV)
			c.pendsv = false;
		if (exc == EXC_SYSTICK)
			c.systick_pend = false;
	}
	c.r[15] = Read32(n, c.vtor + exc * 4) & ~1u;
	c.cycles += 15;
	c.event = true;
	c.sleeping = false;
}

void Chip::ExceptionReturn (int n, uint32_t exc_return)
{
	Core &c = core[n];
	c.spin_pc = 0;
	uint32_t exc = c.ipsr;
	if (exc >= EXC_IRQ0)
		c.nvic_active &= ~(1u << (exc - EXC_IRQ0));
	else
		c.exc_active &= ~(1u << exc);

	c.msp = c.r[13];
	uint32_t kind = exc_return & 0xf;
	bool psp = (kind == 0xd);
	uint32_t sp = psp ? c.psp : c.msp;
	c.r[0]  = Read32(n, sp + 0x00);
	c.r[1]  = Read32(n, sp + 0x04);
	c.r[2]  = Read32(n, sp + 0x08);
	c.r[3]  = Read32(n, sp + 0x0c);
	c.r[12] = Read32(n, sp + 0x10);
	c.r[14] = Read32(n, sp + 0x14);
	uint32_t pc = Read32(n, sp + 0x18);
	uint32_t xpsr = Read32(n, sp + 0x1c);
	sp += 0x20;
	if (xpsr & (1u << 9))
		sp += 4;
	if (psp)
	{
		c.psp = sp;
		c.control |= 2;
	}
	else
	{
		c.msp = sp;
		if (kind != 1)
			c.control &= ~2u;
	}
	// back to handler mode (the stacked exception number) or to thread mode
	c.ipsr = (kind == 1) ? ((xpsr & 0x3f) ? (xpsr & 0x3f) : exc) : 0;
	c.r[13] = (c.ipsr || !(c.control & 2)) ? c.msp : c.psp;
	c.n = xpsr >> 31;
	c.z = (xpsr >> 30) & 1;
	c.c = (xpsr >> 29) & 1;
	c.v = (xpsr >> 28) & 1;
	c.r[15] = pc & ~1u;
	c.cycles += 10;
	c.event = true;
	irq_check[n] = true;	// anything pending behind it goes next
}

void Chip::BranchWritePc (int n, uint32_t addr)
{
	core[n].r[15] = addr & ~1u;
}

void Chip::BxWritePc (int n, uint32_t addr)
{
	Core &c = core[n];
	if (c.ipsr && (addr & 0xf0000000) == 0xf0000000)
	{
		ExceptionReturn(n, addr);
		return;
	}
	if (!(addr & 1))
	{
		c.r[15] = addr;
		HardFault(n, "interworking to ARM state", addr);
		return;
	}
	c.r[15] = addr & ~1u;
}

// ---------------------------------------------------------------------------
// Instruction interpreter (ARMv6-M Thumb)

static inline uint32_t add_flags (Core &c, uint32_t a, uint32_t b, uint32_t carry)
{
	uint64_t r = (uint64_t) a + b + carry;
	uint32_t res = (uint32_t) r;
	c.n = res >> 31;
	c.z = (res == 0);
	c.c = (uint32_t) (r >> 32);
	c.v = ((a ^ res) & (b ^ res)) >> 31;
	return res;
}

static inline void nz (Core &c, uint32_t res)
{
	c.n = res >> 31;
	c.z = (res == 0);
}

static inline bool cond_pass (const Core &c, uint32_t cond)
{
	switch (cond)
	{
		case 0x0: return c.z;
		case 0x1: return !c.z;
		case 0x2: return c.c;
		case 0x3: return !c.c;
		case 0x4: return c.n;
		case 0x5: return !c.n;
		case 0x6: return c.v;
		case 0x7: return !c.v;
		case 0x8: return c.c && !c.z;
		case 0x9: return !c.c || c.z;
		case 0xa: return c.n == c.v;
		case 0xb: return c.n != c.v;
		case 0xc: return !c.z && c.n == c.v;
		case 0xd: return c.z || c.n != c.v;
		default:  return true;
	}
}

// A short backward loop whose iteration left every register as it found it
// and repeated the previous iteration's stores exactly is waiting on another
// agent (the other core, DMA, a timer, an interrupt), all of which only act
// between slices: the rest of the slice can be skipped.
static inline bool spin_detect (Core &c, uint32_t target)
{
	uint32_t flags = (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v;
	bool spin = c.spin_pc == target && c.spin_hash == c.store_hash && c.spin_flags == flags &&
				!memcmp(c.spin_regs, c.r, sizeof(c.spin_regs));
	c.spin_pc = target;
	c.spin_hash = c.store_hash;
	c.spin_flags = flags;
	memcpy(c.spin_regs, c.r, sizeof(c.spin_regs));
	c.store_hash = 0;
	return spin;
}

void Chip::Execute (int n, uint64_t until)
{
	Core &c = core[n];
	uint32_t *r = c.r;

	if (c.halted || c.sleeping || c.lockup)
		return;

	// Kept in a local: stores into SRAM may alias c.cycles for the compiler.
	uint64_t cyc = c.cycles;
#define SYNC_OUT	c.cycles = cyc
#define SYNC_IN		cyc = c.cycles

	while (cyc < until)
	{
		if (irq_check[n])
		{
			// also raised by anything that halts, faults or parks this core
			if (c.halted || c.sleeping || c.lockup)
			{
				SYNC_OUT;
				return;
			}
			irq_check[n] = false;
			int exc = PendingException(n);
			if (exc)
			{
				SYNC_OUT;
				TakeException(n, exc);
				SYNC_IN;
				continue;
			}
		}

		uint32_t pc = r[15];
		uint32_t op;
		if ((pc - 0x10000000u) < 0x04000000u)
			op = rd16(&flash[pc & flash_mask]);
		else if ((pc - 0x20000000u) < SRAM_SIZE - 1)
			op = rd16(sram + (pc - 0x20000000u));
		else if (pc < ROM_SIZE)
			op = rd16(rom + pc);
		else
			op = Read16(n, pc);

		if (trace)
		{
			instr_count++;
			if (profile[n])
				profile[n][((pc & 0x7ffff) >> 1) | (((pc >> 29) & 1) << 18)]++;
			else
				Log("c%d %08x: %04x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x", n, pc, op, r[0], r[1], r[2], r[3], r[13], r[14]);
		}

		r[15] = pc + 2;
		cyc++;

		switch (op >> 11)
		{
			case 0x00:	// LSLS Rd, Rm, #imm
			{
				uint32_t imm = (op >> 6) & 31, rm = r[(op >> 3) & 7];
				if (imm)
				{
					c.c = (rm >> (32 - imm)) & 1;
					rm <<= imm;
				}
				r[op & 7] = rm;
				nz(c, rm);
				break;
			}

			case 0x01:	// LSRS Rd, Rm, #imm
			{
				uint32_t imm = (op >> 6) & 31, rm = r[(op >> 3) & 7];
				if (!imm)
				{
					c.c = rm >> 31;
					rm = 0;
				}
				else
				{
					c.c = (rm >> (imm - 1)) & 1;
					rm >>= imm;
				}
				r[op & 7] = rm;
				nz(c, rm);
				break;
			}

			case 0x02:	// ASRS Rd, Rm, #imm
			{
				uint32_t imm = (op >> 6) & 31, rm = r[(op >> 3) & 7];
				if (!imm)
				{
					c.c = rm >> 31;
					rm = (uint32_t) ((int32_t) rm >> 31);
				}
				else
				{
					c.c = (rm >> (imm - 1)) & 1;
					rm = (uint32_t) ((int32_t) rm >> imm);
				}
				r[op & 7] = rm;
				nz(c, rm);
				break;
			}

			case 0x03:	// ADDS/SUBS register or imm3
			{
				uint32_t rn = r[(op >> 3) & 7];
				uint32_t val = (op & 0x400) ? ((op >> 6) & 7) : r[(op >> 6) & 7];
				if (op & 0x200)
					r[op & 7] = add_flags(c, rn, ~val, 1);
				else
					r[op & 7] = add_flags(c, rn, val, 0);
				break;
			}

			case 0x04:	// MOVS Rd, #imm8
			{
				uint32_t v = op & 0xff;
				r[(op >> 8) & 7] = v;
				nz(c, v);
				break;
			}

			case 0x05:	// CMP Rn, #imm8
				add_flags(c, r[(op >> 8) & 7], ~(op & 0xff), 1);
				break;

			case 0x06:	// ADDS Rdn, #imm8
			{
				uint32_t d = (op >> 8) & 7;
				r[d] = add_flags(c, r[d], op & 0xff, 0);
				break;
			}

			case 0x07:	// SUBS Rdn, #imm8
			{
				uint32_t d = (op >> 8) & 7;
				r[d] = add_flags(c, r[d], ~(op & 0xff), 1);
				break;
			}

			case 0x08:
				if (!(op & 0x400))
				{
					// Data processing
					uint32_t d = op & 7, m = r[(op >> 3) & 7], v = r[d];
					switch ((op >> 6) & 15)
					{
						case 0x0: v &= m; nz(c, v); r[d] = v; break;
						case 0x1: v ^= m; nz(c, v); r[d] = v; break;
						case 0x2:	// LSLS reg
						{
							uint32_t s = m & 0xff;
							if (s)
							{
								if (s < 32) { c.c = (v >> (32 - s)) & 1; v <<= s; }
								else if (s == 32) { c.c = v & 1; v = 0; }
								else { c.c = 0; v = 0; }
							}
							nz(c, v);
							r[d] = v;
							break;
						}
						case 0x3:	// LSRS reg
						{
							uint32_t s = m & 0xff;
							if (s)
							{
								if (s < 32) { c.c = (v >> (s - 1)) & 1; v >>= s; }
								else if (s == 32) { c.c = v >> 31; v = 0; }
								else { c.c = 0; v = 0; }
							}
							nz(c, v);
							r[d] = v;
							break;
						}
						case 0x4:	// ASRS reg
						{
							uint32_t s = m & 0xff;
							if (s)
							{
								if (s < 32) { c.c = (v >> (s - 1)) & 1; v = (uint32_t) ((int32_t) v >> s); }
								else { c.c = v >> 31; v = (uint32_t) ((int32_t) v >> 31); }
							}
							nz(c, v);
							r[d] = v;
							break;
						}
						case 0x5: r[d] = add_flags(c, v, m, c.c); break;
						case 0x6: r[d] = add_flags(c, v, ~m, c.c); break;
						case 0x7:	// RORS
						{
							uint32_t s = m & 0xff;
							if (s)
							{
								s &= 31;
								if (s)
									v = (v >> s) | (v << (32 - s));
								c.c = v >> 31;
							}
							nz(c, v);
							r[d] = v;
							break;
						}
						case 0x8: nz(c, v & m); break;
						case 0x9: r[d] = add_flags(c, 0, ~m, 1); break;
						case 0xa: add_flags(c, v, ~m, 1); break;
						case 0xb: add_flags(c, v, m, 0); break;
						case 0xc: v |= m; nz(c, v); r[d] = v; break;
						case 0xd: v *= m; nz(c, v); r[d] = v; break;
						case 0xe: v &= ~m; nz(c, v); r[d] = v; break;
						case 0xf: v = ~m; nz(c, v); r[d] = v; break;
					}
				}
				else
				{
					// Hi register operations / BX / BLX
					uint32_t d = (op & 7) | ((op >> 4) & 8), m = (op >> 3) & 15;
					uint32_t mv = (m == 15) ? pc + 4 : r[m];
					switch ((op >> 8) & 3)
					{
						case 0:	// ADD
						{
							uint32_t dv = (d == 15) ? pc + 4 : r[d];
							uint32_t res = dv + mv;
							if (d == 15)
							{
								BranchWritePc(n, res);
								cyc++;
							}
							else if (d == 13)
								r[13] = res & ~3u;
							else
								r[d] = res;
							break;
						}
						case 1:	// CMP
						{
							uint32_t dv = (d == 15) ? pc + 4 : r[d];
							add_flags(c, dv, ~mv, 1);
							break;
						}
						case 2:	// MOV
							if (d == 15)
							{
								BranchWritePc(n, mv);
								cyc++;
							}
							else if (d == 13)
								r[13] = mv & ~3u;
							else
								r[d] = mv;
							break;
						case 3:	// BX / BLX
							if (op & 0x80)
								r[14] = (pc + 2) | 1;
							cyc++;
							SYNC_OUT;
							BxWritePc(n, mv);
							SYNC_IN;
							break;
					}
				}
				break;

			case 0x09:	// LDR Rt, [PC, #imm8]
			{
				uint32_t addr = ((pc + 4) & ~3u) + ((op & 0xff) << 2);
				r[(op >> 8) & 7] = Read32(n, addr);
				cyc++;
				break;
			}

			case 0x0a:
			case 0x0b:	// load/store register offset
			{
				uint32_t addr = r[(op >> 3) & 7] + r[(op >> 6) & 7];
				uint32_t t = op & 7;
				cyc++;
				switch ((op >> 9) & 7)
				{
					case 0: Write32(n, addr, r[t]); break;
					case 1: Write16(n, addr, (uint16_t) r[t]); break;
					case 2: Write8(n, addr, (uint8_t) r[t]); break;
					case 3: r[t] = (uint32_t) (int32_t) (int8_t) Read8(n, addr); break;
					case 4: r[t] = Read32(n, addr); break;
					case 5: r[t] = Read16(n, addr); break;
					case 6: r[t] = Read8(n, addr); break;
					case 7: r[t] = (uint32_t) (int32_t) (int16_t) Read16(n, addr); break;
				}
				break;
			}

			case 0x0c:	// STR Rt, [Rn, #imm5*4]
				Write32(n, r[(op >> 3) & 7] + (((op >> 6) & 31) << 2), r[op & 7]);
				cyc++;
				break;

			case 0x0d:	// LDR Rt, [Rn, #imm5*4]
				r[op & 7] = Read32(n, r[(op >> 3) & 7] + (((op >> 6) & 31) << 2));
				cyc++;
				break;

			case 0x0e:	// STRB
				Write8(n, r[(op >> 3) & 7] + ((op >> 6) & 31), (uint8_t) r[op & 7]);
				cyc++;
				break;

			case 0x0f:	// LDRB
				r[op & 7] = Read8(n, r[(op >> 3) & 7] + ((op >> 6) & 31));
				cyc++;
				break;

			case 0x10:	// STRH
				Write16(n, r[(op >> 3) & 7] + (((op >> 6) & 31) << 1), (uint16_t) r[op & 7]);
				cyc++;
				break;

			case 0x11:	// LDRH
				r[op & 7] = Read16(n, r[(op >> 3) & 7] + (((op >> 6) & 31) << 1));
				cyc++;
				break;

			case 0x12:	// STR Rt, [SP, #imm8*4]
				Write32(n, r[13] + ((op & 0xff) << 2), r[(op >> 8) & 7]);
				cyc++;
				break;

			case 0x13:	// LDR Rt, [SP, #imm8*4]
				r[(op >> 8) & 7] = Read32(n, r[13] + ((op & 0xff) << 2));
				cyc++;
				break;

			case 0x14:	// ADR
				r[(op >> 8) & 7] = ((pc + 4) & ~3u) + ((op & 0xff) << 2);
				break;

			case 0x15:	// ADD Rd, SP, #imm8*4
				r[(op >> 8) & 7] = r[13] + ((op & 0xff) << 2);
				break;

			case 0x16:
			case 0x17:	// miscellaneous
				switch ((op >> 8) & 15)
				{
					case 0x0:	// ADD/SUB SP, #imm7*4
						if (op & 0x80)
							r[13] -= (op & 0x7f) << 2;
						else
							r[13] += (op & 0x7f) << 2;
						break;

					case 0x2:	// SXTH/SXTB/UXTH/UXTB
					{
						uint32_t m = r[(op >> 3) & 7], v;
						switch ((op >> 6) & 3)
						{
							case 0:  v = (uint32_t) (int32_t) (int16_t) m; break;
							case 1:  v = (uint32_t) (int32_t) (int8_t) m; break;
							case 2:  v = m & 0xffff; break;
							default: v = m & 0xff; break;
						}
						r[op & 7] = v;
						break;
					}

					case 0x4:
					case 0x5:	// PUSH
					{
						uint32_t list = (op & 0xff) | ((op & 0x100) ? 0x4000 : 0);
						uint32_t cnt = popcount32(list);
						uint32_t addr = r[13] - 4 * cnt;
						r[13] = addr;
						for (int i = 0; i < 15; i++)
							if (list & (1u << i))
							{
								Write32(n, addr, r[i]);
								addr += 4;
							}
						cyc += cnt;
						break;
					}

					case 0x6:	// CPS
						if ((op & 0xffef) == 0xb662)
						{
							c.primask = (op >> 4) & 1;
							irq_check[n] = true;
						}
						else
						{
							SYNC_OUT;
							HardFault(n, "undefined CPS", op);
							SYNC_IN;
						}
						break;

					case 0xa:	// REV/REV16/REVSH
					{
						uint32_t m = r[(op >> 3) & 7], v;
						switch ((op >> 6) & 3)
						{
							case 0:  v = bswap32(m); break;
							case 1:  v = ((m & 0x00ff00ff) << 8) | ((m >> 8) & 0x00ff00ff); break;
							case 3:  v = (uint32_t) (int32_t) (int16_t) (((m & 0xff) << 8) | ((m >> 8) & 0xff)); break;
							default: v = m; SYNC_OUT; HardFault(n, "undefined REV", op); SYNC_IN; break;
						}
						r[op & 7] = v;
						break;
					}

					case 0xc:
					case 0xd:	// POP
					{
						uint32_t list = op & 0xff;
						uint32_t addr = r[13];
						uint32_t cnt = popcount32(list) + ((op & 0x100) ? 1 : 0);
						for (int i = 0; i < 8; i++)
							if (list & (1u << i))
							{
								r[i] = Read32(n, addr);
								addr += 4;
							}
						cyc += cnt;
						if (op & 0x100)
						{
							uint32_t v = Read32(n, addr);
							r[13] = addr + 4;
							cyc += 2;
							SYNC_OUT;
							BxWritePc(n, v);
							SYNC_IN;
						}
						else
							r[13] = addr;
						break;
					}

					case 0xe:	// BKPT
						if (pc < ROM_SIZE && !real_rom)
						{
							SYNC_OUT;
							Hle(n, op & 0xff);
							SYNC_IN;
						}
						else
						{
							Log("rp2040: core%d BKPT #%d at %08x (r0=%08x r1=%08x lr=%08x)", n, op & 0xff, pc, r[0], r[1], r[14]);
							r[15] = pc;
							c.lockup = true;
						}
						irq_check[n] = true;
						break;

					case 0xf:	// hints
						switch ((op >> 4) & 15)
						{
							case 0: break;	// NOP
							case 1: break;	// YIELD
							case 2:			// WFE
								if (c.event)
									c.event = false;
								else
								{
									c.sleeping = true;
									c.wfe = true;
									SYNC_OUT;
									return;
								}
								break;
							case 3:			// WFI
								if (!PendingException(n) && !(c.nvic_pending & c.nvic_enable))
								{
									c.sleeping = true;
									c.wfe = false;
									SYNC_OUT;
									return;
								}
								break;
							case 4:			// SEV
								core[0].event = core[1].event = true;
								WakeOnEvent(n ^ 1);
								break;
							default: break;
						}
						break;

					default:
						r[15] = pc;
						SYNC_OUT;
						HardFault(n, "undefined misc", op);
						SYNC_IN;
						break;
				}
				break;

			case 0x18:	// STMIA Rn!, {list}
			{
				uint32_t rn = (op >> 8) & 7, addr = r[rn];
				uint32_t list = op & 0xff;
				for (int i = 0; i < 8; i++)
					if (list & (1u << i))
					{
						Write32(n, addr, r[i]);
						addr += 4;
					}
				r[rn] = addr;
				cyc += popcount32(list);
				break;
			}

			case 0x19:	// LDMIA Rn!, {list}
			{
				uint32_t rn = (op >> 8) & 7, addr = r[rn];
				uint32_t list = op & 0xff;
				for (int i = 0; i < 8; i++)
					if (list & (1u << i))
					{
						r[i] = Read32(n, addr);
						addr += 4;
					}
				if (!(list & (1u << rn)))
					r[rn] = addr;
				cyc += popcount32(list);
				break;
			}

			case 0x1a:
			case 0x1b:	// conditional branch, UDF, SVC
			{
				uint32_t cond = (op >> 8) & 15;
				if (cond == 0xe)
				{
					r[15] = pc;
					SYNC_OUT;
					HardFault(n, "UDF", op);
					SYNC_IN;
				}
				else if (cond == 0xf)
				{
					SYNC_OUT;
					TakeException(n, EXC_SVCALL);
					SYNC_IN;
				}
				else if (cond_pass(c, cond))
				{
					uint32_t target = pc + 4 + ((uint32_t) (int32_t) (int8_t) (op & 0xff) << 1);
					r[15] = target;
					cyc++;
					if (target <= pc && spin_detect(c, target))
					{
						if (cyc < until)
							cyc = until;
						SYNC_OUT;
						return;
					}
				}
				break;
			}

			case 0x1c:	// B
			{
				int32_t off = (int32_t) ((op & 0x7ff) << 21) >> 20;
				uint32_t target = pc + 4 + (uint32_t) off;
				r[15] = target;
				cyc++;
				if (off < 0 && off >= -256 && spin_detect(c, target))
				{
					if (cyc < until)
						cyc = until;
					SYNC_OUT;
					return;
				}
				break;
			}

			case 0x1d:
				r[15] = pc;
				SYNC_OUT;
				HardFault(n, "undefined 32-bit", op);
				SYNC_IN;
				break;

			case 0x1e:
			case 0x1f:	// 32-bit instructions
			{
				uint32_t op2 = Read16(n, pc + 2);
				r[15] = pc + 4;
				if ((op & 0xf800) == 0xf000 && (op2 & 0xd000) == 0xd000)
				{
					// BL
					uint32_t s = (op >> 10) & 1;
					uint32_t j1 = (op2 >> 13) & 1, j2 = (op2 >> 11) & 1;
					uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
					uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((op & 0x3ff) << 12) | ((op2 & 0x7ff) << 1);
					int32_t off = (int32_t) (imm << 7) >> 7;
					r[14] = (pc + 4) | 1;
					r[15] = pc + 4 + (uint32_t) off;
					cyc += 2;
				}
				else if ((op & 0xfff0) == 0xf380 && (op2 & 0xff00) == 0x8800)
				{
					// MSR
					uint32_t v = r[op & 15], sysm = op2 & 0xff;
					switch (sysm)
					{
						case 0: case 1: case 2: case 3:
							c.n = v >> 31;
							c.z = (v >> 30) & 1;
							c.c = (v >> 29) & 1;
							c.v = (v >> 28) & 1;
							break;
						case 8:
							c.msp = v & ~3u;
							if (c.ipsr || !(c.control & 2))
								r[13] = c.msp;
							break;
						case 9:
							c.psp = v & ~3u;
							if (!c.ipsr && (c.control & 2))
								r[13] = c.psp;
							break;
						case 16:
							c.primask = v & 1;
							irq_check[n] = true;
							break;
						case 20:
							if (!c.ipsr)
								SetSpSel(n, (v & 2) != 0);
							c.control = (c.control & 2) | (v & 1);
							break;
						default:
							break;
					}
					cyc += 2;
				}
				else if (op == 0xf3ef && (op2 & 0xf000) == 0x8000)
				{
					// MRS
					uint32_t sysm = op2 & 0xff, v = 0;
					uint32_t apsr = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28);
					switch (sysm)
					{
						case 0: case 2: v = apsr; break;
						case 1: case 3: v = apsr | c.ipsr; break;
						case 5: case 7: v = c.ipsr; break;
						case 6: v = 0; break;
						case 8: v = (c.ipsr || !(c.control & 2)) ? r[13] : c.msp; break;
						case 9: v = (!c.ipsr && (c.control & 2)) ? r[13] : c.psp; break;
						case 16: v = c.primask; break;
						case 20: v = c.control; break;
						default: break;
					}
					r[(op2 >> 8) & 15] = v;
					cyc += 2;
				}
				else if (op == 0xf3bf && (op2 & 0xff00) == 0x8f00)
				{
					// DSB/DMB/ISB
					cyc += 2;
				}
				else
				{
					r[15] = pc;
					SYNC_OUT;
					HardFault(n, "undefined 32-bit", (op << 16) | op2);
					SYNC_IN;
				}
				break;
			}
		}
	}
	SYNC_OUT;
#undef SYNC_OUT
#undef SYNC_IN
}

// ---------------------------------------------------------------------------
// Bus

uint32_t Chip::Read32 (int cpu, uint32_t a)
{
	if ((a - 0x20000000u) <= SRAM_SIZE - 4)
		return rd32(sram + (a - 0x20000000u));
	if ((a - 0x10000000u) < 0x04000000u)
		return rd32(&flash[a & flash_mask & ~3u]);
	return PeriphRead(cpu, a & ~3u);
}

uint16_t Chip::Read16 (int cpu, uint32_t a)
{
	if ((a - 0x20000000u) <= SRAM_SIZE - 2)
		return rd16(sram + (a - 0x20000000u));
	if ((a - 0x10000000u) < 0x04000000u)
		return rd16(&flash[a & flash_mask & ~1u]);
	return (uint16_t) (PeriphRead(cpu, a & ~3u) >> ((a & 2) * 8));
}

uint8_t Chip::Read8 (int cpu, uint32_t a)
{
	if ((a - 0x20000000u) < SRAM_SIZE)
		return sram[a - 0x20000000u];
	if ((a - 0x10000000u) < 0x04000000u)
		return flash[a & flash_mask];
	return (uint8_t) (PeriphRead(cpu, a & ~3u) >> ((a & 3) * 8));
}

// RAM stores feed the idle-loop hash; a register write always counts as
// progress, since writing a peripheral twice is rarely a no-op.
static inline void store_mix (Core &c, uint32_t a, uint32_t v)
{
	c.store_hash = (c.store_hash ^ a ^ (v * 0x9e3779b9u)) * 0x01000193u + 1;
}

static inline void store_progress (Core &c)
{
	c.spin_pc = 0;
}

void Chip::Write32 (int cpu, uint32_t a, uint32_t v)
{
	if ((a - 0x20000000u) <= SRAM_SIZE - 4)
	{
		store_mix(core[cpu & 1], a, v);
		wr32(sram + (a - 0x20000000u), v);
		return;
	}
	store_progress(core[cpu & 1]);
	PeriphWrite(cpu, a & ~3u, v, 4);
}

void Chip::Write16 (int cpu, uint32_t a, uint16_t v)
{
	if ((a - 0x20000000u) <= SRAM_SIZE - 2)
	{
		store_mix(core[cpu & 1], a, v);
		wr16(sram + (a - 0x20000000u), v);
		return;
	}
	store_progress(core[cpu & 1]);
	// Narrow writes to registers are replicated across the word.
	PeriphWrite(cpu, a, v | ((uint32_t) v << 16), 2);
}

void Chip::Write8 (int cpu, uint32_t a, uint8_t v)
{
	if ((a - 0x20000000u) < SRAM_SIZE)
	{
		store_mix(core[cpu & 1], a, v);
		sram[a - 0x20000000u] = v;
		return;
	}
	store_progress(core[cpu & 1]);
	PeriphWrite(cpu, a, v * 0x01010101u, 1);
}

static inline uint32_t sram_unstriped (uint32_t a)
{
	uint32_t bank = (a >> 16) & 3, o = a & 0xffff;
	return ((o >> 2) << 4) | (bank << 2) | (o & 3);
}

uint32_t Chip::PeriphRead (int cpu, uint32_t a)
{
	if (trace_io)
	{
		trace_io = false;
		uint32_t v = PeriphRead(cpu, a);
		trace_io = true;
		Log("c%d rd %08x -> %08x (pc=%08x)", cpu, a, v, core[cpu & 1].r[15]);
		return v;
	}
	switch (a >> 24)
	{
		case 0x00:
			if (a < ROM_SIZE)
				return rd32(rom + a);
			break;
		case 0x14:
			switch (a & 0xff)
			{
				case 0x00: return xip_ctrl;
				case 0x08: return 0x3;
				case 0x14: return xip_stream_addr;
				case 0x18: return xip_stream_ctr;
				case 0x1c:
					if (xip_stream_ctr)
					{
						uint32_t v = rd32(&flash[xip_stream_addr & flash_mask & ~3u]);
						xip_stream_addr += 4;
						xip_stream_ctr--;
						return v;
					}
					return 0;
				default: return 0;
			}
		case 0x15:
			if ((a & 0xffffff) < XIP_SRAM_SIZE)
				return rd32(xip_sram + (a & 0x3fff));
			break;
		case 0x18:
			return SsiRead(a & 0xfff);
		case 0x20:
			if ((a - 0x20000000u) < SRAM_SIZE)
				return rd32(sram + (a - 0x20000000u));
			break;
		case 0x21:
			if ((a & 0xffffff) < 0x40000)
				return rd32(sram + sram_unstriped(a));
			break;
		case 0x40:
			return ApbRead(a & ~0x3000u);
		case 0x50:
		{
			uint32_t base = a & ~0x3000u;
			if (base < 0x50001000)
				return DmaRead(base & 0xfff);
			if ((base & 0xfff00000) == 0x50100000)
			{
				if (base < 0x50101000)
					return rd32(usb_ram + (base & 0xfff));
				return 0;
			}
			if ((base & 0xfff00000) == 0x50200000)
				return PioRead(0, base & 0xfff);
			if ((base & 0xfff00000) == 0x50300000)
				return PioRead(1, base & 0xfff);
			if ((base & 0xfff00000) == 0x50400000)
			{
				if ((base & 0xfff) == 0 && xip_stream_ctr)
				{
					uint32_t v = rd32(&flash[xip_stream_addr & flash_mask & ~3u]);
					xip_stream_addr += 4;
					xip_stream_ctr--;
					return v;
				}
				return 0;
			}
			break;
		}
		case 0xd0:
			return SioRead(cpu, a & 0xfff);
		case 0xe0:
			return PpbRead(cpu, a & 0xfffff);
		default:
			break;
	}
	Log("rp2040: core%d read of unmapped %08x (pc=%08x)", cpu, a, core[cpu & 1].r[15]);
	return 0;
}

void Chip::PeriphWrite (int cpu, uint32_t a, uint32_t v, int size)
{
	uint32_t wa = a & ~3u;
	if (trace_io)
		Log("c%d wr %08x <- %08x/%d (pc=%08x)", cpu, a, v, size, core[cpu & 1].r[15]);
	switch (a >> 24)
	{
		case 0x14:
			switch (a & 0xfc)
			{
				case 0x00: xip_ctrl = v; return;
				case 0x14: xip_stream_addr = v & ~3u; return;
				case 0x18: xip_stream_ctr = v & 0x3fffff; ServiceDma(); return;
				default: return;
			}
		case 0x15:
			if ((a & 0xffffff) < XIP_SRAM_SIZE)
			{
				if (size == 4)
					wr32(xip_sram + (a & 0x3ffc), v);
				else if (size == 2)
					wr16(xip_sram + (a & 0x3ffe), (uint16_t) v);
				else
					xip_sram[a & 0x3fff] = (uint8_t) v;
				return;
			}
			break;
		case 0x18:
			SsiWrite(wa & 0xfff, v);
			return;
		case 0x21:
			if ((a & 0xffffff) < 0x40000)
			{
				uint32_t o = sram_unstriped(a);
				if (size == 4)
					wr32(sram + (o & ~3u), v);
				else if (size == 2)
					wr16(sram + (o & ~1u), (uint16_t) v);
				else
					sram[o] = (uint8_t) v;
				return;
			}
			break;
		case 0x40:
			ApbWrite(wa & ~0x3000u, v, (wa >> 12) & 3);
			irq_dirty = true;
			return;
		case 0x50:
		{
			uint32_t base = wa & ~0x3000u;
			int alias = (wa >> 12) & 3;
			irq_dirty = true;
			if (base < 0x50001000)
			{
				DmaWrite(base & 0xfff, v, alias);
				return;
			}
			if ((base & 0xfff00000) == 0x50100000)
			{
				if (base < 0x50101000)
				{
					if (size == 4)
						wr32(usb_ram + (a & 0xffc), v);
					else if (size == 2)
						wr16(usb_ram + (a & 0xffe), (uint16_t) v);
					else
						usb_ram[a & 0xfff] = (uint8_t) v;
				}
				return;
			}
			if ((base & 0xfff00000) == 0x50200000)
			{
				PioWrite(0, base & 0xfff, v, alias);
				return;
			}
			if ((base & 0xfff00000) == 0x50300000)
			{
				PioWrite(1, base & 0xfff, v, alias);
				return;
			}
			if ((base & 0xfff00000) == 0x50400000)
				return;
			break;
		}
		case 0xd0:
			SioWrite(cpu, wa & 0xfff, v);
			irq_dirty = true;
			return;
		case 0xe0:
			PpbWrite(cpu, wa & 0xfffff, v);
			irq_check[cpu & 1] = true;
			return;
		default:
			break;
	}
	Log("rp2040: core%d write of unmapped %08x = %08x (pc=%08x)", cpu, a, v, core[cpu & 1].r[15]);
}

// ---------------------------------------------------------------------------
// SIO

void Chip::InterpResult (Core::Interp &it, int i, uint32_t res[3], bool &overf)
{
	uint32_t sm[2], raw[2];
	bool of[2];
	for (int l = 0; l < 2; l++)
	{
		uint32_t ctrl = it.ctrl[l];
		uint32_t input = (ctrl & (1u << 16)) ? it.accum[l ^ 1] : it.accum[l];
		uint32_t shift = ctrl & 31;
		uint32_t lsb = (ctrl >> 5) & 31, msb = (ctrl >> 10) & 31;
		uint32_t shifted = input >> shift;
		uint32_t mask = (msb >= lsb) ? ((msb == 31 ? 0xffffffffu : ((2u << msb) - 1)) & ~((1u << lsb) - 1)) : 0;
		uint32_t v = shifted & mask;
		of[l] = (shifted & ~((msb == 31) ? 0xffffffffu : ((2u << msb) - 1))) != 0;
		if ((ctrl & (1u << 15)) && msb < 31 && (v & (1u << msb)))
			v |= ~((2u << msb) - 1);
		sm[l] = v;
		raw[l] = input;
	}
	overf = of[0] || of[1];
	uint32_t c0 = it.ctrl[0], c1 = it.ctrl[1];
	bool blend = (i == 0) && (c0 & (1u << 21));
	bool clamp = (i == 1) && (c0 & (1u << 22));

	if (blend)
	{
		uint32_t alpha = sm[1] & 0xff;
		if (c1 & (1u << 15))
			res[1] = (uint32_t) ((int32_t) it.base[0] + (((int32_t) it.base[1] - (int32_t) it.base[0]) * (int32_t) alpha >> 8));
		else
			res[1] = it.base[0] + (uint32_t) (((uint64_t) (it.base[1] - it.base[0]) * alpha) >> 8);
		res[0] = alpha;
		res[2] = it.base[2] + sm[0];
	}
	else
	{
		res[0] = it.base[0] + ((c0 & (1u << 18)) ? raw[0] : sm[0]);
		res[1] = it.base[1] + ((c1 & (1u << 18)) ? raw[1] : sm[1]);
		res[2] = it.base[2] + sm[0] + sm[1];
		if (clamp)
		{
			uint32_t v = sm[0];
			if (c0 & (1u << 15))
			{
				if ((int32_t) v < (int32_t) it.base[0]) v = it.base[0];
				if ((int32_t) v > (int32_t) it.base[1]) v = it.base[1];
			}
			else
			{
				if (v < it.base[0]) v = it.base[0];
				if (v > it.base[1]) v = it.base[1];
			}
			res[0] = v;
		}
	}
	res[0] |= ((c0 >> 19) & 3) << 28;
	res[1] |= ((c1 >> 19) & 3) << 28;
}

uint32_t Chip::InterpRead (int cpu, int i, uint32_t off)
{
	Core::Interp &it = core[cpu].interp[i];
	uint32_t res[3];
	bool overf;
	switch (off)
	{
		case 0x00: return it.accum[0];
		case 0x04: return it.accum[1];
		case 0x08: return it.base[0];
		case 0x0c: return it.base[1];
		case 0x10: return it.base[2];
		case 0x14: case 0x18: case 0x1c:
		{
			InterpResult(it, i, res, overf);
			uint32_t v = res[(off - 0x14) / 4];
			it.accum[0] = (it.ctrl[0] & (1u << 17)) ? res[1] : res[0];
			it.accum[1] = (it.ctrl[1] & (1u << 17)) ? res[0] : res[1];
			return v;
		}
		case 0x20: case 0x24: case 0x28:
			InterpResult(it, i, res, overf);
			return res[(off - 0x20) / 4];
		case 0x2c:
		{
			InterpResult(it, i, res, overf);
			uint32_t of0, of1;
			{
				Core::Interp t = it;
				uint32_t r2[3];
				bool o;
				t.ctrl[1] &= ~0x3ffu;
				t.ctrl[1] |= 0x3e0;		// empty lane-1 mask
				InterpResult(t, i, r2, o);
				of0 = o;
				t = it;
				t.ctrl[0] &= ~0x3ffu;
				t.ctrl[0] |= 0x3e0;
				InterpResult(t, i, r2, o);
				of1 = o;
			}
			return (it.ctrl[0] & 0x7fffff) | (of0 << 23) | (of1 << 24) | ((overf ? 1u : 0u) << 25);
		}
		case 0x30: return it.ctrl[1];
		case 0x34:
		case 0x38:
		{
			InterpResult(it, i, res, overf);
			int l = (off - 0x34) / 4;
			return res[l] - it.base[l];
		}
		default: return 0;
	}
}

void Chip::InterpWrite (int cpu, int i, uint32_t off, uint32_t v)
{
	Core::Interp &it = core[cpu].interp[i];
	switch (off)
	{
		case 0x00: it.accum[0] = v; break;
		case 0x04: it.accum[1] = v; break;
		case 0x08: it.base[0] = v; break;
		case 0x0c: it.base[1] = v; break;
		case 0x10: it.base[2] = v; break;
		case 0x14: case 0x18: case 0x1c: break;
		case 0x2c: it.ctrl[0] = v & 0x7fffff; break;
		case 0x30: it.ctrl[1] = v & 0x1fffff; break;
		case 0x34: it.accum[0] += v; break;
		case 0x38: it.accum[1] += v; break;
		case 0x3c:
			it.base[0] = (it.ctrl[0] & (1u << 15)) ? (uint32_t) (int32_t) (int16_t) v : (v & 0xffff);
			it.base[1] = (it.ctrl[1] & (1u << 15)) ? (uint32_t) (int32_t) (int16_t) (v >> 16) : (v >> 16);
			break;
		default: break;
	}
}

uint32_t Chip::SioRead (int cpu, uint32_t off)
{
	Core &c = core[cpu];
	if (off >= 0x080 && off < 0x100)
		return InterpRead(cpu, (off >> 6) & 1, off & 0x3f);
	if (off >= 0x100 && off < 0x180)
	{
		uint32_t bit = 1u << ((off - 0x100) >> 2);
		if (spinlocks & bit)
			return 0;
		spinlocks |= bit;
		return bit;
	}
	switch (off)
	{
		case 0x000: return cpu;
		case 0x004:
		{
			uint32_t out = GpioOutputs();
			return out;
		}
		case 0x008: return 0x02;					// QSPI: SS high
		case 0x010: return sio_out;
		case 0x020: return sio_oe;
		case 0x030: return sio_hi_out;
		case 0x040: return sio_hi_oe;
		case 0x050:
		{
			uint32_t rx = cpu ^ 1;				// fifo written by the other core
			uint32_t st = 0;
			if (fifo_level[rx])
				st |= 1;
			if (fifo_level[cpu] < 8)
				st |= 2;
			return st | fifo_sticky[cpu];
		}
		case 0x058:
		{
			uint32_t rx = cpu ^ 1;
			if (!fifo_level[rx])
			{
				fifo_sticky[cpu] |= 8;
				return 0;
			}
			uint32_t v = fifo[rx][fifo_head[rx]];
			fifo_head[rx] = (fifo_head[rx] + 1) & 7;
			fifo_level[rx]--;
			irq_dirty = true;
			return v;
		}
		case 0x05c: return spinlocks;
		case 0x060: return c.div_udividend;
		case 0x064: return c.div_udivisor;
		case 0x068: return c.div_udividend;
		case 0x06c: return c.div_udivisor;
		case 0x070: c.div_dirty = false; return c.div_quot;
		case 0x074: return c.div_rem;
		case 0x078: return 1 | (c.div_dirty ? 2 : 0);
		default: return 0;
	}
}

void Chip::SioWrite (int cpu, uint32_t off, uint32_t v)
{
	Core &c = core[cpu];
	if (off >= 0x080 && off < 0x100)
	{
		InterpWrite(cpu, (off >> 6) & 1, off & 0x3f, v);
		return;
	}
	if (off >= 0x100 && off < 0x180)
	{
		spinlocks &= ~(1u << ((off - 0x100) >> 2));
		return;
	}
	switch (off)
	{
		case 0x010: sio_out = v & 0x3fffffff; break;
		case 0x014: sio_out |= v & 0x3fffffff; break;
		case 0x018: sio_out &= ~v; break;
		case 0x01c: sio_out ^= v & 0x3fffffff; break;
		case 0x020: sio_oe = v & 0x3fffffff; break;
		case 0x024: sio_oe |= v & 0x3fffffff; break;
		case 0x028: sio_oe &= ~v; break;
		case 0x02c: sio_oe ^= v & 0x3fffffff; break;
		case 0x030: sio_hi_out = v & 0x3f; break;
		case 0x034: sio_hi_out |= v & 0x3f; break;
		case 0x038: sio_hi_out &= ~v; break;
		case 0x03c: sio_hi_out ^= v & 0x3f; break;
		case 0x040: sio_hi_oe = v & 0x3f; break;
		case 0x044: sio_hi_oe |= v & 0x3f; break;
		case 0x048: sio_hi_oe &= ~v; break;
		case 0x04c: sio_hi_oe ^= v & 0x3f; break;
		case 0x050: fifo_sticky[cpu] = 0; break;
		case 0x054:
			if (fifo_level[cpu] >= 8)
				fifo_sticky[cpu] |= 4;
			else
			{
				fifo[cpu][(fifo_head[cpu] + fifo_level[cpu]) & 7] = v;
				fifo_level[cpu]++;
			}
			if (cpu == 0 && core[1].halted && !core[1].lockup)
				Core1LaunchStep();
			break;
		case 0x060: case 0x068:
		case 0x064: case 0x06c:
		{
			bool sgn = (off == 0x068 || off == 0x06c);
			if (off == 0x060 || off == 0x068)
				c.div_udividend = v;
			else
				c.div_udivisor = v;
			uint32_t a = c.div_udividend, b = c.div_udivisor;
			if (sgn)
			{
				int32_t sa = (int32_t) a, sb = (int32_t) b;
				if (!sb)
				{
					c.div_quot = (sa < 0) ? 1 : 0xffffffffu;
					c.div_rem = a;
				}
				else if (sa == INT32_MIN && sb == -1)
				{
					c.div_quot = 0x80000000u;
					c.div_rem = 0;
				}
				else
				{
					c.div_quot = (uint32_t) (sa / sb);
					c.div_rem = (uint32_t) (sa % sb);
				}
			}
			else
			{
				if (!b)
				{
					c.div_quot = 0xffffffffu;
					c.div_rem = a;
				}
				else
				{
					c.div_quot = a / b;
					c.div_rem = a % b;
				}
			}
			c.div_dirty = true;
			break;
		}
		case 0x070: c.div_quot = v; c.div_dirty = true; break;
		case 0x074: c.div_rem = v; c.div_dirty = true; break;
		default: break;
	}
}

// ---------------------------------------------------------------------------
// Private peripheral bus: NVIC, SysTick, SCB

void Chip::SysTickAdvance (int n)
{
	Core &c = core[n];
	uint64_t t = c.cycles > now ? c.cycles : now;
	if (t < c.systick_last)
		return;
	uint64_t elapsed = t - c.systick_last;
	if (!(c.systick_csr & 1))
	{
		c.systick_last = t;
		return;
	}
	uint64_t ticks;
	if (c.systick_csr & 4)
	{
		ticks = elapsed;
		c.systick_last = t;
	}
	else
	{
		// External reference: the 1us watchdog tick
		uint64_t per = sys_hz / 1000000;
		if (!per)
			per = 1;
		ticks = elapsed / per;
		c.systick_last += ticks * per;
	}
	if (!ticks)
		return;
	uint32_t rvr = c.systick_rvr & 0xffffff;
	if (c.systick_cvr == 0)
	{
		c.systick_cvr = rvr;
		ticks--;
	}
	if (ticks < c.systick_cvr)
	{
		c.systick_cvr -= (uint32_t) ticks;
		return;
	}
	ticks -= c.systick_cvr;
	c.systick_cvr = 0;
	c.systick_csr |= 1u << 16;
	if (c.systick_csr & 2)
	{
		c.systick_pend = true;
		irq_check[n] = true;
	}
	if (rvr && ticks)
	{
		uint64_t period = (uint64_t) rvr + 1;
		ticks %= period;
		c.systick_cvr = ticks ? rvr - (uint32_t) (ticks - 1) : 0;
	}
}

uint32_t Chip::PpbRead (int cpu, uint32_t off)
{
	Core &c = core[cpu];
	if (off >= 0xe400 && off < 0xe420)
	{
		uint32_t i = off - 0xe400;
		return c.nvic_prio[i] | (c.nvic_prio[i + 1] << 8) | (c.nvic_prio[i + 2] << 16) | ((uint32_t) c.nvic_prio[i + 3] << 24);
	}
	switch (off)
	{
		case 0xe010:
		{
			SysTickAdvance(cpu);
			uint32_t v = c.systick_csr;
			c.systick_csr &= ~(1u << 16);
			return v;
		}
		case 0xe014: return c.systick_rvr;
		case 0xe018: SysTickAdvance(cpu); return c.systick_cvr;
		case 0xe01c: return 0x0000270f;
		case 0xe100: return c.nvic_enable;
		case 0xe180: return c.nvic_enable;
		case 0xe200: return c.nvic_pending;
		case 0xe280: return c.nvic_pending;
		case 0xed00: return 0x410cc601;
		case 0xed04:
		{
			int pend = 0, best = 5;
			for (int e = 1; e < 48; e++)
			{
				bool p = (e == EXC_PENDSV && c.pendsv) || (e == EXC_SYSTICK && c.systick_pend) ||
						 (e >= EXC_IRQ0 && (c.nvic_pending & c.nvic_enable & (1u << (e - EXC_IRQ0))));
				if (p && ExceptionPriority(cpu, e) < best)
				{
					best = ExceptionPriority(cpu, e);
					pend = e;
				}
			}
			return c.ipsr | (pend << 12) | ((c.nvic_pending & c.nvic_enable) ? (1u << 22) : 0) |
				   (c.systick_pend ? (1u << 26) : 0) | (c.pendsv ? (1u << 28) : 0);
		}
		case 0xed08: return c.vtor;
		case 0xed0c: return 0xfa050000;
		case 0xed10: return c.scr;
		case 0xed14: return 0x00000204;
		case 0xed1c: return c.shpr2;
		case 0xed20: return c.shpr3;
		case 0xed24: return (c.exc_active & (1u << EXC_SVCALL)) ? (1u << 15) : 0;
		case 0xed90: return 0x00000800;
		default: return 0;
	}
}

void Chip::PpbWrite (int cpu, uint32_t off, uint32_t v)
{
	Core &c = core[cpu];
	if (off >= 0xe400 && off < 0xe420)
	{
		uint32_t i = off - 0xe400;
		for (int k = 0; k < 4; k++)
			c.nvic_prio[i + k] = (uint8_t) ((v >> (8 * k)) & 0xc0);
		return;
	}
	switch (off)
	{
		case 0xe010:
			SysTickAdvance(cpu);
			c.systick_csr = (c.systick_csr & (1u << 16)) | (v & 7);
			break;
		case 0xe014: c.systick_rvr = v & 0xffffff; break;
		case 0xe018:
			SysTickAdvance(cpu);
			c.systick_cvr = 0;
			c.systick_csr &= ~(1u << 16);
			break;
		case 0xe100: c.nvic_enable |= v; break;
		case 0xe180: c.nvic_enable &= ~v; break;
		case 0xe200: c.nvic_pending |= v; break;
		case 0xe280: c.nvic_pending &= ~v; break;
		case 0xed04:
			if (v & (1u << 28)) c.pendsv = true;
			if (v & (1u << 27)) c.pendsv = false;
			if (v & (1u << 26)) c.systick_pend = true;
			if (v & (1u << 25)) c.systick_pend = false;
			break;
		case 0xed08: c.vtor = v & 0xffffff00; break;
		case 0xed0c:
			if ((v >> 16) == 0x05fa && (v & 4))
			{
				Log("rp2040: core%d SYSRESETREQ", cpu);
				reset_request = true;
			}
			break;
		case 0xed10: c.scr = v & 0x16; break;
		case 0xed1c: c.shpr2 = v & 0xc0000000; break;
		case 0xed20: c.shpr3 = v & 0xc0c00000; break;
		default: break;
	}
}

// ---------------------------------------------------------------------------
// Clocks, timer, GPIO and the rest of APB

static uint32_t pll_hz (const uint32_t *pll)
{
	uint32_t refdiv = pll[0] & 0x3f, fbdiv = pll[2] & 0xfff;
	uint32_t pd1 = (pll[3] >> 16) & 7, pd2 = (pll[3] >> 12) & 7;
	if (!refdiv || !fbdiv || !pd1 || !pd2 || (pll[1] & 1))
		return 0;
	return (uint32_t) ((uint64_t) XOSC_HZ / refdiv * fbdiv / (pd1 * pd2));
}

static uint32_t div_hz (uint32_t hz, uint32_t div)
{
	if (!div)
		div = 1u << 16;		// 0 means 65536 for the integer part
	return (uint32_t) (((uint64_t) hz << 8) / div);
}

void Chip::UpdateClocks ()
{
	uint32_t ref_ctrl = clk_ctrl[4], sys_ctrl = clk_ctrl[5];
	uint32_t ref_src;
	switch (ref_ctrl & 3)
	{
		case 0:  ref_src = ROSC_HZ; break;
		case 2:  ref_src = XOSC_HZ; break;
		default: ref_src = pll_hz(pll_usb); break;
	}
	uint32_t ref_hz = ref_src / (((clk_div[4] >> 8) & 3) ? ((clk_div[4] >> 8) & 3) : 1);
	uint32_t src;
	if (sys_ctrl & 1)
	{
		switch ((sys_ctrl >> 5) & 7)
		{
			case 0:  src = pll_hz(pll_sys); break;
			case 1:  src = pll_hz(pll_usb); break;
			case 2:  src = ROSC_HZ; break;
			case 3:  src = XOSC_HZ; break;
			default: src = XOSC_HZ; break;
		}
	}
	else
		src = ref_hz;
	uint32_t hz = div_hz(src, clk_div[5]);
	if (hz < 1000000)
		hz = 1000000;
	if (hz != sys_hz)
	{
		AdvanceTimer();
		sys_hz = hz;
		Log("rp2040: clk_sys = %u Hz", hz);
	}
}

void Chip::AdvanceTimer ()
{
	if (now <= us_last_cycles)
		return;
	uint64_t delta = now - us_last_cycles;
	us_last_cycles = now;
	if (timer_pause)
		return;
	us_rem += delta * 1000000ull;
	us_count += us_rem / sys_hz;
	us_rem %= sys_hz;

	for (int i = 0; i < 4; i++)
	{
		if (!(timer_armed & (1u << i)))
			continue;
		if ((int32_t) ((uint32_t) us_count - timer_alarm[i]) >= 0)
		{
			timer_armed &= ~(1u << i);
			timer_intr |= 1u << i;
		}
	}
}

uint64_t Chip::NextTimerEvent ()
{
	uint64_t best = ~0ull;
	for (int i = 0; i < 4; i++)
	{
		if (!(timer_armed & (1u << i)))
			continue;
		int32_t d = (int32_t) (timer_alarm[i] - (uint32_t) us_count);
		uint64_t cyc = (d <= 0) ? 1 : (((uint64_t) d * sys_hz + 999999) / 1000000);
		if (now + cyc < best)
			best = now + cyc;
	}
	for (int n = 0; n < 2; n++)
	{
		Core &c = core[n];
		if ((c.systick_csr & 3) == 3 && !c.halted)
		{
			uint64_t per = (c.systick_csr & 4) ? 1 : (sys_hz / 1000000);
			uint64_t cyc = ((uint64_t) (c.systick_cvr ? c.systick_cvr : (c.systick_rvr & 0xffffff) + 1) + 1) * per;
			if (now + cyc < best)
				best = now + cyc;
		}
	}
	return best;
}

// Pad function selects and overrides as masks, rebuilt whenever a GPIO
// control register changes.
void Chip::RecalcGpioMux ()
{
	mux_sio = mux_pio0 = mux_pio1 = 0;
	out_inv = out_force = out_force_val = 0;
	oe_inv = oe_force = oe_force_val = 0;
	in_inv = in_force = in_force_val = 0;
	for (int i = 0; i < NUM_GPIO; i++)
	{
		uint32_t ctrl = gpio_ctrl[i], bit = 1u << i;
		switch (ctrl & 0x1f)
		{
			case 5: mux_sio |= bit; break;
			case 6: mux_pio0 |= bit; break;
			case 7: mux_pio1 |= bit; break;
			default: break;
		}
		switch ((ctrl >> 8) & 3)
		{
			case 1: out_inv |= bit; break;
			case 2: out_force |= bit; break;
			case 3: out_force |= bit; out_force_val |= bit; break;
		}
		switch ((ctrl >> 12) & 3)
		{
			case 1: oe_inv |= bit; break;
			case 2: oe_force |= bit; break;
			case 3: oe_force |= bit; oe_force_val |= bit; break;
		}
		switch ((ctrl >> 16) & 3)
		{
			case 1: in_inv |= bit; break;
			case 2: in_force |= bit; break;
			case 3: in_force |= bit; in_force_val |= bit; break;
		}
	}
}

uint32_t Chip::GpioOutputs ()
{
	uint32_t out = (sio_out & mux_sio) | (pio[0].pin_out & mux_pio0) | (pio[1].pin_out & mux_pio1);
	uint32_t oe = (sio_oe & mux_sio) | (pio[0].pin_oe & mux_pio0) | (pio[1].pin_oe & mux_pio1);
	out = ((out ^ out_inv) & ~out_force) | out_force_val;
	oe = (((oe ^ oe_inv) & ~oe_force) | oe_force_val) & ~gpio_ext_drive;
	uint32_t pad = (out & oe) | (gpio_ext & ~oe);
	return (((pad ^ in_inv) & ~in_force) | in_force_val) & 0x3fffffff;
}

void Chip::UpdateIrqLines ()
{
	// GPIO edge/level detection, only while some pin interrupt is enabled
	uint32_t any_inte = 0;
	for (int w = 0; w < 4; w++)
		any_inte |= gpio_inte[0][w] | gpio_inte[1][w] | gpio_intf[0][w] | gpio_intf[1][w];
	uint32_t lv = any_inte ? GpioOutputs() : gpio_prev;
	uint32_t rise = lv & ~gpio_prev, fall = ~lv & gpio_prev;
	gpio_prev = lv;
	for (int i = 0; any_inte && i < NUM_GPIO; i++)
	{
		uint32_t word = i >> 3, sh = (i & 7) * 4;
		uint32_t bits = 0;
		if (!((lv >> i) & 1)) bits |= 1;
		if ((lv >> i) & 1) bits |= 2;
		if ((fall >> i) & 1) bits |= 4;
		if ((rise >> i) & 1) bits |= 8;
		gpio_intr[word] = (gpio_intr[word] & ~(3u << sh)) | ((bits & 3) << sh) | ((bits & 12) << sh);
	}

	for (int n = 0; n < 2; n++)
	{
		Core &c = core[n];
		uint32_t lines = 0;
		uint32_t ts = (timer_intr | timer_intf) & timer_inte;
		lines |= ts & 0xf;
		for (int p = 0; p < 2; p++)
		{
			if (!(pio[p].irq_inte[0] | pio[p].irq_inte[1] | pio[p].irq_intf[0] | pio[p].irq_intf[1]))
				continue;
			uint32_t intr = PioIntr(p);
			if ((intr & pio[p].irq_inte[0]) | pio[p].irq_intf[0])
				lines |= 1u << (IRQ_PIO0_0 + 2 * p);
			if ((intr & pio[p].irq_inte[1]) | pio[p].irq_intf[1])
				lines |= 1u << (IRQ_PIO0_1 + 2 * p);
		}
		if ((dma_intr & dma_inte[0]) | dma_intf[0])
			lines |= 1u << IRQ_DMA_0;
		if ((dma_intr & dma_inte[1]) | dma_intf[1])
			lines |= 1u << IRQ_DMA_1;
		for (int w = 0; w < 4; w++)
			if ((gpio_intr[w] & gpio_inte[n][w]) | gpio_intf[n][w])
				lines |= 1u << IRQ_IO_BANK0;
		uint32_t rx = n ^ 1;
		if (fifo_level[rx] || (fifo_sticky[n] & 0xc))
			lines |= 1u << (n ? IRQ_SIO_PROC1 : IRQ_SIO_PROC0);
		uint32_t newp = lines & ~c.nvic_active & ~c.nvic_pending;
		c.nvic_pending |= lines & ~c.nvic_active;
		if (newp && (c.scr & 0x10))
			c.event = true;
		if (c.nvic_pending & c.nvic_enable)
			irq_check[n] = true;
		if (c.pendsv || c.systick_pend)
			irq_check[n] = true;
	}
}

void Chip::WakeOnEvent (int n)
{
	Core &c = core[n];
	if (c.sleeping && c.wfe && c.event)
	{
		c.event = false;
		c.sleeping = false;
	}
}

uint32_t Chip::ApbRead (uint32_t a)
{
	uint32_t off = a & 0xfff;
	switch (a & 0xfffff000)
	{
		case 0x40000000:	// SYSINFO
			if (off == 0x00) return 0x20002927;
			if (off == 0x04) return 0x00000002;
			return 0;
		case 0x40004000:	// SYSCFG
			return off < 0x20 ? syscfg[off >> 2] : 0;
		case 0x40008000:	// CLOCKS
			if (off < 0x78)
			{
				uint32_t idx = off / 12, reg = off % 12;
				if (reg == 0) return clk_ctrl[idx];
				if (reg == 4) return clk_div[idx];
				if (idx == 4) return 1u << (clk_ctrl[4] & 3);
				if (idx == 5) return 1u << (clk_ctrl[5] & 1);
				return 1;
			}
			if (off == 0x98) return 0x10;
			if (off == 0x9c)
			{
				uint32_t khz = sys_hz / 1000;
				return khz << 5;
			}
			return 0;
		case 0x4000c000:	// RESETS
			if (off == 0x0) return resets;
			if (off == 0x8) return ~resets & 0x01ffffff;
			return 0;
		case 0x40010000:	// PSM
			if (off == 0x0) return psm_frce_on;
			if (off == 0x4) return psm_frce_off;
			if (off == 0xc) return 0x1ffff & ~psm_frce_off;
			return 0;
		case 0x40014000:	// IO_BANK0
		{
			if (off < 0xf0)
			{
				int pin = off >> 3;
				if (pin >= NUM_GPIO)
					return 0;
				if (off & 4)
					return gpio_ctrl[pin];
				uint32_t lv = GpioOutputs();
				return ((lv >> pin) & 1) << 17 | ((lv >> pin) & 1) << 19;
			}
			if (off < 0x100) return gpio_intr[(off - 0xf0) >> 2];
			if (off < 0x110) return gpio_inte[0][(off - 0x100) >> 2];
			if (off < 0x120) return gpio_intf[0][(off - 0x110) >> 2];
			if (off < 0x130) { int w = (off - 0x120) >> 2; return (gpio_intr[w] & gpio_inte[0][w]) | gpio_intf[0][w]; }
			if (off < 0x140) return gpio_inte[1][(off - 0x130) >> 2];
			if (off < 0x150) return gpio_intf[1][(off - 0x140) >> 2];
			if (off < 0x160) { int w = (off - 0x150) >> 2; return (gpio_intr[w] & gpio_inte[1][w]) | gpio_intf[1][w]; }
			return 0;
		}
		case 0x40018000:	// IO_QSPI
			if (off < 0x30 && (off & 4)) return qspi_ctrl[off >> 3];
			return 0;
		case 0x4001c000:	// PADS_BANK0
			return off < 0x80 ? pads[off >> 2] : 0;
		case 0x40020000:	// PADS_QSPI
			return off < 0x1c ? qspi_pads[off >> 2] : 0;
		case 0x40024000:	// XOSC
			if (off == 0x00) return xosc_ctrl;
			if (off == 0x04) return 0x80001000 | ((xosc_ctrl >> 12) == 0xfab ? 0 : 0);
			if (off == 0x0c) return xosc_startup;
			return 0;
		case 0x40028000:	// PLL_SYS
		case 0x4002c000:	// PLL_USB
		{
			uint32_t *pll = ((a & 0xfffff000) == 0x40028000) ? pll_sys : pll_usb;
			if (off == 0x0) return pll[0] | 0x80000000;
			if (off < 0x10) return pll[off >> 2];
			return 0;
		}
		case 0x40030000:	// BUSCTRL
			if (off == 0x0) return busctrl_priority;
			if (off == 0x4) return 1;
			return 0;
		case 0x40034000:	// UART0
		case 0x40038000:	// UART1
		{
			int u = ((a & 0xfffff000) == 0x40038000);
			switch (off)
			{
				case 0x18: return 0x90;		// TXFE | RXFE
				case 0x24: return uart_ibrd[u];
				case 0x28: return uart_fbrd[u];
				case 0x2c: return uart_lcr[u];
				case 0x30: return uart_cr[u];
				case 0x38: return uart_imsc[u];
				case 0x3c: return 0x20;		// TX ready
				case 0x40: return 0x20 & uart_imsc[u];
				case 0xfe0: return 0x11;
				case 0xfe4: return 0x10;
				case 0xfe8: return 0x34;
				case 0xfec: return 0x00;
				default: return 0;
			}
		}
		case 0x4003c000:	// SPI0
		case 0x40040000:	// SPI1
			if (off == 0x0c) return 0x03;
			return 0;
		case 0x4004c000:	// ADC
			if (off == 0x00) return adc_cs | 0x100;
			if (off == 0x04) return adc_result;
			if (off == 0x08) return adc_fcs | (1u << 8);
			if (off == 0x0c) return adc_result;
			if (off == 0x10) return adc_div;
			return 0;
		case 0x40050000:	// PWM
			return off < 0xb4 ? pwm[off >> 2] : 0;
		case 0x40054000:	// TIMER
			AdvanceTimer();
			switch (off)
			{
				case 0x08: return timer_latched_hi;
				case 0x0c: timer_latched_hi = (uint32_t) (us_count >> 32); return (uint32_t) us_count;
				case 0x10: case 0x14: case 0x18: case 0x1c: return timer_alarm[(off - 0x10) >> 2];
				case 0x20: return timer_armed;
				case 0x24: return (uint32_t) (us_count >> 32);
				case 0x28: return (uint32_t) us_count;
				case 0x30: return timer_pause;
				case 0x34: return timer_intr;
				case 0x38: return timer_inte;
				case 0x3c: return timer_intf;
				case 0x40: return (timer_intr | timer_intf) & timer_inte;
				default: return 0;
			}
		case 0x40058000:	// WATCHDOG
			if (off == 0x00) return watchdog_ctrl;
			if (off == 0x08) return watchdog_reason;
			if (off >= 0x0c && off < 0x2c) return watchdog_scratch[(off - 0x0c) >> 2];
			if (off == 0x2c) return watchdog_tick | ((watchdog_tick & 0x200) ? 0x400 : 0);
			return 0;
		case 0x4005c000:	// RTC
			return 0;
		case 0x40060000:	// ROSC
			if (off == 0x00) return rosc_ctrl;
			if (off == 0x18) return 0x80001000;
			if (off == 0x1c)
			{
				uint32_t bit = ((rosc_lfsr >> 0) ^ (rosc_lfsr >> 2) ^ (rosc_lfsr >> 3) ^ (rosc_lfsr >> 5)) & 1;
				rosc_lfsr = (rosc_lfsr >> 1) | (bit << 15);
				return bit;
			}
			return 0;
		case 0x40064000:	// VREG_AND_CHIP_RESET
			if (off == 0x0) return vreg | 0x1000;
			if (off == 0x4) return bod;
			if (off == 0x8) return chip_reset;
			return 0;
		case 0x4006c000:	// TBMAN
			return off == 0 ? 5 : 0;
		default:
			return 0;
	}
}

void Chip::ApbWrite (uint32_t a, uint32_t v, int alias)
{
	uint32_t off = a & 0xfff;
	switch (a & 0xfffff000)
	{
		case 0x40004000:
			if (off < 0x20)
				syscfg[off >> 2] = alias_apply(syscfg[off >> 2], v, alias);
			return;
		case 0x40008000:
			if (off < 0x78)
			{
				uint32_t idx = off / 12, reg = off % 12;
				if (reg == 0)
					clk_ctrl[idx] = alias_apply(clk_ctrl[idx], v, alias);
				else if (reg == 4)
					clk_div[idx] = alias_apply(clk_div[idx], v, alias);
				UpdateClocks();
			}
			return;
		case 0x4000c000:
			if (off == 0x0)
			{
				resets = alias_apply(resets, v, alias) & 0x01ffffff;
				// Blocks held in reset lose their state.
				if (resets & (1u << 2))
				{
					for (int i = 0; i < NUM_DMA; i++)
					{
						dma[i].busy = false;
						dma[i].ctrl = (uint32_t) i << 11;
					}
					dma_intr = 0;
				}
			}
			return;
		case 0x40010000:
			if (off == 0x0)
				psm_frce_on = alias_apply(psm_frce_on, v, alias);
			else if (off == 0x4)
			{
				uint32_t old = psm_frce_off;
				psm_frce_off = alias_apply(psm_frce_off, v, alias) & 0x1ffff;
				if ((old & 0x10000) && !(psm_frce_off & 0x10000))
				{
					// Core 1 out of reset: back to the boot ROM's launch wait.
					uint64_t t = core[1].cycles;
					ResetCore(1);
					core[1].cycles = t > now ? t : now;
					Core1LaunchStep();
				}
				else if (psm_frce_off & 0x10000)
				{
					core[1].halted = true;
					core[1].launch_step = -1;
					irq_check[1] = true;
				}
			}
			return;
		case 0x40014000:
		{
			if (off < 0xf0)
			{
				int pin = off >> 3;
				if (pin < NUM_GPIO && (off & 4))
				{
					gpio_ctrl[pin] = alias_apply(gpio_ctrl[pin], v, alias) & 0x3333f;
					RecalcGpioMux();
				}
				return;
			}
			if (off < 0x100)
			{
				int w = (off - 0xf0) >> 2;
				uint32_t clr = (alias == 3 || alias == 1) ? 0 : v;
				gpio_intr[w] &= ~(clr & 0xcccccccc);
				return;
			}
			if (off < 0x110) { int w = (off - 0x100) >> 2; gpio_inte[0][w] = alias_apply(gpio_inte[0][w], v, alias); return; }
			if (off < 0x120) { int w = (off - 0x110) >> 2; gpio_intf[0][w] = alias_apply(gpio_intf[0][w], v, alias); return; }
			if (off >= 0x130 && off < 0x140) { int w = (off - 0x130) >> 2; gpio_inte[1][w] = alias_apply(gpio_inte[1][w], v, alias); return; }
			if (off >= 0x140 && off < 0x150) { int w = (off - 0x140) >> 2; gpio_intf[1][w] = alias_apply(gpio_intf[1][w], v, alias); return; }
			return;
		}
		case 0x40018000:
			if (off < 0x30 && (off & 4))
			{
				qspi_ctrl[off >> 3] = alias_apply(qspi_ctrl[off >> 3], v, alias);
				if ((off >> 3) == 1)
				{
					uint32_t ov = (qspi_ctrl[1] >> 8) & 3;
					if (ov == 2)
						SpiSelect(true);
					else if (ov == 3)
						SpiSelect(false);
				}
			}
			return;
		case 0x4001c000:
			if (off < 0x80)
				pads[off >> 2] = alias_apply(pads[off >> 2], v, alias);
			return;
		case 0x40020000:
			if (off < 0x1c)
				qspi_pads[off >> 2] = alias_apply(qspi_pads[off >> 2], v, alias);
			return;
		case 0x40024000:
			if (off == 0x00) xosc_ctrl = alias_apply(xosc_ctrl, v, alias);
			else if (off == 0x0c) xosc_startup = alias_apply(xosc_startup, v, alias);
			return;
		case 0x40028000:
		case 0x4002c000:
		{
			uint32_t *pll = ((a & 0xfffff000) == 0x40028000) ? pll_sys : pll_usb;
			if (off < 0x10)
				pll[off >> 2] = alias_apply(pll[off >> 2], v, alias);
			UpdateClocks();
			return;
		}
		case 0x40030000:
			if (off == 0x0)
				busctrl_priority = alias_apply(busctrl_priority, v, alias);
			return;
		case 0x40034000:
		case 0x40038000:
		{
			int u = ((a & 0xfffff000) == 0x40038000);
			switch (off)
			{
				case 0x00:
				{
					char ch = (char) (v & 0xff);
					if (ch == '\n' || uart_line[u].size() > 200)
					{
						uart_line[u].push_back(0);
						Log("rp2040 uart%d: %s", u, &uart_line[u][0]);
						uart_line[u].clear();
					}
					else if (ch != '\r')
						uart_line[u].push_back(ch);
					return;
				}
				case 0x24: uart_ibrd[u] = v; return;
				case 0x28: uart_fbrd[u] = v; return;
				case 0x2c: uart_lcr[u] = alias_apply(uart_lcr[u], v, alias); return;
				case 0x30: uart_cr[u] = alias_apply(uart_cr[u], v, alias); return;
				case 0x38: uart_imsc[u] = alias_apply(uart_imsc[u], v, alias); return;
				default: return;
			}
		}
		case 0x4004c000:
			if (off == 0x00)
			{
				adc_cs = alias_apply(adc_cs, v, alias) & ~4u;
				if (v & 4)
				{
					uint32_t ainsel = (adc_cs >> 12) & 7;
					adc_result = (ainsel == 4) ? 0x36c : 0x7ff;
				}
			}
			else if (off == 0x08) adc_fcs = alias_apply(adc_fcs, v, alias);
			else if (off == 0x10) adc_div = alias_apply(adc_div, v, alias);
			return;
		case 0x40050000:
			if (off < 0xb4)
				pwm[off >> 2] = alias_apply(pwm[off >> 2], v, alias);
			return;
		case 0x40054000:
			AdvanceTimer();
			switch (off)
			{
				case 0x00:
					us_count = ((uint64_t) v << 32) | (uint32_t) us_count;
					return;
				case 0x04:
					us_count = (us_count & 0xffffffff00000000ull) | v;
					return;
				case 0x10: case 0x14: case 0x18: case 0x1c:
				{
					int i = (off - 0x10) >> 2;
					timer_alarm[i] = v;
					timer_armed |= 1u << i;
					return;
				}
				case 0x20:
					timer_armed &= ~(alias == 3 ? 0 : v);
					return;
				case 0x30: timer_pause = alias_apply(timer_pause, v, alias) & 1; return;
				case 0x34: timer_intr &= ~(alias == 3 ? 0 : v); return;
				case 0x38: timer_inte = alias_apply(timer_inte, v, alias) & 0xf; return;
				case 0x3c: timer_intf = alias_apply(timer_intf, v, alias) & 0xf; return;
				default: return;
			}
		case 0x40058000:
			if (off == 0x00)
			{
				watchdog_ctrl = alias_apply(watchdog_ctrl, v, alias);
				if (watchdog_ctrl & 0x80000000)
				{
					Log("rp2040: watchdog trigger");
					watchdog_ctrl &= ~0x80000000u;
					reset_request = true;
					watchdog_reason = 2;
				}
			}
			else if (off == 0x04) watchdog_load = v & 0xffffff;
			else if (off >= 0x0c && off < 0x2c) watchdog_scratch[(off - 0x0c) >> 2] = alias_apply(watchdog_scratch[(off - 0x0c) >> 2], v, alias);
			else if (off == 0x2c) watchdog_tick = alias_apply(watchdog_tick, v, alias) & 0x3ff;
			return;
		case 0x40060000:
			if (off == 0x00) rosc_ctrl = alias_apply(rosc_ctrl, v, alias);
			return;
		case 0x40064000:
			if (off == 0x0) vreg = alias_apply(vreg, v, alias) & 0xf3;
			else if (off == 0x4) bod = alias_apply(bod, v, alias);
			else if (off == 0x8) chip_reset = alias_apply(chip_reset, v, alias);
			return;
		default:
			return;
	}
}

// ---------------------------------------------------------------------------
// DMA. Transfers complete as soon as their DREQ allows; nothing on the other
// side of the bus can observe a DMA in flight any sooner.

bool Chip::DmaDreq (int treq)
{
	if (treq == 0x3f)
		return true;
	if (treq < 16)
	{
		int p = treq >> 3, sm = treq & 3;
		PioSm &s = pio[p].sm[sm];
		uint32_t sc = s.shiftctrl;
		if (treq & 4)
			return s.rx_level > 0;
		uint32_t depth = (sc & (1u << 30)) ? 8 : ((sc & (1u << 31)) ? 0 : 4);
		return s.tx_level < depth;
	}
	if (treq == 37)
		return xip_stream_ctr != 0;
	if (treq >= 0x3b && treq <= 0x3e)
		return true;
	// UART/SPI/PWM/ADC/I2C: always ready
	return true;
}

void Chip::DmaTrigger (int ch)
{
	DmaChannel &d = dma[ch];
	if (!(d.ctrl & 1))
		return;
	if (!d.busy)
	{
		d.busy = true;
		d.trans_count = d.reload;
	}
	if (!dma_in_service)
		ServiceDma();
}

void Chip::DmaComplete (int ch)
{
	DmaChannel &d = dma[ch];
	d.busy = false;
	if (!(d.ctrl & (1u << 21)))
		dma_intr |= 1u << ch;
	irq_dirty = true;
	int chain = (d.ctrl >> 11) & 15;
	if (chain != ch)
		DmaTrigger(chain);
}

void Chip::ServiceDma ()
{
	if (dma_in_service)
		return;
	dma_in_service = true;
	bool progress = true;
	int guard = 0;
	while (progress && guard++ < 64)
	{
		progress = false;
		for (int ch = 0; ch < NUM_DMA; ch++)
		{
			DmaChannel &d = dma[ch];
			if (!d.busy || !(d.ctrl & 1))
				continue;
			int treq = (d.ctrl >> 15) & 0x3f;
			uint32_t size = 1u << ((d.ctrl >> 2) & 3);
			uint32_t ring = (d.ctrl >> 6) & 15;
			uint32_t ring_mask = ring ? ((1u << ring) - 1) : 0;
			bool ring_write = (d.ctrl & (1u << 10)) != 0;
			while (d.trans_count && DmaDreq(treq))
			{
				uint32_t v;
				if (size == 4)
					v = Read32(0, d.read_addr);
				else if (size == 2)
					v = Read16(0, d.read_addr);
				else
					v = Read8(0, d.read_addr);
				if (d.ctrl & (1u << 22))
				{
					if (size == 4)
						v = bswap32(v);
					else if (size == 2)
						v = ((v & 0xff) << 8) | ((v >> 8) & 0xff);
				}
				if ((d.ctrl & (1u << 23)) && (dma_sniff_ctrl & 1) && (int) ((dma_sniff_ctrl >> 1) & 15) == ch)
				{
					uint32_t calc = (dma_sniff_ctrl >> 5) & 15;
					if (calc == 0xf)
						dma_sniff_data += v;
				}
				if (size == 4)
					Write32(0, d.write_addr, v);
				else if (size == 2)
					Write16(0, d.write_addr, (uint16_t) v);
				else
					Write8(0, d.write_addr, (uint8_t) v);
				if (d.ctrl & (1u << 4))
				{
					uint32_t na = d.read_addr + size;
					d.read_addr = (ring && !ring_write) ? ((d.read_addr & ~ring_mask) | (na & ring_mask)) : na;
				}
				if (d.ctrl & (1u << 5))
				{
					uint32_t na = d.write_addr + size;
					d.write_addr = (ring && ring_write) ? ((d.write_addr & ~ring_mask) | (na & ring_mask)) : na;
				}
				d.trans_count--;
				progress = true;
				if (!d.busy)
					break;
			}
			if (d.busy && !d.trans_count)
			{
				DmaComplete(ch);
				progress = true;
			}
		}
	}
	dma_in_service = false;
}

uint32_t Chip::DmaRead (uint32_t off)
{
	if (off < 0x300)
	{
		DmaChannel &d = dma[off >> 6];
		switch (off & 0x3f)
		{
			case 0x00: case 0x14: case 0x28: case 0x3c: return d.read_addr;
			case 0x04: case 0x18: case 0x2c: case 0x34: return d.write_addr;
			case 0x08: case 0x1c: case 0x24: case 0x38: return d.trans_count;
			default: return d.ctrl | (d.busy ? (1u << 24) : 0);
		}
	}
	switch (off)
	{
		case 0x400: return dma_intr;
		case 0x404: return dma_inte[0];
		case 0x408: return dma_intf[0];
		case 0x40c: return (dma_intr & dma_inte[0]) | dma_intf[0];
		case 0x414: return dma_inte[1];
		case 0x418: return dma_intf[1];
		case 0x41c: return (dma_intr & dma_inte[1]) | dma_intf[1];
		case 0x420: case 0x424: case 0x428: case 0x42c: return dma_timer[(off - 0x420) >> 2];
		case 0x434: return dma_sniff_ctrl;
		case 0x438: return dma_sniff_data;
		case 0x448: return NUM_DMA;
		default:
			if (off >= 0x800 && off < 0xb00)
			{
				DmaChannel &d = dma[(off - 0x800) >> 6];
				if ((off & 0x3f) == 4)
					return d.reload;
			}
			return 0;
	}
}

void Chip::DmaWrite (uint32_t off, uint32_t v, int alias)
{
	if (off < 0x300)
	{
		int ch = off >> 6;
		DmaChannel &d = dma[ch];
		uint32_t reg = off & 0x3f;
		bool trig = false;
		switch (reg)
		{
			case 0x00: case 0x14: case 0x28: case 0x3c:
				d.read_addr = alias_apply(d.read_addr, v, alias);
				trig = (reg == 0x3c);
				break;
			case 0x04: case 0x18: case 0x2c: case 0x34:
				d.write_addr = alias_apply(d.write_addr, v, alias);
				trig = (reg == 0x2c);
				break;
			case 0x08: case 0x1c: case 0x24: case 0x38:
				d.reload = alias_apply(d.reload, v, alias);
				trig = (reg == 0x1c);
				break;
			default:
			{
				uint32_t nv = alias_apply(d.ctrl, v, alias);
				d.ctrl = (d.ctrl & 0xe1000000) | (nv & 0x00ffffff);
				if (nv & 0xe0000000)
					d.ctrl &= ~(nv & 0x60000000);
				trig = (reg == 0x0c);
				break;
			}
		}
		if (trig)
		{
			if (v == 0 && reg != 0x0c)
			{
				// Null trigger: only raises the IRQ of a quiet channel.
				if (d.ctrl & (1u << 21))
					dma_intr |= 1u << ch;
			}
			else
				DmaTrigger(ch);
		}
		return;
	}
	switch (off)
	{
		case 0x400: dma_intr &= ~v; break;
		case 0x404: dma_inte[0] = alias_apply(dma_inte[0], v, alias); break;
		case 0x408: dma_intf[0] = alias_apply(dma_intf[0], v, alias); break;
		case 0x40c: dma_intr &= ~v; break;
		case 0x414: dma_inte[1] = alias_apply(dma_inte[1], v, alias); break;
		case 0x418: dma_intf[1] = alias_apply(dma_intf[1], v, alias); break;
		case 0x41c: dma_intr &= ~v; break;
		case 0x420: case 0x424: case 0x428: case 0x42c: dma_timer[(off - 0x420) >> 2] = v; break;
		case 0x430:
			for (int ch = 0; ch < NUM_DMA; ch++)
				if (v & (1u << ch))
					DmaTrigger(ch);
			break;
		case 0x434: dma_sniff_ctrl = alias_apply(dma_sniff_ctrl, v, alias); break;
		case 0x438: dma_sniff_data = v; break;
		case 0x444:
			for (int ch = 0; ch < NUM_DMA; ch++)
				if (v & (1u << ch))
					dma[ch].busy = false;
			break;
		default: break;
	}
}

// ---------------------------------------------------------------------------
// PIO. State machines only advance when StepPio() is called (after the
// outside world changes a pin) or when the CPU pokes them; each runs until
// it stalls. Delay cycles and clock dividers are not modelled.

static inline uint32_t sm_tx_depth (const PioSm &s)
{
	if (s.shiftctrl & (1u << 30)) return 8;
	if (s.shiftctrl & (1u << 31)) return 0;
	return 4;
}

static inline uint32_t sm_rx_depth (const PioSm &s)
{
	if (s.shiftctrl & (1u << 31)) return 8;
	if (s.shiftctrl & (1u << 30)) return 0;
	return 4;
}

bool Chip::PioTxPush (int p, int sm, uint32_t v)
{
	PioSm &s = pio[p].sm[sm];
	if (s.tx_level >= sm_tx_depth(s))
	{
		pio[p].fdebug |= 1u << (16 + sm);
		return false;
	}
	s.tx[(s.tx_head + s.tx_level) & 7] = v;
	s.tx_level++;
	return true;
}

bool Chip::PioTxPop (int p, int sm, uint32_t &v)
{
	PioSm &s = pio[p].sm[sm];
	if (!s.tx_level)
		return false;
	v = s.tx[s.tx_head];
	s.tx_head = (s.tx_head + 1) & 7;
	s.tx_level--;
	return true;
}

bool Chip::PioRxPush (int p, int sm, uint32_t v)
{
	PioSm &s = pio[p].sm[sm];
	if (s.rx_level >= sm_rx_depth(s))
		return false;
	s.rx[(s.rx_head + s.rx_level) & 7] = v;
	s.rx_level++;
	return true;
}

bool Chip::PioRxPop (int p, int sm, uint32_t &v)
{
	PioSm &s = pio[p].sm[sm];
	if (!s.rx_level)
	{
		pio[p].fdebug |= 1u << (8 + sm);
		return false;
	}
	v = s.rx[s.rx_head];
	s.rx_head = (s.rx_head + 1) & 7;
	s.rx_level--;
	return true;
}

void Chip::PioSetPins (int p, uint32_t base, uint32_t count, uint32_t v, bool dirs)
{
	uint32_t &target = dirs ? pio[p].pin_oe : pio[p].pin_out;
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t pin = (base + i) & 31;
		target = (target & ~(1u << pin)) | (((v >> i) & 1) << pin);
	}
}

uint32_t Chip::PioFstat (int p)
{
	uint32_t st = 0;
	for (int i = 0; i < 4; i++)
	{
		PioSm &s = pio[p].sm[i];
		if (s.rx_level >= sm_rx_depth(s)) st |= 1u << i;
		if (!s.rx_level) st |= 1u << (8 + i);
		if (s.tx_level >= sm_tx_depth(s)) st |= 1u << (16 + i);
		if (!s.tx_level) st |= 1u << (24 + i);
	}
	return st;
}

uint32_t Chip::PioIntr (int p)
{
	uint32_t st = PioFstat(p);
	uint32_t intr = 0;
	for (int i = 0; i < 4; i++)
	{
		if (!(st & (1u << (8 + i)))) intr |= 1u << i;
		if (!(st & (1u << (16 + i)))) intr |= 1u << (4 + i);
	}
	intr |= (pio[p].irq & 0xf) << 8;
	return intr;
}

void Chip::PioRestartSm (int p, int sm)
{
	PioSm &s = pio[p].sm[sm];
	s.isr = 0;
	s.isr_count = 0;
	s.osr_count = 32;
	s.exec_pending = false;
	s.stalled = false;
}

// Execute one instruction. Returns false if it stalled.
void Chip::PioExec (int p, int smi, uint16_t ins, bool is_exec)
{
	Pio &pi = pio[p];
	PioSm &s = pi.sm[smi];
	uint32_t exec = s.execctrl, shift = s.shiftctrl, pinctrl = s.pinctrl;
	uint32_t wrap_top = (exec >> 12) & 31, wrap_bottom = (exec >> 7) & 31;

	// side-set
	uint32_t ss_count = (pinctrl >> 29) & 7;
	uint32_t ds = (ins >> 8) & 31;
	bool side_en = (exec & (1u << 30)) != 0;
	uint32_t delay_bits = 5 - ss_count;
	if (ss_count)
	{
		uint32_t data = ds >> delay_bits;
		uint32_t bits = ss_count;
		bool apply = true;
		if (side_en)
		{
			apply = (data >> (ss_count - 1)) & 1;
			bits = ss_count - 1;
			data &= (1u << bits) - 1;
		}
		if (apply && bits)
			PioSetPins(p, (pinctrl >> 10) & 31, bits, data, (exec & (1u << 29)) != 0);
	}

	uint32_t op = ins >> 13, a1 = (ins >> 5) & 7, a2 = ins & 31;
	bool advance = true, stall = false;
	uint32_t pull_thresh = ((shift >> 25) & 31) ? ((shift >> 25) & 31) : 32;
	uint32_t push_thresh = ((shift >> 20) & 31) ? ((shift >> 20) & 31) : 32;
	uint32_t pins = GpioOutputs();

	switch (op)
	{
		case 0:	// JMP
		{
			bool take;
			switch (a1)
			{
				case 0: take = true; break;
				case 1: take = (s.x == 0); break;
				case 2: take = (s.x != 0); s.x--; break;
				case 3: take = (s.y == 0); break;
				case 4: take = (s.y != 0); s.y--; break;
				case 5: take = (s.x != s.y); break;
				case 6: take = (pins >> ((exec >> 24) & 31)) & 1; break;
				default: take = (s.osr_count < pull_thresh); break;
			}
			if (take)
			{
				s.pc = a2;
				advance = false;
			}
			break;
		}
		case 1:	// WAIT
		{
			uint32_t pol = (ins >> 7) & 1, src = (ins >> 5) & 3;
			bool ok;
			if (src == 0)
				ok = ((pins >> a2) & 1) == pol;
			else if (src == 1)
				ok = ((pins >> ((((pinctrl >> 15) & 31) + a2) & 31)) & 1) == pol;
			else if (src == 2)
			{
				uint32_t idx = (a2 & 0x10) ? ((a2 & 0x8) | ((a2 + smi) & 3)) : (a2 & 7);
				ok = ((pi.irq >> idx) & 1) == pol;
				if (ok && pol)
					pi.irq &= ~(1u << idx);
			}
			else
				ok = true;
			if (!ok)
				stall = true;
			break;
		}
		case 2:	// IN
		{
			uint32_t n = a2 ? a2 : 32, data;
			if (s.isr_count >= push_thresh && (shift & (1u << 16)))
			{
				if (!PioRxPush(p, smi, s.isr))
				{
					stall = true;
					break;
				}
				s.isr = 0;
				s.isr_count = 0;
			}
			switch (a1)
			{
				case 0:
				{
					uint32_t base = (pinctrl >> 15) & 31;
					data = base ? ((pins >> base) | (pins << (32 - base))) : pins;
					break;
				}
				case 1: data = s.x; break;
				case 2: data = s.y; break;
				case 6: data = s.isr; break;
				case 7: data = s.osr; break;
				default: data = 0; break;
			}
			if (n < 32)
				data &= (1u << n) - 1;
			if (shift & (1u << 18))
				s.isr = (n == 32) ? data : ((s.isr >> n) | (data << (32 - n)));
			else
				s.isr = (n == 32) ? data : ((s.isr << n) | data);
			s.isr_count += n;
			if (s.isr_count > 32)
				s.isr_count = 32;
			if ((shift & (1u << 16)) && s.isr_count >= push_thresh)
			{
				if (PioRxPush(p, smi, s.isr))
				{
					s.isr = 0;
					s.isr_count = 0;
				}
			}
			break;
		}
		case 3:	// OUT
		{
			uint32_t n = a2 ? a2 : 32, data;
			if ((shift & (1u << 17)) && s.osr_count >= pull_thresh)
			{
				uint32_t v;
				if (!PioTxPop(p, smi, v))
				{
					stall = true;
					break;
				}
				s.osr = v;
				s.osr_count = 0;
			}
			if (shift & (1u << 19))
			{
				data = (n == 32) ? s.osr : (s.osr & ((1u << n) - 1));
				s.osr = (n == 32) ? 0 : (s.osr >> n);
			}
			else
			{
				data = (n == 32) ? s.osr : (s.osr >> (32 - n));
				s.osr = (n == 32) ? 0 : (s.osr << n);
			}
			s.osr_count += n;
			if (s.osr_count > 32)
				s.osr_count = 32;
			switch (a1)
			{
				case 0: PioSetPins(p, pinctrl & 31, (pinctrl >> 20) & 63, data, false); break;
				case 1: s.x = data; break;
				case 2: s.y = data; break;
				case 3: break;
				case 4: PioSetPins(p, pinctrl & 31, (pinctrl >> 20) & 63, data, true); break;
				case 5: s.pc = data & 31; advance = false; break;
				case 6: s.isr = data; s.isr_count = n; break;
				case 7: s.exec_pending = true; s.exec_instr = (uint16_t) data; break;
			}
			break;
		}
		case 4:	// PUSH / PULL
			if (ins & 0x80)
			{
				bool ifempty = (ins >> 6) & 1, block = (ins >> 5) & 1;
				if (ifempty && s.osr_count < pull_thresh)
					break;
				uint32_t v;
				if (PioTxPop(p, smi, v))
				{
					s.osr = v;
					s.osr_count = 0;
				}
				else if (block)
					stall = true;
				else
				{
					s.osr = s.x;
					s.osr_count = 0;
				}
			}
			else
			{
				bool iffull = (ins >> 6) & 1, block = (ins >> 5) & 1;
				if (iffull && s.isr_count < push_thresh)
					break;
				if (PioRxPush(p, smi, s.isr))
				{
					s.isr = 0;
					s.isr_count = 0;
				}
				else if (block)
					stall = true;
				else
				{
					s.isr = 0;
					s.isr_count = 0;
				}
			}
			break;
		case 5:	// MOV
		{
			uint32_t src;
			switch (ins & 7)
			{
				case 0:
				{
					uint32_t base = (pinctrl >> 15) & 31;
					src = base ? ((pins >> base) | (pins << (32 - base))) : pins;
					break;
				}
				case 1: src = s.x; break;
				case 2: src = s.y; break;
				case 5:
				{
					uint32_t n = exec & 15;
					bool rx = (exec >> 4) & 1;
					src = ((rx ? s.rx_level : s.tx_level) < n) ? 0xffffffffu : 0;
					break;
				}
				case 6: src = s.isr; break;
				case 7: src = s.osr; break;
				default: src = 0; break;
			}
			switch ((ins >> 3) & 3)
			{
				case 1: src = ~src; break;
				case 2:
				{
					uint32_t v = src, rv = 0;
					for (int i = 0; i < 32; i++)
						rv |= ((v >> i) & 1) << (31 - i);
					src = rv;
					break;
				}
			}
			switch (a1)
			{
				case 0: PioSetPins(p, pinctrl & 31, (pinctrl >> 20) & 63, src, false); break;
				case 1: s.x = src; break;
				case 2: s.y = src; break;
				case 4: s.exec_pending = true; s.exec_instr = (uint16_t) src; break;
				case 5: s.pc = src & 31; advance = false; break;
				case 6: s.isr = src; s.isr_count = 0; break;
				case 7: s.osr = src; s.osr_count = 0; break;
				default: break;
			}
			break;
		}
		case 6:	// IRQ
		{
			uint32_t idx = (a2 & 0x10) ? ((a2 & 0x8) | ((a2 + smi) & 3)) : (a2 & 7);
			if (ins & 0x40)
				pi.irq &= ~(1u << idx);
			else
			{
				if (!s.stalled)
					pi.irq |= 1u << idx;
				if ((ins & 0x20) && (pi.irq & (1u << idx)))
					stall = true;
			}
			break;
		}
		case 7:	// SET
			switch (a1)
			{
				case 0: PioSetPins(p, (pinctrl >> 5) & 31, (pinctrl >> 26) & 7, a2, false); break;
				case 1: s.x = a2; break;
				case 2: s.y = a2; break;
				case 4: PioSetPins(p, (pinctrl >> 5) & 31, (pinctrl >> 26) & 7, a2, true); break;
				default: break;
			}
			break;
	}

	s.stalled = stall;
	if (stall)
		return;
	if (advance && !is_exec)
		s.pc = (s.pc == wrap_top) ? wrap_bottom : ((s.pc + 1) & 31);
}

void Chip::PioRunSm (int p, int smi, int budget)
{
	PioSm &s = pio[p].sm[smi];
	while (budget-- > 0)
	{
		if (s.exec_pending)
		{
			uint16_t ins = (uint16_t) s.exec_instr;
			s.exec_pending = false;
			PioExec(p, smi, ins, true);
			if (s.stalled)
			{
				s.exec_pending = true;
				s.exec_instr = ins;
				return;
			}
			continue;
		}
		PioExec(p, smi, pio[p].instr[s.pc & 31], false);
		if (s.stalled)
			return;
	}
}

void Chip::StepPio ()
{
	for (int pass = 0; pass < 2; pass++)
		for (int p = 0; p < 2; p++)
			for (int i = 0; i < 4; i++)
				if (pio[p].ctrl & (1u << i))
					PioRunSm(p, i, 24);
	ServiceDma();
}

uint32_t Chip::PioRead (int p, uint32_t off)
{
	Pio &pi = pio[p];
	switch (off)
	{
		case 0x000: return pi.ctrl & 0xf;
		case 0x004: return PioFstat(p);
		case 0x008: return pi.fdebug;
		case 0x00c:
		{
			uint32_t v = 0;
			for (int i = 0; i < 4; i++)
				v |= (pi.sm[i].tx_level & 15) << (i * 8) | (pi.sm[i].rx_level & 15) << (i * 8 + 4);
			return v;
		}
		case 0x020: case 0x024: case 0x028: case 0x02c:
		{
			uint32_t v = 0;
			PioRxPop(p, (off - 0x20) >> 2, v);
			ServiceDma();
			return v;
		}
		case 0x030: return pi.irq;
		case 0x038: return pi.input_sync_bypass;
		case 0x03c: return pi.pin_out;
		case 0x040: return pi.pin_oe;
		case 0x044: return 0x00200404;
		case 0x128: return PioIntr(p);
		case 0x12c: return pi.irq_inte[0];
		case 0x130: return pi.irq_intf[0];
		case 0x134: return (PioIntr(p) & pi.irq_inte[0]) | pi.irq_intf[0];
		case 0x138: return pi.irq_inte[1];
		case 0x13c: return pi.irq_intf[1];
		case 0x140: return (PioIntr(p) & pi.irq_inte[1]) | pi.irq_intf[1];
		default:
			if (off >= 0x0c8 && off < 0x128)
			{
				int sm = (off - 0x0c8) / 0x18;
				PioSm &s = pi.sm[sm];
				switch ((off - 0x0c8) % 0x18)
				{
					case 0x00: return s.clkdiv;
					case 0x04: return s.execctrl | ((s.exec_pending && s.stalled) ? 0x80000000u : 0);
					case 0x08: return s.shiftctrl;
					case 0x0c: return s.pc;
					case 0x10: return s.exec_pending ? s.exec_instr : pi.instr[s.pc & 31];
					case 0x14: return s.pinctrl;
				}
			}
			return 0;
	}
}

void Chip::PioWrite (int p, uint32_t off, uint32_t v, int alias)
{
	Pio &pi = pio[p];
	switch (off)
	{
		case 0x000:
		{
			uint32_t nv = alias_apply(pi.ctrl, v, alias);
			for (int i = 0; i < 4; i++)
				if (nv & (1u << (4 + i)))
					PioRestartSm(p, i);
			pi.ctrl = nv & 0xf;
			StepPio();
			return;
		}
		case 0x008: pi.fdebug &= ~v; return;
		case 0x010: case 0x014: case 0x018: case 0x01c:
			PioTxPush(p, (off - 0x10) >> 2, v);
			return;
		case 0x030: pi.irq &= ~(v & 0xff); StepPio(); return;
		case 0x034: pi.irq |= v & 0xff; StepPio(); return;
		case 0x038: pi.input_sync_bypass = alias_apply(pi.input_sync_bypass, v, alias); return;
		case 0x12c: pi.irq_inte[0] = alias_apply(pi.irq_inte[0], v, alias) & 0xfff; return;
		case 0x130: pi.irq_intf[0] = alias_apply(pi.irq_intf[0], v, alias) & 0xfff; return;
		case 0x138: pi.irq_inte[1] = alias_apply(pi.irq_inte[1], v, alias) & 0xfff; return;
		case 0x13c: pi.irq_intf[1] = alias_apply(pi.irq_intf[1], v, alias) & 0xfff; return;
		default:
			if (off >= 0x048 && off < 0x0c8)
			{
				pi.instr[(off - 0x48) >> 2] = (uint16_t) v;
				return;
			}
			if (off >= 0x0c8 && off < 0x128)
			{
				int sm = (off - 0x0c8) / 0x18;
				PioSm &s = pi.sm[sm];
				switch ((off - 0x0c8) % 0x18)
				{
					case 0x00: s.clkdiv = alias_apply(s.clkdiv, v, alias); break;
					case 0x04: s.execctrl = alias_apply(s.execctrl, v, alias) & 0x7fffffff; break;
					case 0x08:
					{
						uint32_t nv = alias_apply(s.shiftctrl, v, alias);
						if ((nv ^ s.shiftctrl) & 0xc0000000)
						{
							s.tx_level = s.rx_level = 0;
							s.tx_head = s.rx_head = 0;
						}
						s.shiftctrl = nv;
						break;
					}
					case 0x10:
						s.exec_pending = false;
						PioExec(p, sm, (uint16_t) v, true);
						if (s.stalled)
						{
							s.exec_pending = true;
							s.exec_instr = (uint16_t) v;
						}
						else if (s.exec_pending)
							PioRunSm(p, sm, 1);
						break;
					case 0x14: s.pinctrl = alias_apply(s.pinctrl, v, alias); break;
				}
			}
			return;
	}
}

// ---------------------------------------------------------------------------
// SSI and the W25Q128 behind it, for code that talks to the flash directly.

void Chip::SpiSelect (bool sel)
{
	if (sel == spi_selected)
		return;
	if (!sel)
	{
		if (spi_cmd == 0x06)
			spi_wel = true;
		else if (spi_cmd == 0x04)
			spi_wel = false;
		else if (spi_cmd == 0x02 || spi_cmd == 0x20 || spi_cmd == 0x52 || spi_cmd == 0xd8)
			spi_wel = false;
	}
	spi_selected = sel;
	spi_cmd = 0;
	spi_count = 0;
	spi_addr = 0;
}

uint8_t Chip::SpiXfer (uint8_t out)
{
	if (!spi_selected)
		return 0xff;
	uint32_t i = spi_count++;
	if (i == 0)
	{
		spi_cmd = out;
		return 0xff;
	}
	switch (spi_cmd)
	{
		case 0x9f:
		{
			static const uint8_t id[3] = { 0xef, 0x40, 0x18 };
			return i <= 3 ? id[i - 1] : 0xff;
		}
		case 0x4b:
			if (i < 5)
				return 0xff;
			return (uint8_t) (0xe6 + (i - 5) * 0x11);
		case 0x05: return spi_wel ? 0x02 : 0x00;
		case 0x35: return 0x02;
		case 0x03:
		case 0x0b:
			if (i <= 3)
			{
				spi_addr = (spi_addr << 8) | out;
				return 0xff;
			}
			if (spi_cmd == 0x0b && i == 4)
				return 0xff;
			return flash[(spi_addr++) & flash_mask];
		case 0x02:
			if (i <= 3)
			{
				spi_addr = (spi_addr << 8) | out;
				return 0xff;
			}
			if (spi_wel)
			{
				flash[((spi_addr & ~0xffu) | ((spi_addr + (i - 4)) & 0xff)) & flash_mask] &= out;
				flash_dirty = true;
			}
			return 0xff;
		case 0x20:
		case 0x52:
		case 0xd8:
			if (i <= 3)
			{
				spi_addr = (spi_addr << 8) | out;
				if (i == 3 && spi_wel)
				{
					uint32_t sz = spi_cmd == 0x20 ? 0x1000 : (spi_cmd == 0x52 ? 0x8000 : 0x10000);
					uint32_t base = spi_addr & ~(sz - 1) & flash_mask;
					memset(&flash[base], 0xff, sz);
					flash_dirty = true;
				}
			}
			return 0xff;
		default:
			return 0xff;
	}
}

uint32_t Chip::SsiRead (uint32_t off)
{
	switch (off)
	{
		case 0x20: return 0;
		case 0x24: return (uint32_t) ssi_rx.size();
		case 0x28:
			ssi_idle_seen = true;
			return 0x06 | (ssi_rx.empty() ? 0 : 0x08);
		case 0x58: return 0x3430322a;
		case 0x5c: return 0x51535049;
		case 0x60:
		{
			if (ssi_rx.empty())
				return 0;
			uint32_t v = ssi_rx[0];
			ssi_rx.erase(ssi_rx.begin());
			return v;
		}
		default:
			return off < 0x100 ? ssi_regs[off >> 2] : 0;
	}
}

void Chip::SsiWrite (uint32_t off, uint32_t v)
{
	if (off < 0x100)
		ssi_regs[off >> 2] = v;
	if (off == 0x60)
	{
		// Auto chip select drops whenever the TX FIFO drains; the firmware
		// always polls SR for that before its next command.
		uint32_t ov = (qspi_ctrl[1] >> 8) & 3;
		if (ov == 0)
		{
			if (ssi_idle_seen)
				SpiSelect(false);
			SpiSelect((ssi_regs[0x08 >> 2] & 1) && ssi_regs[0x10 >> 2]);
		}
		ssi_idle_seen = false;
		uint8_t in = SpiXfer((uint8_t) v);
		if (ssi_rx.size() < 16)
			ssi_rx.push_back(in);
	}
	else if (off == 0x08 && !(v & 1))
	{
		ssi_rx.clear();
		if (((qspi_ctrl[1] >> 8) & 3) == 0)
			SpiSelect(false);
	}
}

// ---------------------------------------------------------------------------
// Boot ROM routines, natively

static inline float f_of (uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t u_of (float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double d_of (uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t u_of (double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

// The ROM float library flushes denormals to zero and treats NaN as infinity.
static inline uint32_t f_in (uint32_t v)
{
	uint32_t e = (v >> 23) & 0xff;
	if (e == 0)
		return v & 0x80000000u;
	if (e == 0xff)
		return (v & 0x80000000u) | 0x7f800000u;
	return v;
}

static inline uint32_t f_out (float f)
{
	uint32_t v = u_of(f);
	uint32_t e = (v >> 23) & 0xff;
	if (e == 0)
		return v & 0x80000000u;
	if (e == 0xff)
		return (v & 0x80000000u) | 0x7f800000u;
	return v;
}

// Invalid arithmetic gives infinity rather than NaN: positive for add and
// subtract, the product of the operand signs for multiply and divide.
static inline uint32_t f_arith (float f, uint32_t sign = 0)
{
	return (f != f) ? (0x7f800000u | (sign & 0x80000000u)) : f_out(f);
}

static inline uint64_t d_in (uint64_t v)
{
	uint32_t e = (uint32_t) (v >> 52) & 0x7ff;
	if (e == 0)
		return v & 0x8000000000000000ull;
	if (e == 0x7ff)
		return (v & 0x8000000000000000ull) | 0x7ff0000000000000ull;
	return v;
}

static inline uint64_t d_out (double d)
{
	return d_in(u_of(d));
}

static inline uint64_t d_arith (double d, uint64_t sign = 0)
{
	return (d != d) ? (0x7ff0000000000000ull | (sign & 0x8000000000000000ull)) : d_out(d);
}

static int32_t fcmp_raw (uint32_t a, uint32_t b)
{
	if ((int32_t) (a ^ b) < 0)
	{
		if (((a | b) << 1) == 0)
			return 0;
		return (int32_t) a >= 0 ? 1 : -1;
	}
	int32_t s = ((int32_t) a < 0) ? -1 : 1;
	if ((int32_t) a > (int32_t) b)
		return s;
	if ((int32_t) a < (int32_t) b)
		return -s;
	return 0;
}

static int32_t dcmp_raw (uint64_t a, uint64_t b)
{
	if ((int64_t) (a ^ b) < 0)
	{
		if (((a | b) << 1) == 0)
			return 0;
		return (int64_t) a >= 0 ? 1 : -1;
	}
	int32_t s = ((int64_t) a < 0) ? -1 : 1;
	if ((int64_t) a > (int64_t) b)
		return s;
	if ((int64_t) a < (int64_t) b)
		return -s;
	return 0;
}

// float -> fixed with n fraction bits, rounding towards -infinity, clamped.
static int64_t to_fix (double x, int n, int64_t lo, int64_t hi)
{
	double v = floor(ldexp(x, n));
	if (!(v >= (double) lo))
		return (x != x) ? hi : lo;
	if (v >= (double) hi)
		return hi;
	return (int64_t) v;
}

static uint64_t to_ufix (double x, int n, uint64_t hi)
{
	double v = floor(ldexp(x, n));
	if (!(v > 0))
		return 0;
	if (v >= (double) hi)
		return hi;
	return (uint64_t) v;
}

void Chip::Hle (int n, uint32_t id)
{
	Core &c = core[n];
	uint32_t *r = c.r;
	hle_calls[id & 0xff]++;
	c.store_hash += 0x51ed270bu * (id + 1);	// a ROM call's effects are not hashed

	if (id >= HLE_SF && id < HLE_SF + 32)
	{
		uint32_t a = f_in(r[0]), b = f_in(r[1]);
		float fa = f_of(a), fb = f_of(b);
		c.cycles += 40;
		switch ((id - HLE_SF) * 4)
		{
			case 0x00: r[0] = f_arith(fa + fb); break;
			case 0x04: r[0] = f_arith(fa - fb); break;
			case 0x08: r[0] = f_arith(fa * fb, a ^ b); break;
			case 0x0c: r[0] = f_arith(fa / fb, a ^ b); break;
			case 0x10:
			case 0x14:
			{
				int32_t res = fcmp_raw(r[0], r[1]);
				r[0] = (uint32_t) res;
				c.n = res < 0; c.z = res == 0; c.c = 1; c.v = 0;
				break;
			}
			case 0x54:
			{
				uint32_t x = r[0], y = r[1];
				uint32_t ex = (x << 1) >> 24, ey = (y << 1) >> 24;
				if (ex == 0 || ex == 0xff) x = (x >> 23) << 23;
				if (ey == 0 || ey == 0xff) y = (y >> 23) << 23;
				int32_t res = fcmp_raw(x, y);
				r[0] = (uint32_t) res;
				c.n = res < 0; c.z = res == 0; c.c = 1; c.v = 0;
				break;
			}
			case 0x18: r[0] = f_out(sqrtf(fa)); break;
			case 0x1c: r[0] = (uint32_t) (int32_t) to_fix(fa, 0, INT32_MIN, INT32_MAX); break;
			case 0x20: r[0] = (uint32_t) (int32_t) to_fix(fa, (int32_t) r[1], INT32_MIN, INT32_MAX); break;
			case 0x24: r[0] = (uint32_t) to_ufix(fa, 0, 0xffffffffu); break;
			case 0x28: r[0] = (uint32_t) to_ufix(fa, (int32_t) r[1], 0xffffffffu); break;
			case 0x2c: r[0] = f_out((float) (int32_t) r[0]); break;
			case 0x30: r[0] = f_out((float) ldexp((double) (int32_t) r[0], -(int32_t) r[1])); break;
			case 0x34: r[0] = f_out((float) r[0]); break;
			case 0x38: r[0] = f_out((float) ldexp((double) r[0], -(int32_t) r[1])); break;
			case 0x3c: r[0] = f_out(cosf(fa)); break;
			case 0x40: r[0] = f_out(sinf(fa)); break;
			case 0x44: r[0] = f_out(tanf(fa)); break;
			case 0x48: r[1] = f_out(cosf(fa)); r[0] = f_out(sinf(fa)); break;
			case 0x4c: r[0] = f_out(expf(fa)); break;
			case 0x50: r[0] = f_out(logf(fa)); break;
			case 0x58: r[0] = f_out(atan2f(fa, fb)); break;
			case 0x5c: r[0] = f_out((float) (int64_t) (((uint64_t) r[1] << 32) | r[0])); break;
			case 0x60: r[0] = f_out((float) ldexp((double) (int64_t) (((uint64_t) r[1] << 32) | r[0]), -(int32_t) r[2])); break;
			case 0x64: r[0] = f_out((float) (((uint64_t) r[1] << 32) | r[0])); break;
			case 0x68: r[0] = f_out((float) ldexp((double) (((uint64_t) r[1] << 32) | r[0]), -(int32_t) r[2])); break;
			case 0x6c: case 0x70:
			{
				int sh = ((id - HLE_SF) * 4 == 0x70) ? (int32_t) r[1] : 0;
				uint64_t v = (uint64_t) to_fix(fa, sh, INT64_MIN, INT64_MAX);
				r[0] = (uint32_t) v;
				r[1] = (uint32_t) (v >> 32);
				break;
			}
			case 0x74: case 0x78:
			{
				int sh = ((id - HLE_SF) * 4 == 0x78) ? (int32_t) r[1] : 0;
				uint64_t v = to_ufix(fa, sh, ~0ull);
				r[0] = (uint32_t) v;
				r[1] = (uint32_t) (v >> 32);
				break;
			}
			case 0x7c:
			{
				uint64_t v = d_out((double) fa);
				r[0] = (uint32_t) v;
				r[1] = (uint32_t) (v >> 32);
				break;
			}
		}
		return;
	}

	if (id >= HLE_SD && id < HLE_SD + 32)
	{
		uint64_t a = d_in(((uint64_t) r[1] << 32) | r[0]);
		uint64_t b = d_in(((uint64_t) r[3] << 32) | r[2]);
		double da = d_of(a), db = d_of(b);
		uint64_t res = 0;
		bool wide = true;
		c.cycles += 80;
		switch ((id - HLE_SD) * 4)
		{
			case 0x00: res = d_arith(da + db); break;
			case 0x04: res = d_arith(da - db); break;
			case 0x08: res = d_arith(da * db, a ^ b); break;
			case 0x0c: res = d_arith(da / db, a ^ b); break;
			case 0x10:
			case 0x14:
			case 0x54:
			{
				uint64_t x = ((uint64_t) r[1] << 32) | r[0], y = ((uint64_t) r[3] << 32) | r[2];
				if ((id - HLE_SD) * 4 == 0x54)
				{
					x = d_in(x);
					y = d_in(y);
				}
				int32_t cr = dcmp_raw(x, y);
				r[0] = (uint32_t) cr;
				c.n = cr < 0; c.z = cr == 0; c.c = 1; c.v = 0;
				return;
			}
			case 0x18: res = d_out(sqrt(da)); break;
			case 0x1c: r[0] = (uint32_t) (int32_t) to_fix(da, 0, INT32_MIN, INT32_MAX); wide = false; break;
			case 0x20: r[0] = (uint32_t) (int32_t) to_fix(da, (int32_t) r[2], INT32_MIN, INT32_MAX); wide = false; break;
			case 0x24: r[0] = (uint32_t) to_ufix(da, 0, 0xffffffffu); wide = false; break;
			case 0x28: r[0] = (uint32_t) to_ufix(da, (int32_t) r[2], 0xffffffffu); wide = false; break;
			case 0x2c: res = d_out((double) (int32_t) r[0]); break;
			case 0x30: res = d_out(ldexp((double) (int32_t) r[0], -(int32_t) r[1])); break;
			case 0x34: res = d_out((double) r[0]); break;
			case 0x38: res = d_out(ldexp((double) r[0], -(int32_t) r[1])); break;
			case 0x3c: res = d_out(cos(da)); break;
			case 0x40: res = d_out(sin(da)); break;
			case 0x44: res = d_out(tan(da)); break;
			case 0x48:
			{
				uint64_t cv = d_out(cos(da));
				res = d_out(sin(da));
				r[2] = (uint32_t) cv;
				r[3] = (uint32_t) (cv >> 32);
				break;
			}
			case 0x4c: res = d_out(exp(da)); break;
			case 0x50: res = d_out(log(da)); break;
			case 0x58: res = d_out(atan2(da, db)); break;
			case 0x5c: res = d_out((double) (int64_t) (((uint64_t) r[1] << 32) | r[0])); break;
			case 0x60: res = d_out(ldexp((double) (int64_t) (((uint64_t) r[1] << 32) | r[0]), -(int32_t) r[2])); break;
			case 0x64: res = d_out((double) (((uint64_t) r[1] << 32) | r[0])); break;
			case 0x68: res = d_out(ldexp((double) (((uint64_t) r[1] << 32) | r[0]), -(int32_t) r[2])); break;
			case 0x6c: res = (uint64_t) to_fix(da, 0, INT64_MIN, INT64_MAX); break;
			case 0x70: res = (uint64_t) to_fix(da, (int32_t) r[2], INT64_MIN, INT64_MAX); break;
			case 0x74: res = to_ufix(da, 0, ~0ull); break;
			case 0x78: res = to_ufix(da, (int32_t) r[2], ~0ull); break;
			case 0x7c: r[0] = f_out((float) da); wide = false; break;
		}
		if (wide)
		{
			r[0] = (uint32_t) res;
			r[1] = (uint32_t) (res >> 32);
		}
		return;
	}

	switch (id)
	{
		case HLE_POPCOUNT: r[0] = popcount32(r[0]); break;
		case HLE_REVERSE:
		{
			uint32_t v = r[0], o = 0;
			for (int i = 0; i < 32; i++)
				o |= ((v >> i) & 1) << (31 - i);
			r[0] = o;
			break;
		}
		case HLE_CLZ: r[0] = r[0] ? clz32(r[0]) : 32; break;
		case HLE_CTZ: r[0] = r[0] ? ctz32(r[0]) : 32; break;
		case HLE_MEMSET:
		case HLE_MEMSET4:
		{
			uint32_t dst = r[0], val = r[1] & 0xff, cnt = r[2];
			if ((dst - 0x20000000u) < SRAM_SIZE && cnt <= SRAM_SIZE - (dst - 0x20000000u))
				memset(sram + (dst - 0x20000000u), (int) val, cnt);
			else
				for (uint32_t i = 0; i < cnt; i++)
					Write8(n, dst + i, (uint8_t) val);
			c.cycles += cnt / 4 + 8;
			break;
		}
		case HLE_MEMCPY:
		case HLE_MEMCPY44:
		{
			uint32_t dst = r[0], src = r[1], cnt = r[2];
			bool dst_ram = (dst - 0x20000000u) < SRAM_SIZE && cnt <= SRAM_SIZE - (dst - 0x20000000u);
			if (dst_ram && (src - 0x20000000u) < SRAM_SIZE && cnt <= SRAM_SIZE - (src - 0x20000000u))
				memmove(sram + (dst - 0x20000000u), sram + (src - 0x20000000u), cnt);
			else if (dst_ram && (src - 0x10000000u) < 0x04000000u && ((src & flash_mask) + cnt) <= flash.size())
				memcpy(sram + (dst - 0x20000000u), &flash[src & flash_mask], cnt);
			else
				for (uint32_t i = 0; i < cnt; i++)
					Write8(n, dst + i, Read8(n, src + i));
			c.cycles += cnt / 2 + 8;
			break;
		}
		case HLE_FLASH_CONNECT:
			memset(qspi_ctrl, 0, sizeof(qspi_ctrl));
			SpiSelect(false);
			c.cycles += 200;
			break;
		case HLE_FLASH_EXIT_XIP:
		case HLE_FLASH_FLUSH:
		case HLE_FLASH_ENTER_XIP:
			// all three finish by releasing the chip-select override
			qspi_ctrl[1] &= ~0x300u;
			SpiSelect(false);
			c.cycles += 200;
			break;
		case HLE_FLASH_ERASE:
		{
			uint32_t addr = r[0] & flash_mask, cnt = r[1];
			if (addr + cnt > flash.size())
				cnt = (uint32_t) flash.size() - addr;
			memset(&flash[addr], 0xff, cnt);
			flash_dirty = true;
			Log("rp2040: flash erase %06x+%x", addr, cnt);
			c.cycles += sys_hz / 20;		// ~50 ms per sector burst
			break;
		}
		case HLE_FLASH_PROGRAM:
		{
			uint32_t addr = r[0] & flash_mask, src = r[1], cnt = r[2];
			for (uint32_t i = 0; i < cnt && addr + i < flash.size(); i++)
				flash[addr + i] &= Read8(n, src + i);
			flash_dirty = true;
			Log("rp2040: flash program %06x+%x", addr, cnt);
			c.cycles += cnt * 8;
			break;
		}
		case HLE_WAIT_FOR_VECTOR:
			c.halted = true;
			c.launch_step = 0;
			Core1LaunchStep();
			break;
		case HLE_USB_BOOT:
			Log("rp2040: core%d reset_usb_boot", n);
			c.lockup = true;
			break;
		case HLE_DEAD:
			r[15] -= 2;
			c.sleeping = true;
			c.wfe = false;
			break;
		default:
			Log("rp2040: core%d unhandled ROM call %d", n, id);
			break;
	}
}

// Core 1's boot-ROM wait_for_vector handshake: echo 0, 0, 1, VTOR, SP, entry.
void Chip::Core1LaunchStep ()
{
	Core &c = core[1];
	if (!c.halted || c.launch_step < 0)
		return;
	for (int guard = 0; guard < 32; guard++)
	{
		if (c.launch_step == 0)
		{
			// drain, then announce ourselves with a 0
			fifo_level[0] = 0;
			if (fifo_level[1] >= 8)
				return;
			fifo[1][(fifo_head[1] + fifo_level[1]) & 7] = 0;
			fifo_level[1]++;
			core[0].event = true;
			WakeOnEvent(0);
			c.launch_step = 1;
			continue;
		}
		if (!fifo_level[0])
			return;
		uint32_t v = fifo[0][fifo_head[0]];
		fifo_head[0] = (fifo_head[0] + 1) & 7;
		fifo_level[0]--;
		if (v == 0 || (c.launch_step == 1 && v != 1))
		{
			c.launch_step = 0;
			continue;
		}
		if (c.launch_step >= 2)
			c.launch_words[c.launch_step] = v;
		if (fifo_level[1] < 8)
		{
			fifo[1][(fifo_head[1] + fifo_level[1]) & 7] = v;
			fifo_level[1]++;
		}
		core[0].event = true;
		WakeOnEvent(0);
		if (c.launch_step == 4)
		{
			uint32_t vt = c.launch_words[2], sp = c.launch_words[3], entry = c.launch_words[4];
			uint64_t t = c.cycles;
			ResetCore(1);
			c.cycles = t;
			c.halted = false;
			c.vtor = vt;
			c.msp = sp;
			c.r[13] = sp;
			c.r[15] = entry & ~1u;
			c.r[14] = ROM_CORE1_DEAD | 1;
			c.launch_step = -1;
			Log("rp2040: core1 launched at %08x sp=%08x vtor=%08x", entry, sp, vt);
			return;
		}
		c.launch_step++;
	}
}

// ---------------------------------------------------------------------------
// Scheduler

void Chip::RunUntil (uint64_t target)
{
	while (now < target)
	{
		if (reset_request)
		{
			reset_request = false;
			uint32_t scratch[8], reason = watchdog_reason;
			memcpy(scratch, watchdog_scratch, sizeof(scratch));
			uint64_t t = now;
			uint32_t ext = gpio_ext;
			PowerOn();
			now = t;
			us_last_cycles = t;
			core[0].cycles = core[1].cycles = t;
			core[0].systick_last = core[1].systick_last = t;
			gpio_ext = ext;
			memcpy(watchdog_scratch, scratch, sizeof(scratch));
			watchdog_reason = reason;
		}

		uint64_t end = now + SLICE;
		if (end > target)
			end = target;
		bool busy = false;
		for (int n = 0; n < 2; n++)
		{
			Core &c = core[n];
			if (c.halted || c.lockup || c.sleeping)
			{
				if (c.cycles < end)
					c.cycles = end;
				continue;
			}
			busy = true;
			irq_check[n] = true;
			if (c.cycles < end)
				Execute(n, end);
			if (c.sleeping && c.cycles < end)
				c.cycles = end;
		}
		if (!busy)
		{
			// Both cores idle: skip ahead to the next thing that can wake one.
			uint64_t ev = NextTimerEvent();
			if (ev > end)
				end = ev < target ? ev : target;
			for (int n = 0; n < 2; n++)
				if (core[n].cycles < end)
					core[n].cycles = end;
		}
		now = end;
		AdvanceTimer();
		SysTickAdvance(0);
		SysTickAdvance(1);
		if (irq_dirty)
		{
			irq_dirty = false;
			ServiceDma();
		}
		UpdateIrqLines();
		if (core[1].halted && core[1].launch_step >= 0)
			Core1LaunchStep();
		for (int n = 0; n < 2; n++)
		{
			Core &c = core[n];
			if (!c.sleeping)
				continue;
			if (c.wfe)
			{
				if (c.event)
				{
					c.event = false;
					c.sleeping = false;
				}
				else if (PendingException(n))
					c.sleeping = false;
			}
			else if ((c.nvic_pending & c.nvic_enable) || c.pendsv || c.systick_pend)
				c.sleeping = false;
		}
	}
}

// ---------------------------------------------------------------------------
// Savestates

#define STATE_FIELDS(X) \
	X(core) X(sram) X(xip_sram) X(usb_ram) X(now) X(sys_hz) X(us_count) X(us_rem) X(us_last_cycles) \
	X(resets) X(psm_frce_on) X(psm_frce_off) X(clk_ctrl) X(clk_div) X(pll_sys) X(pll_usb) \
	X(xosc_ctrl) X(xosc_startup) X(rosc_ctrl) X(rosc_lfsr) X(watchdog_ctrl) X(watchdog_load) \
	X(watchdog_reason) X(watchdog_scratch) X(watchdog_tick) X(vreg) X(bod) X(chip_reset) \
	X(busctrl_priority) X(syscfg) X(gpio_ctrl) X(pads) X(gpio_intr) X(gpio_inte) X(gpio_intf) \
	X(qspi_ctrl) X(qspi_pads) X(gpio_ext) X(gpio_ext_drive) X(gpio_prev) X(sio_out) X(sio_oe) X(sio_hi_out) X(sio_hi_oe) \
	X(fifo) X(fifo_head) X(fifo_level) X(fifo_sticky) X(spinlocks) X(timer_alarm) X(timer_armed) \
	X(timer_intr) X(timer_inte) X(timer_intf) X(timer_pause) X(timer_latched_hi) X(uart_cr) \
	X(uart_ibrd) X(uart_fbrd) X(uart_lcr) X(uart_imsc) X(adc_cs) X(adc_result) X(adc_div) X(adc_fcs) \
	X(pwm) X(ssi_regs) X(xip_ctrl) X(xip_stream_addr) X(xip_stream_ctr) X(spi_cmd) \
	X(spi_count) X(spi_addr) X(spi_selected) X(spi_wel) X(dma) X(dma_intr) X(dma_inte) X(dma_intf) \
	X(dma_timer) X(dma_sniff_ctrl) X(dma_sniff_data) X(pio) X(irq_check)

size_t Chip::StateSize ()
{
	size_t s = 8;
#define SZ(f) s += sizeof(f);
	STATE_FIELDS(SZ)
#undef SZ
	return s;
}

void Chip::SaveState (uint8_t *buf)
{
	uint8_t *p = buf;
	memcpy(p, "RP2040S1", 8);
	p += 8;
#define SV(f) memcpy(p, &f, sizeof(f)); p += sizeof(f);
	STATE_FIELDS(SV)
#undef SV
}

bool Chip::LoadState (const uint8_t *buf, size_t size)
{
	if (size != StateSize() || memcmp(buf, "RP2040S1", 8))
		return false;
	const uint8_t *p = buf + 8;
#define LD(f) memcpy(&f, p, sizeof(f)); p += sizeof(f);
	STATE_FIELDS(LD)
#undef LD
	ssi_rx.clear();
	irq_dirty = true;
	RecalcGpioMux();
	return true;
}

}
