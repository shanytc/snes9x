/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_MEMORY_H_
#define _SGB_GB_MEMORY_H_

#include <cstdint>

namespace SGB {

struct Cart;
struct Ppu;
struct Apu;
struct Timer;
struct Joypad;
struct CpuState;

// GB address space — flat 16-bit. All access goes through this one bus.
//   0x0000-0x3FFF  ROM bank 0
//   0x4000-0x7FFF  ROM bank N (MBC-switched)
//   0x8000-0x9FFF  VRAM
//   0xA000-0xBFFF  External RAM (cart)
//   0xC000-0xDFFF  WRAM
//   0xE000-0xFDFF  Echo RAM (mirror of C000-DDFF)
//   0xFE00-0xFE9F  OAM
//   0xFEA0-0xFEFF  Unusable
//   0xFF00-0xFF7F  I/O
//   0xFF80-0xFFFE  HRAM
//   0xFFFF         IE register
// Callback fires each time the CPU initiates a serial transfer (write
// 0x80/0x81 to 0xFF02). The byte passed is whatever was in 0xFF01 at
// the moment. Used by the test harness to capture Blargg output; P6a
// may hook it too. nullptr disables.
using SerialByteCallback = void (*)(void *user, uint8_t byte);

struct Memory
{
	Cart   *cart   = nullptr;
	Ppu    *ppu    = nullptr;
	Apu    *apu    = nullptr;
	Timer  *timer  = nullptr;
	Joypad *joypad = nullptr;
	// CPU clock, for PPU-register write-time reconstruction. In the per-dot
	// interleave the CPU trails the PPU by up to kMaxOpcodeTCycles, so at the
	// moment a store reaches PpuWriteReg the PPU has already rendered dots the
	// write should have affected; cpu->t_cycles gives the store's true dot.
	// Transient wiring like ppu/apu above — never serialized.
	const CpuState *cpu = nullptr;

	uint8_t wram[0x8000];
	uint8_t hram[0x7F];
	uint8_t ie;             // 0xFFFF
	uint8_t if_;            // 0xFF0F
	uint8_t serial_data;    // 0xFF01 last written byte
	uint8_t serial_control; // 0xFF02 last written; bit 7 stays set on external-clock waits
	uint8_t serial_bits = 0; // internal-clock transfer: bits left to shift (0 = idle)
	uint8_t serial_guard = 0; // ticks after arming during which edges don't clock

	// Boot ROM overlay, cleared by the first write to 0xFF50. boot_rom_size
	// picks the layout: 0x100 = DMG/SGB (0x0000-0x00FF), 0x900 = CGB (also
	// 0x0200-0x08FF, leaving the cart header at 0x0100-0x01FF visible).
	uint8_t  boot_rom[0x900];
	uint16_t boot_rom_size;
	bool     boot_rom_enabled;

	uint8_t  svbk = 1;            // 0xFF70
	bool     key1_armed   = false;
	bool     double_speed = false;

	// CGB undocumented registers ($FF72/$FF73/$FF74 R/W, $FF75 bits 6-4).
	uint8_t  ff72 = 0, ff73 = 0, ff74 = 0, ff75 = 0;

	// True when the machine is CGB hardware, including its DMG-compat
	// mode (ppu->cgb is false there). STOP display + speed-switch gates.
	bool     cgb_hw = false;

	uint8_t  hdma1 = 0, hdma2 = 0, hdma3 = 0, hdma4 = 0;  // 0xFF51-0xFF54
	uint8_t  hdma5 = 0xFF;        // 0xFF55 status
	uint16_t hdma_src = 0, hdma_dst = 0, hdma_len = 0;
	bool     hdma_active = false;
	bool     hdma_hblank_latch = false;

	// Double-speed odd-cycle carry for MemTick's CPU→PPU/APU clock halving.
	// Transient (never serialized).
	uint8_t  ds_tick_rem = 0;

	// OAM DMA engine — one byte per M-cycle in the CPU clock domain, with
	// a 1-M-cycle startup delay after the $FF46 write. While a transfer
	// runs, CPU reads of OAM return $FF and reads on the bus the DMA
	// source occupies return the byte currently on the DMA bus.
	bool     dma_active   = false;  // a transfer is moving bytes
	int16_t  dma_index    = 0;      // next byte 0..159
	uint16_t dma_src      = 0;      // source base (page << 8, echo-folded)
	int8_t   dma_setup    = 0;      // M-cycles until dma_pending goes live
	uint16_t dma_src_next = 0;      // source staged by the latest $FF46 write
	uint8_t  dma_bus_byte = 0xFF;   // byte currently on the DMA bus
	uint8_t  dma_oam_old[0xA0];     // OAM content as of DMA start (scan-slot rewind)
	uint8_t  dma_last     = 0xFF;   // last byte written to $FF46; reads echo it
	// Set while a DMA/HDMA block moves bytes, so its own reads and writes
	// bypass the CPU-visible VRAM/OAM locks. Transient (never serialized).
	bool     dma_vram_bypass = false;

	// Fires each time the CPU starts a serial transfer. Per instance so
	// parallel cores can each capture their own output.
	SerialByteCallback serial_cb   = nullptr;
	void              *serial_user = nullptr;
};

// Advance the world by `tcycles` CPU T-cycles: timer runs in the CPU clock
// domain, PPU and APU in real time (half rate under CGB double-speed). The
// CPU core calls this once per machine cycle BEFORE each bus access, so
// reads/writes observe hardware state at their exact M-cycle.
void MemTick(Memory &m, int32_t tcycles, bool tick_dma = true);
void MemOamBugIncDec(Memory &m, uint16_t value);

uint8_t MemRead(Memory &m, uint16_t addr);
void    MemWrite(Memory &m, uint16_t addr, uint8_t value);

// 16-bit helpers assume little-endian, two sequential 8-bit accesses.
uint16_t MemRead16(Memory &m, uint16_t addr);
void     MemWrite16(Memory &m, uint16_t addr, uint16_t value);

void MemReset(Memory &m, bool cgb);

// Transfer one 0x10-byte CGB HDMA block during HBlank. No-op when inactive.
void MemHdmaHBlank(Memory &m);

// DMG LCDC write conflict (SameBoy GB_CONFLICT_DMG_LCDC): one dot before
// the real value lands, the register briefly holds a blend — BG-enable
// applies early, and OBJ-disable applies early at pixel 0 or mid object
// fetch. Returns false when no conflict write is needed (CGB).
bool MemLcdcPartial(Memory &m, uint8_t value, uint8_t *partial);

// LCD turned off outside HBlank with an HBlank HDMA armed: the off edge
// counts as entering HBlank, so one pending block fires (SameBoy GB_lcd_off).
void MemHdmaLcdOff(Memory &m);

void SetSerialCallback(Memory &m, SerialByteCallback cb, void *user = nullptr);

} // namespace SGB

#endif
