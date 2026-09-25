/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Raspberry Pi RP2040: two Cortex-M0+ cores, SIO, DMA, PIO, timer and the
// rest of the chip at the level a pico-sdk firmware touches it. The boot ROM
// is replaced by native equivalents of its lookup tables and routines.
// Self-contained (no snes9x headers) so it can be driven standalone.

#ifndef _RP2040_H_
#define _RP2040_H_

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace RP2040
{

enum
{
	SRAM_SIZE		= 0x42000,
	ROM_SIZE		= 0x4000,
	XIP_SRAM_SIZE	= 0x4000,
	USB_RAM_SIZE	= 0x1000,
	NUM_IRQS		= 32,
	NUM_DMA			= 12,
	NUM_GPIO		= 30
};

struct Core
{
	uint32_t	r[16];
	uint32_t	n, z, c, v;				// APSR flags, 0 or 1
	uint32_t	msp, psp;				// banked SP (r[13] holds the live one)
	uint32_t	primask, control;
	uint32_t	ipsr;					// current exception number, 0 = thread
	uint64_t	cycles;					// local time in clk_sys cycles
	bool	sleeping;				// WFI/WFE
	bool	wfe;					// sleeping in WFE (an event wakes it)
	bool	event;					// the WFE event register
	bool	halted;					// core 1 held in reset / in the boot ROM
	bool	lockup;
	// NVIC and SCB, one set per core
	uint32_t	nvic_enable, nvic_pending, nvic_active;
	uint8_t	nvic_prio[NUM_IRQS];
	uint32_t	vtor, scr, shpr2, shpr3;
	uint32_t	exc_active;				// bit n: system exception n active
	bool	pendsv, systick_pend;
	uint32_t	systick_csr, systick_rvr, systick_cvr;
	uint64_t	systick_last;
	// SIO per-core blocks
	uint32_t	div_udividend, div_udivisor, div_quot, div_rem;
	bool	div_dirty;
	struct Interp
	{
		uint32_t	accum[2], base[3], ctrl[2];
	}	interp[2];
	// core 1 boot-ROM launch handshake (wait_for_vector)
	int		launch_step;
	uint32_t	launch_words[6];
	// idle-loop detection: register state and store hash at the last loop head
	uint32_t	spin_pc, spin_hash, store_hash;
	uint32_t	spin_regs[13], spin_flags;
};

struct PioSm
{
	uint32_t	clkdiv, execctrl, shiftctrl, pinctrl;
	uint32_t	pc, x, y, isr, osr;
	uint32_t	isr_count, osr_count;
	uint32_t	tx[8], rx[8];
	uint32_t	tx_head, tx_level, rx_head, rx_level;
	bool	exec_pending;
	uint32_t	exec_instr;
	bool	stalled;
};

struct Pio
{
	uint32_t	ctrl, fdebug, irq, irq_inte[2], irq_intf[2];
	uint32_t	input_sync_bypass;
	uint16_t	instr[32];
	PioSm	sm[4];
	uint32_t	pin_out, pin_oe;			// what the state machines drive
};

struct DmaChannel
{
	uint32_t	read_addr, write_addr, trans_count, reload, ctrl;
	bool	busy;
};

class Chip
{
public:
	Chip ();

	bool	LoadFlash (const uint8_t *data, size_t size);
	void	LoadBootRom (const uint8_t *data, size_t size);	// optional real ROM (testing)
	void	PowerOn ();

	// Run both cores until the global clock reaches target (clk_sys cycles).
	void	RunUntil (uint64_t target);
	uint64_t Now () const { return now; }
	uint32_t SysHz () const { return sys_hz; }

	// External pins: the levels the outside world presents, and which of
	// them it drives hard enough to override the chip's own outputs.
	void	SetGpioInputs (uint32_t levels, uint32_t drive) { gpio_ext = levels; gpio_ext_drive = drive; }
	uint32_t GpioInputs () const { return gpio_ext; }
	uint32_t GpioOutputs ();			// resolved pad levels driven by the chip
	void	StepPio ();					// let the state machines react to input changes
	void	ServiceDma ();

	// Flash sector writes the firmware made, for persisting a save area.
	bool	FlashDirty () const { return flash_dirty; }
	void	ClearFlashDirty () { flash_dirty = false; }
	const std::vector<uint8_t> &Flash () const { return flash; }
	std::vector<uint8_t> &FlashMutable () { return flash; }

	// Savestates: everything except flash (which is saved separately).
	size_t	StateSize ();
	void	SaveState (uint8_t *buf);
	bool	LoadState (const uint8_t *buf, size_t size);

	// Debug
	void	(*log_fn) (const char *msg);
	uint32_t hle_calls[256];
	uint64_t instr_count;
	bool	trace, trace_io;
	uint32_t *profile[2];				// with trace: hit counts per halfword, flash then SRAM (1 << 19 entries)

	Core	core[2];

	// Bus (public so the CPU helpers can reach them)
	uint32_t Read32 (int cpu, uint32_t addr);
	uint16_t Read16 (int cpu, uint32_t addr);
	uint8_t	Read8 (int cpu, uint32_t addr);
	void	Write32 (int cpu, uint32_t addr, uint32_t v);
	void	Write16 (int cpu, uint32_t addr, uint16_t v);
	void	Write8 (int cpu, uint32_t addr, uint8_t v);

private:
	// Memories
	std::vector<uint8_t> flash;
	uint32_t flash_mask;
	bool	flash_dirty;
	uint8_t	rom[ROM_SIZE];
	bool	real_rom;
	uint8_t	sram[SRAM_SIZE];
	uint8_t	xip_sram[XIP_SRAM_SIZE];
	uint8_t	usb_ram[USB_RAM_SIZE];

	uint64_t now;
	uint32_t sys_hz;
	uint64_t us_count, us_rem;		// TIMER, in microseconds
	uint64_t us_last_cycles;

	// Peripherals
	uint32_t resets;
	uint32_t psm_frce_on, psm_frce_off;
	uint32_t clk_ctrl[10], clk_div[10];
	uint32_t pll_sys[4], pll_usb[4];
	uint32_t xosc_ctrl, xosc_startup, rosc_ctrl, rosc_lfsr;
	uint32_t watchdog_ctrl, watchdog_load, watchdog_reason, watchdog_scratch[8], watchdog_tick;
	uint32_t vreg, bod, chip_reset;
	uint32_t busctrl_priority;
	uint32_t syscfg[8];
	uint32_t gpio_ctrl[NUM_GPIO], pads[NUM_GPIO + 2];
	uint32_t gpio_intr[4], gpio_inte[2][4], gpio_intf[2][4];
	uint32_t qspi_ctrl[6], qspi_pads[7];
	uint32_t gpio_ext, gpio_ext_drive, gpio_prev;
	uint32_t mux_sio, mux_pio0, mux_pio1;		// derived from gpio_ctrl
	uint32_t out_inv, out_force, out_force_val, oe_inv, oe_force, oe_force_val, in_inv, in_force, in_force_val;
	uint32_t sio_out, sio_oe;
	uint32_t sio_hi_out, sio_hi_oe;
	uint32_t fifo[2][8];			// fifo[n]: written by core n, read by the other
	uint32_t fifo_head[2], fifo_level[2];
	uint32_t fifo_sticky[2];		// per reader: ROE/WOF
	uint32_t spinlocks;
	uint32_t timer_alarm[4], timer_armed, timer_intr, timer_inte, timer_intf, timer_pause;
	uint32_t timer_latched_hi;
	uint32_t uart_cr[2], uart_ibrd[2], uart_fbrd[2], uart_lcr[2], uart_imsc[2];
	std::vector<char> uart_line[2];
	uint32_t adc_cs, adc_result, adc_div, adc_fcs;
	uint32_t pwm[0xb4 / 4];
	uint32_t ssi_regs[0x100 / 4];
	uint32_t xip_ctrl, xip_stream_addr, xip_stream_ctr;
	// flash SPI device behind the SSI
	uint8_t	spi_cmd;
	uint32_t spi_count, spi_addr;
	bool	spi_selected, spi_wel, ssi_idle_seen;
	std::vector<uint8_t> ssi_rx;

	bool	irq_check[2];			// re-evaluate pending exceptions before the next instruction
	bool	irq_dirty;
	bool	reset_request;

	DmaChannel dma[NUM_DMA];
	uint32_t dma_intr, dma_inte[2], dma_intf[2];
	uint32_t dma_timer[4];
	uint32_t dma_sniff_ctrl, dma_sniff_data;
	bool	dma_in_service;

	Pio		pio[2];

	// CPU
	void	ResetCore (int n);
	void	Execute (int n, uint64_t until);
	void	TakeException (int n, uint32_t exc);
	void	ExceptionReturn (int n, uint32_t exc_return);
	int		PendingException (int n);
	int		ExecutionPriority (int n);
	int		ExceptionPriority (int n, int exc);
	void	BranchWritePc (int n, uint32_t addr);
	void	BxWritePc (int n, uint32_t addr);
	uint32_t Xpsr (int n);
	void	SetSpSel (int n, bool psp);
	void	Hle (int n, uint32_t id);
	void	Core1LaunchStep (void);
	void	HardFault (int n, const char *why, uint32_t addr);

	// Bus internals
	uint32_t PeriphRead (int cpu, uint32_t addr);
	void	PeriphWrite (int cpu, uint32_t addr, uint32_t v, int size);
	uint32_t SioRead (int cpu, uint32_t off);
	void	SioWrite (int cpu, uint32_t off, uint32_t v);
	uint32_t PpbRead (int cpu, uint32_t off);
	void	PpbWrite (int cpu, uint32_t off, uint32_t v);
	uint32_t ApbRead (uint32_t addr);
	void	ApbWrite (uint32_t addr, uint32_t v, int alias);
	uint32_t DmaRead (uint32_t off);
	void	DmaWrite (uint32_t off, uint32_t v, int alias);
	uint32_t PioRead (int p, uint32_t off);
	void	PioWrite (int p, uint32_t off, uint32_t v, int alias);
	uint32_t SsiRead (uint32_t off);
	void	SsiWrite (uint32_t off, uint32_t v);
	uint8_t	SpiXfer (uint8_t out);
	void	SpiSelect (bool sel);

	// Timing and interrupts
	void	UpdateClocks ();
	void	AdvanceTimer ();
	void	UpdateIrqLines ();
	void	RecalcGpioMux ();
	uint32_t IrqLines ();
	uint64_t NextTimerEvent ();
	void	SysTickAdvance (int n);
	void	WakeOnEvent (int n);

	// DMA
	void	DmaTrigger (int ch);
	bool	DmaDreq (int treq);
	void	DmaComplete (int ch);

	// PIO
	void	PioExec (int p, int sm, uint16_t ins, bool from_exec);
	void	PioRunSm (int p, int sm, int budget);
	void	PioRestartSm (int p, int sm);
	bool	PioTxPush (int p, int sm, uint32_t v);
	bool	PioTxPop (int p, int sm, uint32_t &v);
	bool	PioRxPush (int p, int sm, uint32_t v);
	bool	PioRxPop (int p, int sm, uint32_t &v);
	void	PioSetPins (int p, uint32_t base, uint32_t count, uint32_t v, bool dirs);
	uint32_t PioFstat (int p);
	uint32_t PioIntr (int p);

	// SIO helpers
	uint32_t InterpRead (int cpu, int i, uint32_t off);
	void	InterpWrite (int cpu, int i, uint32_t off, uint32_t v);
	void	InterpResult (Core::Interp &it, int i, uint32_t res[3], bool &overf);

	void	BuildHleRom ();
	void	Log (const char *fmt, ...);
};

}

#endif
