/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "sgb.h"

#include "gb_cpu.h"
#include "gb_memory.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_cart.h"
#include "sgb_packet.h"
#include "sgb_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace SGB {

// Embedded SGB1 / SGB2 GB-side boot ROMs. These are the authentic boot ROMs
// that scroll the Nintendo logo AND produce the 5-packet header handshake
// ($F1/$F3/$F5/$F7/$F9 command bytes + cart header bytes) that the SGB BIOS
// on the SNES side waits for to unblock its splash screen.
//
// Source: LIJI32/SameBoy (https://github.com/LIJI32/SameBoy), MIT license.
// Redistributed via Mesen2 (https://github.com/SourMesen/Mesen2 —
// Core/Gameboy/GbBootRom.h), also MIT, and tweaked with a short delay
// loop at the handoff so Tetris DX's SGB border doesn't get skipped.
// Preserving those redistributions here under the same terms.
static const uint8_t kSgbBootRom[256] = {
    0x31, 0xFE, 0xFF, 0x21, 0x00, 0x80, 0x22, 0xCB, 0x6C, 0x28, 0xFB, 0x3E,
    0x80, 0xE0, 0x26, 0xE0, 0x11, 0x3E, 0xF3, 0xE0, 0x12, 0xE0, 0x25, 0x3E,
    0x77, 0xE0, 0x24, 0x3E, 0x00, 0xE0, 0x47, 0x11, 0x04, 0x01, 0x21, 0x10,
    0x80, 0x1A, 0x47, 0xCD, 0xC9, 0x00, 0xCD, 0xC9, 0x00, 0x13, 0x7B, 0xEE,
    0x34, 0x20, 0xF2, 0x11, 0xEA, 0x00, 0x0E, 0x08, 0x1A, 0x13, 0x22, 0x23,
    0x0D, 0x20, 0xF9, 0x3E, 0x19, 0xEA, 0x10, 0x99, 0x21, 0x2F, 0x99, 0x0E,
    0x0C, 0x3D, 0x28, 0x08, 0x32, 0x0D, 0x20, 0xF9, 0x2E, 0x0F, 0x18, 0xF5,
    0x3E, 0x91, 0xE0, 0x40, 0x3E, 0xF1, 0xE0, 0x80, 0x21, 0x04, 0x01, 0xAF,
    0x4F, 0xAF, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xCD, 0xB7, 0x00, 0xE5,
    0x06, 0x0E, 0x16, 0x00, 0xCD, 0xAD, 0x00, 0x82, 0x57, 0x05, 0x20, 0xF8,
    0xCD, 0xB7, 0x00, 0xE1, 0x06, 0x0E, 0xCD, 0xAD, 0x00, 0xCD, 0xB7, 0x00,
    0x05, 0x20, 0xF7, 0x3E, 0x20, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xC6,
    0x02, 0xE0, 0x80, 0x3E, 0x58, 0xBD, 0x20, 0xC9, 0x0E, 0x13, 0x3E, 0xC1,
    0xE2, 0x0C, 0x3E, 0x07, 0xE2, 0x3E, 0xFC, 0xE0, 0x47, 0x3E, 0x01, 0x21,
    0x60, 0xC0, 0xC3, 0xF2, 0x00, 0x3E, 0x4F, 0xBD, 0x38, 0x02, 0x2A, 0xC9,
    0x23, 0xAF, 0xC9, 0x5F, 0x16, 0x08, 0x3E, 0x10, 0xCB, 0x1B, 0x38, 0x01,
    0x87, 0xE2, 0x3E, 0x30, 0xE2, 0x15, 0xC8, 0x18, 0xF1, 0x3E, 0x04, 0x0E,
    0x00, 0xCB, 0x20, 0xF5, 0xCB, 0x11, 0xF1, 0xCB, 0x11, 0x3D, 0x20, 0xF5,
    0x79, 0x22, 0x23, 0x22, 0x23, 0xC9, 0xE5, 0x21, 0x0F, 0xFF, 0xCB, 0x86,
    0xCB, 0x46, 0x28, 0xFC, 0xE1, 0xC9, 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5,
    0x42, 0x3C, 0x16, 0xE1, 0x1E, 0xAA, 0x1B, 0x14, 0x15, 0x20, 0xFB, 0x5A,
    0xEE, 0x00, 0xE0, 0x50
};

static const uint8_t kSgb2BootRom[256] = {
    0x31, 0xFE, 0xFF, 0x21, 0x00, 0x80, 0x22, 0xCB, 0x6C, 0x28, 0xFB, 0x3E,
    0x80, 0xE0, 0x26, 0xE0, 0x11, 0x3E, 0xF3, 0xE0, 0x12, 0xE0, 0x25, 0x3E,
    0x77, 0xE0, 0x24, 0x3E, 0x00, 0xE0, 0x47, 0x11, 0x04, 0x01, 0x21, 0x10,
    0x80, 0x1A, 0x47, 0xCD, 0xC9, 0x00, 0xCD, 0xC9, 0x00, 0x13, 0x7B, 0xEE,
    0x34, 0x20, 0xF2, 0x11, 0xEA, 0x00, 0x0E, 0x08, 0x1A, 0x13, 0x22, 0x23,
    0x0D, 0x20, 0xF9, 0x3E, 0x19, 0xEA, 0x10, 0x99, 0x21, 0x2F, 0x99, 0x0E,
    0x0C, 0x3D, 0x28, 0x08, 0x32, 0x0D, 0x20, 0xF9, 0x2E, 0x0F, 0x18, 0xF5,
    0x3E, 0x91, 0xE0, 0x40, 0x3E, 0xF1, 0xE0, 0x80, 0x21, 0x04, 0x01, 0xAF,
    0x4F, 0xAF, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xCD, 0xB7, 0x00, 0xE5,
    0x06, 0x0E, 0x16, 0x00, 0xCD, 0xAD, 0x00, 0x82, 0x57, 0x05, 0x20, 0xF8,
    0xCD, 0xB7, 0x00, 0xE1, 0x06, 0x0E, 0xCD, 0xAD, 0x00, 0xCD, 0xB7, 0x00,
    0x05, 0x20, 0xF7, 0x3E, 0x20, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xC6,
    0x02, 0xE0, 0x80, 0x3E, 0x58, 0xBD, 0x20, 0xC9, 0x0E, 0x13, 0x3E, 0xC1,
    0xE2, 0x0C, 0x3E, 0x07, 0xE2, 0x3E, 0xFC, 0xE0, 0x47, 0x3E, 0xFF, 0x21,
    0x60, 0xC0, 0xC3, 0xF2, 0x00, 0x3E, 0x4F, 0xBD, 0x38, 0x02, 0x2A, 0xC9,
    0x23, 0xAF, 0xC9, 0x5F, 0x16, 0x08, 0x3E, 0x10, 0xCB, 0x1B, 0x38, 0x01,
    0x87, 0xE2, 0x3E, 0x30, 0xE2, 0x15, 0xC8, 0x18, 0xF1, 0x3E, 0x04, 0x0E,
    0x00, 0xCB, 0x20, 0xF5, 0xCB, 0x11, 0xF1, 0xCB, 0x11, 0x3D, 0x20, 0xF5,
    0x79, 0x22, 0x23, 0x22, 0x23, 0xC9, 0xE5, 0x21, 0x0F, 0xFF, 0xCB, 0x86,
    0xCB, 0x46, 0x28, 0xFC, 0xE1, 0xC9, 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5,
    0x42, 0x3C, 0x16, 0xE1, 0x1E, 0xAA, 0x1B, 0x14, 0x15, 0x20, 0xFB, 0x5A,
    0xEE, 0x00, 0xE0, 0x50
};

struct Emulator::Impl
{
	Cpu         cpu;
	Memory      mem;
	Ppu         ppu;
	Apu         apu;
	Timer       timer;
	Joypad      joypad;
	Cart        cart;
	PacketState sgb_pkt;
	SgbState    sgb_state;

	// 256×224 composite staging buffer — heap-resident to keep a
	// ~112 KB allocation off the stack of whatever thread drives
	// S9xMainLoop (snes9x's per-thread stacks aren't always large).
	uint16_t    composite[SGB_BORDER_W * SGB_BORDER_H];

	RunMode     run_mode  = RunMode::SGB;
	FrameBuffer fb{};
	bool        has_rom   = false;
	float       clock_mul = 1.0f;

	// Staged GB-side boot ROM. Copied into mem.boot_rom on Reset when
	// boot_rom_loaded is true (authentic BIOS mode only). Staging is kept
	// separate because MemReset zeroes mem.boot_rom.
	uint8_t     boot_rom_staging[0x100];
	bool        boot_rom_loaded = false;

	// ICD2 state — the SGB cart-chip register set exposed at 0x6000-0x7FFF
	// on the SNES side when running under a real BIOS. Layout mirrors the
	// community-documented SGB hardware protocol (see sgb.h for the register
	// table; see bsnes / Mesen2 for reference implementations — their
	// register choices match what we implement here).
	struct Icd2
	{
		// $6003 — control. bit 7 = GB release, 5..4 = player count,
		// 1..0 = clock divider select.
		uint8_t  control;

		// $6001 — LCD row bank select for the $7800 char window. Also
		// resets the $7800 read position to 0. P2d consumes this.
		uint8_t  lcd_row_select;

		// $7800 auto-increment pointer. Populated in P2d.
		uint16_t read_position;

		// $6004-$6007 — joypad state that the GB sees when it polls
		// $FF00. P2e will wire these to the SNES pad.
		uint8_t  joypad[4];
		uint8_t  input_value;   // last $FF00 write from GB (for edge detect)
		uint8_t  input_index;   // 0..3 — current MLT_REQ player slot

		// Packet assembler. GB bit-bangs SGB commands over $FF00:
		//   $00 (P14+P15 both active) = reset/start pulse
		//   $10 (P15 active only)     = 1-bit
		//   $20 (P14 active only)     = 0-bit
		//   $30 (both inactive)       = clock-high / idle
		// We shift bits into bit_accumulator LSB-first, pack into
		// assembly_buf[] every 8 bits. On the 16th byte the fully-
		// assembled packet is pushed onto packet_queue.
		uint8_t  assembly_buf[16];
		uint16_t bit_accumulator;
		uint8_t  packet_bit;    // 0..7
		uint8_t  packet_byte;   // 0..16
		bool     in_packet;

		// 64-deep packet queue (ring buffer). Matches bsnes's icd.cpp
		// packet path. The GB sends packets in bursts (5 handshake
		// packets back-to-back, plus game-driven palette/CHR/PCT/ATTR
		// transfers) — a single-slot FIFO loses anything after the
		// first while the BIOS is busy draining, which corrupts the
		// expected command sequence and strands the BIOS waiting for
		// $6002 forever. Queue drains on $7000-$700F reads: drain_ptr
		// advances per read; at 16 we pop the head and decrement count.
		uint8_t  packet_queue[64][16];
		uint8_t  queue_head;         // next packet to drain (0..63)
		uint8_t  queue_tail;         // next slot to push (0..63)
		uint8_t  queue_count;        // number of packets queued (0..64)

		// P2c diagnostic counters — help verify packet flow by OSD.
		uint32_t packets_received;   // completed 16-byte packets
		uint32_t fifo_reads;         // $7000-$700F reads
		uint32_t ctrl_writes;        // $6003 writes
		uint32_t row_writes;         // $6001 writes
		uint32_t f1_packets;         // packets whose first byte == $F1 (boot ROM handshake)
		uint8_t  last_cmd_ids[8];    // ring buffer of last 8 packet command IDs (byte0 >> 3)
		uint8_t  last_cmd_ids_len;   // 0..8

		// Per-address read/write counts for the registers the BIOS is
		// most likely to poll. Lets the status line expose which
		// register the BIOS is hot-looping on (distinct from bucketed
		// counts since $60xx and $78xx share low nibbles).
		uint32_t r_6000, r_6002, r_6003, r_600F, r_7000, r_7800;
		uint32_t w_6000, w_6001, w_6003, w_7000, w_6004;
		uint16_t last_read_addr;
		uint16_t last_write_addr;
		uint8_t  last_write_val;

		// First byte of the first-ever packet to arrive after reset —
		// that's what the SGB2 BIOS checks at $BE69 against #$F1. If
		// this is not $F1, the BIOS's $02C0 handshake counter resets
		// every call and the boot loop at $BE3C never exits.
		uint8_t  first_packet_byte0;
		bool     first_packet_seen;

		// Diagnostic: snapshot of the FIRST packet's full 16 bytes, and
		// the byte0 of the first 8 packets. Lets us verify (a) that the
		// Nintendo logo bytes at $0104.. reach the BIOS intact inside
		// packet 0, and (b) that the boot ROM's packet-0 byte sequence
		// is exactly F1 F3 F5 F7 F9 (5 packets) rather than a longer
		// drifting list.
		uint8_t  pkt0_bytes[16];
		uint8_t  byte0_log[8];
		uint8_t  byte0_log_len;

		// Ring of the last 16 bytes returned for $7000-$700F reads.
		// If the BIOS's drain path is reaching our ICD2 handler, we'll
		// see real packet bytes here. If it's getting zeros/FFs, our
		// queue wasn't serving valid data at that moment.
		uint8_t  last_r7000_vals[16];
		uint8_t  last_r7000_idx;

		// bsnes-style packet read buffer. Reading $6002 with a pending
		// packet pops the front of the queue into r7000_buf[0..15], and
		// subsequent $7000-$700F reads return from this buffer (NOT the
		// queue directly). This matters because the BIOS may not read
		// all 16 bytes of a packet — if we only pop on $700F, we'd keep
		// re-reading the same packet forever.
		uint8_t  r7000_buf[16];

		// Synthesized boot-ROM handshake. Most GB boot ROM dumps are plain
		// DMG — they scroll the Nintendo logo and disable themselves but
		// do NOT send the 5-packet SGB handshake the BIOS requires. We
		// synthesize them here from the cart header so BIOS mode works
		// without needing an SGB-specific boot ROM.
		uint8_t  synth_packets[6][16];
		uint8_t  synth_remaining;    // packets yet to hand out (6 → 0)
		uint8_t  drain_ptr;          // bytes read of current packet (0..15)

		// P2d — framebuffer char-transfer for $7800. The SGB BIOS reads
		// 320 bytes per bank (8 rows × 20 tiles × 2 bit-planes) to
		// reconstruct a 160×8 slice of the GB screen. 144 scanlines /
		// 8 = 18 full slices per frame, so we advance an 18-slot slice
		// index on each $6001 write.
		uint8_t  slice_index;        // 0..17 — which 8-row band of the GB frame

		// $6000 row/bank counters (Mesen2-style). Advanced by GB PPU
		// scanline events: sgb_row++ on each HBlank end (mode 0 → OAM
		// scan for line 1..143), sgb_bank advances every 8 rows, and
		// both get zeroed on VBlank entry. $6000 = (row & 0xF8) | bank.
		uint8_t  sgb_row;
		uint8_t  sgb_bank;

		// Per-pixel capture ring — 4 banks × 8 rows × 160 pixels (palette
		// indices 0..3). The GB PPU's renderer calls S9xSGBCaptureScanline
		// after drawing each visible line, which writes into bank[sgb_bank]
		// at row (sgb_row & 7). Matches bsnes/Mesen2 layout but only used
		// as a transitional buffer; $7800 reads from full_frame instead
		// (see below).
		uint8_t  lcd_ring[4][8 * 160];

		// Full 18-slice frame buffer. Each scanline N is captured into
		// full_frame[N/8][N%8] without bank-wrap overwrite, so all 18
		// slices (rows 0-7, 8-15, ..., 136-143) are simultaneously
		// addressable.
		uint8_t  full_frame[18][8 * 160];

		// BIOS's $6001-write sequence counter. The BIOS writes $6001
		// once per slice in order (slice 0, 1, ..., 17, then wraps).
		// We track this independently of the bank value the BIOS
		// writes, so $7800 reads map to the actual slice the BIOS is
		// requesting — fixing the bank-wrap ambiguity (BIOS reads bank
		// 3 at frame-end-wrap expecting slice 17, not slice 15 which
		// happens to be in bank 3 at that moment).
		uint8_t  read_slice;          // 0..17, current slice being read

		// Snapshot of the selected bank at the moment $6001 was written.
		// $7800 reads from this snapshot (not the live lcd_ring), so the
		// BIOS gets a stable view of the bank even if the GB overwrites
		// it while the BIOS is mid-DMA. Without this, a bank whose rows
		// are concurrently being re-drawn (slice 4 overwrites slice 0's
		// bank 0, for instance) reads as a mix of two slices and produces
		// horizontally-banded tile corruption in CHR_TRN output.
		uint8_t  r7800_snapshot[8 * 160];
	} icd2;

	// Cache of the first 6 packets bit-banged by the GB boot ROM. The
	// SGB BIOS handshake validates that two consecutive GB resets produce
	// byte-identical packets ($B8AE:BE3C captures byte 1s into $7E:1718,
	// then $B0F6:B107 drains a second time and $B119 compares against
	// $1718 — mismatch → $02FA=1 → cart error). Stored OUTSIDE the
	// Icd2 struct because Reset() zeroes icd2 on every $6003 0→1
	// transition, and we want the cache to persist across those resets
	// so we can re-queue deterministic packets on subsequent releases.
	uint8_t  cached_packets[6][16]{};
	uint8_t  cached_count   = 0;   // 0..6 (how many filled during first boot)
	bool     cache_valid    = false; // set true once 6 packets cached
	uint8_t  replays_done   = 0;   // cap replay count so post-splash releases don't
	                                // keep re-queuing handshake packets (which would
	                                // block game-generated SGB commands).
};

// File-local trampoline — lets the process-global SgbCommandCallback
// forward into the singleton Emulator's Impl.
static void SgbCommandTrampoline(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	Instance().OnSgbCommandInternal(cmd, data, len);
}

Emulator::Emulator() : impl_(new Impl) {}

Emulator::~Emulator() { delete impl_; }

bool Emulator::Init()
{
	ColdReset();   // first-time init: clear cache state too
	SetSgbCommandCallback(&SgbCommandTrampoline);
	return true;
}

void Emulator::Deinit()
{
	UnloadROM();
}

void Emulator::ColdReset()
{
	// Clear the BIOS-handshake cache before delegating to Reset(). On a
	// user-initiated reset (File→Reset / new ROM load), the SNES BIOS
	// will run from scratch and expects to perform the full handshake
	// again — but our cache_valid flag and cached_packets persist
	// across the GB-side Reset() (which is correct for in-game $6003
	// 0→1 transitions). Without clearing, the next $6003 release skips
	// the boot-ROM run and replays cached packets, producing no SGB
	// splash and either a black screen or whatever stale state the
	// SNES retained from before the reset.
	std::memset(impl_->cached_packets, 0, sizeof impl_->cached_packets);
	impl_->cached_count = 0;
	impl_->cache_valid  = false;
	impl_->replays_done = 0;
	Reset();
}

void Emulator::Reset()
{
	impl_->cpu.Reset();
	MemReset(impl_->mem);
	PpuReset(impl_->ppu);
	ApuReset(impl_->apu);
	TimerReset(impl_->timer);
	JoypadReset(impl_->joypad);
	PacketReset(impl_->sgb_pkt);
	SgbReset(impl_->sgb_state);
	std::memset(&impl_->icd2, 0, sizeof impl_->icd2);
	// $7800 capture ring starts as $FF (matches bsnes `output[2048]=0xFF`).
	std::memset(impl_->icd2.lcd_ring, 0xFF, sizeof impl_->icd2.lcd_ring);
	// $7800 snapshot buffer also $FF so early reads (before any $6001
	// write) return open-bus-like all-ones rather than stale pixels.
	std::memset(impl_->icd2.r7800_snapshot, 0xFF, sizeof impl_->icd2.r7800_snapshot);
	// $7000-$700F latch buffer starts as $FF so reads before the first
	// $6002 pop return all-ones (matches bsnes r7000 power-on state).
	std::memset(impl_->icd2.r7000_buf, 0xFF, sizeof impl_->icd2.r7000_buf);
	// Init read_slice to 17 so the first $6001 write advances it to
	// 0 (slice 0 — what the BIOS expects on its first drain).
	impl_->icd2.read_slice = 17;
	// Joypad registers idle = $FF (active-low, no buttons held).
	// bsnes r6004-r6007 = 0xff. Initializing to 0 makes the GB see all
	// buttons held and the SGB BIOS's probe sequences fail. Critical.
	impl_->icd2.joypad[0] = 0xFF;
	impl_->icd2.joypad[1] = 0xFF;
	impl_->icd2.joypad[2] = 0xFF;
	impl_->icd2.joypad[3] = 0xFF;

	impl_->mem.ppu    = &impl_->ppu;
	impl_->mem.apu    = &impl_->apu;
	impl_->mem.timer  = &impl_->timer;
	impl_->mem.joypad = &impl_->joypad;
	impl_->mem.cart   = &impl_->cart;

	impl_->fb.pixels = impl_->ppu.framebuffer;
	impl_->fb.width  = GB_SCREEN_WIDTH;
	impl_->fb.height = GB_SCREEN_HEIGHT;
	impl_->fb.pitch  = GB_SCREEN_WIDTH;

	// Apply run-mode specific post-boot register values. The SGB BIOS
	// hands control to the cart with slightly different register state
	// than a DMG boot ROM does — some games (notably Donkey Kong and
	// Pokemon) check these to detect whether they're running on a real
	// SGB host.
	CpuState &cs = impl_->cpu.State();
	switch (impl_->run_mode)
	{
		case RunMode::SGB:
			cs.r.af = 0x0100;
			cs.r.bc = 0x0014;
			cs.r.de = 0x0000;
			cs.r.hl = 0xC060;
			break;
		case RunMode::SGB2:
			cs.r.af = 0xFF00;
			cs.r.bc = 0x0014;
			cs.r.de = 0x0000;
			cs.r.hl = 0xC060;
			break;
		case RunMode::DMG:
		default:
			// gb_cpu.cpp Reset() already set DMG values.
			break;
	}

	// If a GB-side boot ROM was staged (authentic BIOS mode), overlay it
	// at 0x0000-0x00FF and start the CPU there. The boot code will scroll
	// the Nintendo logo, send the 5-packet SGB handshake the BIOS is
	// waiting for, then write 0xFF50 to disable itself — at which point
	// the cart takes over exactly as it would on real hardware.
	if (impl_->boot_rom_loaded)
	{
		std::memcpy(impl_->mem.boot_rom, impl_->boot_rom_staging, sizeof impl_->mem.boot_rom);
		impl_->mem.boot_rom_enabled = true;
		cs.r.af = 0x0000;
		cs.r.bc = 0x0000;
		cs.r.de = 0x0000;
		cs.r.hl = 0x0000;
		cs.r.sp = 0x0000;
		cs.r.pc = 0x0000;
	}
}

bool Emulator::LoadBootROM(const uint8_t *data, size_t size)
{
	if (!data || size == 0)
	{
		impl_->boot_rom_loaded = false;
		return true;
	}
	if (size != sizeof impl_->boot_rom_staging) return false;
	std::memcpy(impl_->boot_rom_staging, data, size);
	impl_->boot_rom_loaded = true;
	return true;
}

void Emulator::PrimeBIOSHandshake()
{
	if (!impl_->has_rom || impl_->cart.rom.size() < 0x150) return;

	Emulator::Impl::Icd2 &icd = impl_->icd2;
	const std::vector<uint8_t> &rom = impl_->cart.rom;

	// Real SGB boot ROM handshake sends 6 packets with byte 0 cycling
	// through $F1, $F3, $F5, $F7, $F9, $FB (low 3 bits encode a +2
	// packet index that overflows the cmd_id nibble on the 5th step:
	// cmd $1E idx 1/3/5/7 → $1F idx 1/3). The BIOS at $BE66/$BE69
	// verifies the first packet's byte 0 against #$F1 and counts each
	// subsequent packet into $02C0 at $BE75. Observed live from our
	// own boot ROM capture: b0s = F1 F3 F5 F7 F9 FB, exactly.
	// Bytes 1..15 are successive 15-byte slices of the cart header
	// starting at $0104 (Nintendo logo bytes → title → cart-type → etc).
	static const uint8_t kHeaderByte0[6] = { 0xF1, 0xF3, 0xF5, 0xF7, 0xF9, 0xFB };
	for (int p = 0; p < 6; ++p)
	{
		icd.synth_packets[p][0] = kHeaderByte0[p];
		for (int b = 1; b < 16; ++b)
		{
			const size_t off = 0x0104 + p * 15 + (b - 1);
			icd.synth_packets[p][b] = (off < rom.size()) ? rom[off] : 0x00;
		}
	}

	// Populate the synth queue but DO NOT stage into the FIFO yet. The
	// BIOS's splash-animation code on the SNES side might drain pending
	// packets via its own paths before it reaches the handshake wait at
	// $BE3C. Staging too early means our packets get eaten before the
	// handshake counter ever increments. Wait for the BIOS to release
	// the GB (write $6003 bit 7) — that's the real-hardware signal that
	// the BIOS is ready to see handshake packets.
	icd.synth_remaining = 6;     // 6 packets queued, none staged yet
	icd.drain_ptr       = 0;
}

// Push a freshly-assembled 16-byte packet onto the queue. Drops the
// oldest slot silently if the queue is full — matches bsnes icd.cpp
// behavior (`if(packetSize >= 64) packetSize = 64;`). Bumps the
// diagnostic counters as the canonical "packet arrived" event.
static void IcdPushQueue(Emulator::Impl::Icd2 &icd, const uint8_t *pkt)
{
	std::memcpy(icd.packet_queue[icd.queue_tail], pkt, 16);
	icd.queue_tail = static_cast<uint8_t>((icd.queue_tail + 1) & 63);
	if (icd.queue_count < 64)
		icd.queue_count++;
	else
		icd.queue_head = static_cast<uint8_t>((icd.queue_head + 1) & 63);

	icd.packets_received++;
	const uint8_t byte0  = pkt[0];
	const uint8_t cmd_id = static_cast<uint8_t>(byte0 >> 3);
	if (byte0 == 0xF1) icd.f1_packets++;
	if (!icd.first_packet_seen)
	{
		icd.first_packet_byte0 = byte0;
		icd.first_packet_seen  = true;
		std::memcpy(icd.pkt0_bytes, pkt, 16);
	}
	if (icd.byte0_log_len < 8)
		icd.byte0_log[icd.byte0_log_len++] = byte0;
	if (icd.last_cmd_ids_len < 8)
		icd.last_cmd_ids[icd.last_cmd_ids_len++] = cmd_id;
	else
	{
		for (int i = 0; i < 7; ++i)
			icd.last_cmd_ids[i] = icd.last_cmd_ids[i + 1];
		icd.last_cmd_ids[7] = cmd_id;
	}
}

static void IcdStageNextSynth(Emulator::Impl::Icd2 &icd)
{
	if (icd.synth_remaining == 0) return;
	const uint8_t next_idx = static_cast<uint8_t>(6 - icd.synth_remaining);
	IcdPushQueue(icd, icd.synth_packets[next_idx]);
	icd.synth_remaining--;
}

bool Emulator::LoadROM(const uint8_t *data, size_t size, const char *path)
{
	if (!CartLoad(impl_->cart, data, size, path))
		return false;
	impl_->has_rom = true;
	ColdReset();   // new cart → start fresh, drop any stale handshake cache
	return true;
}

void Emulator::UnloadROM()
{
	if (impl_->has_rom) CartSaveBattery(impl_->cart);
	CartUnload(impl_->cart);
	impl_->has_rom = false;
	// Drop any staged boot ROM so a subsequent BIOS-less load starts at
	// $0100 with the normal post-boot register state.
	impl_->boot_rom_loaded = false;
}

bool Emulator::HasROM() const { return impl_->has_rom; }

void Emulator::SetRunMode(RunMode m)
{
	if (m == impl_->run_mode) return;  // idempotent — avoids re-pushing clock
	impl_->run_mode = m;
	// SGB1 runs the GB at SNES_master / 5 = 4.295455 MHz (~2.4% faster
	// than DMG); SGB2 and DMG both run at the authentic 4.194304 MHz.
	// The APU uses this to keep cycles_per_sample correct so audio plays
	// at the right pitch in every mode.
	const int32_t clock = (m == RunMode::SGB) ? 4295455 : 4194304;
	ApuSetClockHz(impl_->apu, clock);
}
RunMode Emulator::GetRunMode() const { return impl_->run_mode; }

void Emulator::RunFrame()
{
	if (!impl_->has_rom) return;

	impl_->ppu.frame_ready = false;

	// Cycle budget per SNES frame depends on run mode and the user
	// overclock/underclock knob:
	//   SGB1: SNES master clock / 5 = 4.2955 MHz (2.4% faster than real GB)
	//   SGB2: exact GB clock         = 4.1943 MHz
	//   DMG:  exact GB clock         = 4.1943 MHz
	// At NTSC SNES refresh (60.099 fps) that's ~71485 / 69801 T-cycles per
	// SNES frame. Clamp the multiplier to a sane range so users can't
	// accidentally freeze the emulator with a 0x or 1000x setting.
	constexpr double SNES_FPS = 60.09881389744051;
	double base_hz;
	switch (impl_->run_mode)
	{
		case RunMode::SGB:  base_hz = 21477272.727272 / 5.0; break;
		case RunMode::SGB2: base_hz = 4194304.0;             break;
		case RunMode::DMG:
		default:            base_hz = 4194304.0;             break;
	}

	float mul = impl_->clock_mul;
	if (mul < 0.10f) mul = 0.10f;
	if (mul > 8.00f) mul = 8.00f;

	const double   per_frame = (base_hz / SNES_FPS) * static_cast<double>(mul);
	const int32_t  budget    = static_cast<int32_t>(per_frame);

	RunCycles(budget);
}

void Emulator::RunCycles(int32_t tcycles)
{
	if (!impl_->has_rom) return;

	// Partial-frame advance. Used when a host driving its own clock wants
	// to interleave GB execution with other work at finer granularity
	// than one frame. Same per-step ticking as RunFrame.
	int32_t remaining = tcycles;
	while (remaining > 0)
	{
		const int64_t pre_t = impl_->cpu.State().t_cycles;
		impl_->cpu.Step(impl_->mem);
		int32_t consumed = static_cast<int32_t>(
			impl_->cpu.State().t_cycles - pre_t);
		if (consumed <= 0) consumed = 4;

		PpuStep  (impl_->ppu,   impl_->mem, consumed);
		TimerStep(impl_->timer, impl_->mem, consumed);
		ApuStep  (impl_->apu,                consumed);

		remaining -= consumed;
	}
}

const FrameBuffer &Emulator::GetFrameBuffer() const { return impl_->fb; }

void Emulator::GetStatus(char *buf, size_t cap) const
{
	if (!buf || cap == 0) return;
	const CpuState &s = impl_->cpu.State();
	const Emulator::Impl::Icd2 &icd = impl_->icd2;

	const uint8_t *p0 = icd.pkt0_bytes;
	const uint8_t *b0 = icd.byte0_log;
	const uint8_t *rv = icd.last_r7000_vals;
	std::snprintf(buf, cap,
	              "GBPC=%04X ctrl=%02X pkts=%u F1=%u ly=%u "
	              "b0s=%02X%02X%02X%02X%02X%02X "
	              "p0=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
	              "r7000_ring=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
	              "R:6002=%u 7000=%u W:6001=%u 6003=%u",
	              s.r.pc, icd.control,
	              icd.packets_received,
	              icd.f1_packets,
	              static_cast<unsigned>(impl_->ppu.ly),
	              b0[0], b0[1], b0[2], b0[3], b0[4], b0[5],
	              p0[0],  p0[1],  p0[2],  p0[3],  p0[4],  p0[5],  p0[6],  p0[7],
	              p0[8],  p0[9],  p0[10], p0[11], p0[12], p0[13], p0[14], p0[15],
	              rv[0],  rv[1],  rv[2],  rv[3],  rv[4],  rv[5],  rv[6],  rv[7],
	              rv[8],  rv[9],  rv[10], rv[11], rv[12], rv[13], rv[14], rv[15],
	              icd.r_6002, icd.r_7000,
	              icd.w_6001, icd.w_6003);
}

// ICD2 register mirrors repeat every 16 bytes across each kB window
// ($6000-$67FF, $7000-$77FF, $7800-$7FFF). Matches real SGB hardware.
static inline uint16_t Icd2Mask(uint16_t addr) { return addr & 0xF80F; }

uint8_t Emulator::GetICD2(uint16_t addr)
{
	if (!impl_) return 0xFF;
	const uint16_t a = Icd2Mask(addr);
	Emulator::Impl::Icd2 &icd = impl_->icd2;
	icd.last_read_addr = a;
	switch (a)
	{
		case 0x6000: icd.r_6000++; break;
		case 0x6002: icd.r_6002++; break;
		case 0x6003: icd.r_6003++; break;
		case 0x600F: icd.r_600F++; break;
		default:
			if      (a >= 0x7000 && a <= 0x700F) icd.r_7000++;
			else if (a >= 0x7800 && a <= 0x780F) icd.r_7800++;
			break;
	}

	// $7000-$700F — bsnes-style latch read. Returns bytes from r7000_buf,
	// which was populated on the last $6002 read that found a pending
	// packet. This lets the BIOS read any subset of the 16 bytes (or
	// re-read them) without advancing the queue.
	if (a >= 0x7000 && a <= 0x700F)
	{
		const uint8_t b = icd.r7000_buf[a & 0x0F];
		// Trace: log into ring so OSD can see what our ICD2 actually
		// served for the last drain attempts.
		icd.last_r7000_vals[icd.last_r7000_idx & 0x0F] = b;
		icd.last_r7000_idx++;
		return b;
	}
	// $7800-$780F — GB frame char-transfer window. Streams bit-plane
	// bytes from the live 4-bank capture ring (lcd_ring), selected by
	// lcd_row_select. Layout per bank: 20 tiles × 8 rows × 2 planes =
	// 320 bytes of real data, then $FF padding to 512 before wrap.
	// Reading from the live ring (not the end-of-frame framebuffer)
	// is what lets the BIOS capture the boot animation's Nintendo logo
	// scroll — the BIOS reads $7800 while the GB is still mid-drawing.
	if (a >= 0x7800 && a <= 0x780F)
	{
		const uint16_t pos = icd.read_position;
		icd.read_position  = static_cast<uint16_t>((icd.read_position + 1) & 0x1FF);
		if (pos >= 320)
			return 0xFF;

		const uint8_t tile_col = static_cast<uint8_t>(pos / 16);    // 0..19
		const uint8_t byte_in  = static_cast<uint8_t>(pos & 0x0F);
		const uint8_t row_in   = static_cast<uint8_t>(byte_in >> 1); // 0..7
		const uint8_t plane    = static_cast<uint8_t>(byte_in & 1);

		// Read from the full 18-slice frame buffer indexed by the
		// BIOS's $6001-write sequence counter (read_slice). Each
		// $6001 write advances read_slice by 1 (mod 18), so the
		// BIOS's drain pattern (slice 0, 1, …, 17, wrap) maps
		// directly to the right slice in full_frame.
		const uint8_t slice    = icd.read_slice % 18;
		const uint8_t *px      = &icd.full_frame[slice][row_in * 160 + tile_col * 8];

		uint8_t b = 0;
		for (int i = 0; i < 8; ++i)
		{
			const uint8_t palidx = static_cast<uint8_t>(px[i] & 0x03);
			const uint8_t bit    = static_cast<uint8_t>((palidx >> plane) & 1);
			b = static_cast<uint8_t>(b | (bit << (7 - i)));
		}
		return b;
	}

	switch (a)
	{
		case 0x6000:
			// bsnes `io.cpp`: `vcounter & ~7 | writeBank` — the low bits
			// are the latched $6001 bank value (the BIOS's *request*),
			// NOT our scanline-derived sgb_bank. Before the BIOS writes
			// $6001 (early handshake) these bits must read zero, matching
			// real hardware / bsnes. Upper bits are GB LY masked — includes
			// VBlank lines 144..153, so during VBlank $6000 = 0x90 | bank.
			return static_cast<uint8_t>((impl_->ppu.ly & 0xF8) | (icd.lcd_row_select & 0x03));
		case 0x6002:
		{
			// Lazy-stage the next synth packet if queue's empty (no-op
			// when a real boot ROM is running — synth_remaining stays 0).
			if (icd.queue_count == 0 && icd.synth_remaining > 0)
				IcdStageNextSynth(icd);

			// Auto-stage cached packets if queue is empty and we have a
			// valid cache from the first boot. Without this, the BIOS
			// eventually enters sub_80B107 with empty queue and spins
			// on $6002 forever (it doesn't write $6003 inside that
			// loop, so our release-triggered replay can't fire).
			//
			// Cap the count so the BIOS can eventually EXIT the
			// validate-and-reset cycle. Staging indefinitely keeps the
			// BIOS forever re-running handshake validation, which
			// interferes with game-display tile transfers — garbled
			// CHR_TRN output. 50 iterations is enough for the BIOS to
			// complete splash setup and advance $0101 to game-display
			// mode; after that the game's own SGB commands drive the
			// packet queue and validation is no longer needed.
			if (icd.queue_count == 0 && impl_->cache_valid &&
			    impl_->replays_done < 50)
			{
				for (int p = 0; p < 6; ++p)
					IcdPushQueue(icd, impl_->cached_packets[p]);
				impl_->replays_done++;
			}

			// bsnes semantics: reading $6002 with a pending packet POPS
			// the front packet into r7000_buf[0..15] and shifts the queue.
			if (icd.queue_count > 0)
			{
				std::memcpy(icd.r7000_buf, icd.packet_queue[icd.queue_head], 16);
				icd.queue_head = static_cast<uint8_t>((icd.queue_head + 1) & 63);
				icd.queue_count--;
				IcdStageNextSynth(icd);
				return 0x01;
			}
			return 0x00;
		}
		case 0x6003: return icd.control;    // R/W — some BIOS paths verify writes
		case 0x600F: return 0x21;           // BIOS version byte (bsnes / Mesen return $21)
	}
	return 0x00;  // bsnes readIO falls through to 0 for unmapped ICD2 addrs
}

void Emulator::SetICD2(uint8_t value, uint16_t addr)
{
	if (!impl_) return;
	const uint16_t a = Icd2Mask(addr);
	Emulator::Impl::Icd2 &icd = impl_->icd2;
	icd.last_write_addr = a;
	icd.last_write_val  = value;
	switch (a)
	{
		case 0x6000: icd.w_6000++; break;
		case 0x6001: icd.w_6001++; break;
		case 0x6003: icd.w_6003++; break;
		case 0x6004: icd.w_6004++; break;
		default:
			if (a >= 0x7000 && a <= 0x700F) icd.w_7000++;
			break;
	}

	switch (a)
	{
		case 0x6001:
			icd.lcd_row_select = value;
			icd.read_position  = 0;
			// Advance the slice-read counter. The BIOS issues $6001
			// writes in order: slice 0, 1, …, 17, then wraps. We use
			// THIS counter (not the bank field) to index the full_frame
			// buffer, so each $7800 stream serves the correct slice
			// regardless of bank-wrap.
			icd.read_slice = static_cast<uint8_t>((icd.read_slice + 1) % 18);
			icd.slice_index = static_cast<uint8_t>((icd.slice_index + 1) % 18);
			icd.row_writes++;
			return;
		case 0x6003:
		{
			// Match bsnes `io.cpp`: on 0→1 transition of bit 7, the GB
			// is POWER-ON-RESET. On 1→0 we just freeze the GB in reset.
			const bool was_released = (icd.control & 0x80) != 0;
			const bool now_released = (value & 0x80) != 0;
			icd.ctrl_writes++;

			icd.control = value;

			if (!was_released && now_released)
			{
				if (!impl_->cache_valid)
				{
					// First release: fresh GB boot. Reset() zeroes icd2,
					// reloads the boot ROM, sets PC=$0000. The GB's boot
					// ROM will bit-bang 6 packets, which OnJoyserWrite
					// caches into impl_->cached_packets as they arrive.
					Reset();
					icd.control = value;  // re-apply after Reset wiped it
					if (!impl_->boot_rom_loaded)
						PrimeBIOSHandshake();
				}
				else if (icd.queue_count == 0)
				{
					// Subsequent release with an EMPTY queue — BIOS is
					// expecting handshake packets to validate. Replay
					// cached packets so $B119 passes and the state
					// machine can advance $0102 through its ~41 sub-
					// states (at which point $B0CD sets $0101=5 and
					// $808EA0 jumps to game-mode display).
					// Gate on queue_count==0 so we don't wipe any
					// game-generated packets that are already queued —
					// only replay when the BIOS actually needs packets.
					for (int p = 0; p < 6; ++p)
						IcdPushQueue(icd, impl_->cached_packets[p]);
					impl_->replays_done++;
				}
				// Non-empty queue means game packets are pending —
				// leave them alone and let the BIOS drain/process them.
			}
			else if (was_released && !now_released)
			{
				// Entering reset: freeze row/bank so $6000 reads match
				// the fresh GB state the BIOS sees on the next release.
				icd.sgb_row  = 0;
				icd.sgb_bank = 0;
			}
			return;
		}
		case 0x6004:
		{
			icd.joypad[0] = value;
			// SGB $6004 joypad-mirror format (verified against the BIOS's
			// own bit-shuffle in sub_80BCDE — see SGB2.sfc.lst:12372+).
			// Active-LOW. The BIOS writes:
			//   bit 0: RIGHT     bit 4: A
			//   bit 1: LEFT      bit 5: B
			//   bit 2: UP        bit 6: SELECT
			//   bit 3: DOWN      bit 7: START
			// Earlier comment said the opposite (dpad in upper nibble) —
			// that produced "press A → character moves right" because we
			// interpreted bit 4 as Right.
			uint8_t mask = 0;
			if (!(value & 0x01)) mask |= GB_RIGHT;
			if (!(value & 0x02)) mask |= GB_LEFT;
			if (!(value & 0x04)) mask |= GB_UP;
			if (!(value & 0x08)) mask |= GB_DOWN;
			if (!(value & 0x10)) mask |= GB_A;
			if (!(value & 0x20)) mask |= GB_B;
			if (!(value & 0x40)) mask |= GB_SELECT;
			if (!(value & 0x80)) mask |= GB_START;
			JoypadSet(impl_->joypad, impl_->mem, mask);
			return;
		}
		case 0x6005: icd.joypad[1]      = value;                                           return;
		case 0x6006: icd.joypad[2]      = value;                                           return;
		case 0x6007: icd.joypad[3]      = value;                                           return;
	}
}

bool Emulator::IsGBReleased() const
{
	if (!impl_) return false;
	return (impl_->icd2.control & 0x80) != 0;
}

bool Emulator::IsHandshakePending() const
{
	if (!impl_) return false;
	return impl_->icd2.synth_remaining > 0 || impl_->icd2.queue_count > 0;
}

void Emulator::OnPpuHBlank()
{
	if (!impl_) return;
	Emulator::Impl::Icd2& icd = impl_->icd2;

	// Restored normal mapping. Trying a different empirical tweak elsewhere.
	icd.sgb_row = impl_->ppu.ly;
	icd.sgb_bank = (icd.sgb_row / 8) & 0x03;
}

void Emulator::OnPpuVBlank()
{
	if (!impl_) return;
	// Reset row and bank at VBlank — see earlier comment for why both.
	impl_->icd2.sgb_row  = 0;
	impl_->icd2.sgb_bank = 0;

	// Force-realign read_slice to 16 per frame so the first $6001 write
	// of the next frame always advances to slice 17 (the wrap-read).
	// Without this, drift accumulates if any frame has !=18 $6001 writes
	// (e.g., during handshake transitions or BIOS internal scheduling
	// variation), producing the slow vertical scroll-up of the entire
	// composited image.
	impl_->icd2.read_slice = 16;
}

void Emulator::CaptureScanline(const uint8_t *pixels)
{
	if (!impl_ || !pixels) return;
	Emulator::Impl::Icd2 &icd = impl_->icd2;
	const uint8_t bank  = static_cast<uint8_t>(icd.sgb_bank & 0x03);
	const uint8_t row   = static_cast<uint8_t>(icd.sgb_row  & 0x07);
	const uint8_t slice = static_cast<uint8_t>((icd.sgb_row / 8) % 18);

	// Legacy 4-bank ring (kept for state-save compatibility).
	std::memcpy(&icd.lcd_ring[bank][row * 160], pixels, 160);
	// Full 18-slice buffer — what $7800 actually reads from.
	if (slice < 18)
		std::memcpy(&icd.full_frame[slice][row * 160], pixels, 160);
}

void Emulator::BlitScreen(uint16_t *dest, uint32_t pitch_pixels)
{
	if (!impl_->has_rom || !dest) return;

	// Stage border into our heap-resident 256 × 224 buffer. Border
	// leaves the centered 20 × 18 tile area untouched — we overwrite
	// that next with palette-resolved GB pixels.
	uint16_t *const staging = impl_->composite;
	SgbRenderBorder(impl_->sgb_state, staging);

	// Pick the source pixels for the GB screen area based on MASK_EN.
	const uint8_t *src_fb = impl_->ppu.framebuffer;
	if (impl_->sgb_state.mask_mode == SGB_MASK_FREEZE &&
	    impl_->sgb_state.frozen_frame_valid)
	{
		src_fb = impl_->sgb_state.frozen_frame;
	}

	const uint32_t origin_x = SGB_GB_TILE_X * 8;  // 48
	const uint32_t origin_y = SGB_GB_TILE_Y * 8;  // 40

	for (uint32_t py = 0; py < GB_SCREEN_HEIGHT; ++py)
	{
		const uint32_t dst_y     = origin_y + py;
		uint16_t *const dst_row  = staging + dst_y * SGB_BORDER_W + origin_x;
		for (uint32_t px = 0; px < GB_SCREEN_WIDTH; ++px)
		{
			uint16_t color;
			switch (impl_->sgb_state.mask_mode)
			{
				case SGB_MASK_BLACK:
					color = 0x0000;
					break;
				case SGB_MASK_BLANK:
					color = impl_->sgb_state.active[0].colors[0];
					break;
				default:
				{
					const uint8_t  shade   = src_fb[py * GB_SCREEN_WIDTH + px];
					const uint32_t tile_x  = px / 8;
					const uint32_t tile_y  = py / 8;
					color = SgbResolveColor(impl_->sgb_state, tile_x, tile_y, shade);
					break;
				}
			}
			dst_row[px] = color;
		}
	}

	// Copy to destination with pitch. For a packed 256-wide dest this
	// degenerates into a flat memcpy per row.
	for (uint32_t y = 0; y < SGB_BORDER_H; ++y)
	{
		std::memcpy(dest + y * pitch_pixels,
		            staging + y * SGB_BORDER_W,
		            SGB_BORDER_W * sizeof(uint16_t));
	}
}

int32_t Emulator::DrainAudio(int16_t *out, int32_t max_samples)
{
	return ApuDrain(impl_->apu, out, max_samples);
}

int32_t Emulator::GetAudioSampleRate() const
{
	return impl_->apu.output_rate;
}

int32_t Emulator::GetAudioClockHz() const
{
	return impl_->apu.clock_hz;
}

int32_t Emulator::GetAudioCyclesPerSample() const
{
	return impl_->apu.cycles_per_sample;
}

int32_t Emulator::GetAudioCpsRemainderStep() const
{
	return impl_->apu.cps_remainder_step;
}

int32_t Emulator::GetAudioSamplesAvailable() const
{
	const uint32_t head = impl_->apu.sample_head;
	const uint32_t tail = impl_->apu.sample_tail;
	const uint32_t frames = (head >= tail)
		? (head - tail)
		: (APU_SAMPLE_BUF_SIZE - tail + head);
	// Each frame is one stereo pair = 2 int16 values.
	return static_cast<int32_t>(frames * 2);
}

void Emulator::SetAudioRate(int32_t rate_hz)
{
	ApuSetOutputRate(impl_->apu, rate_hz);
}

void Emulator::SetJoypad(uint16_t snes_pad_mask)
{
	// SNES->GB button mapping. B/Y map to A/B (SNES has extra shoulders & face buttons).
	uint8_t gb = 0;
	if (snes_pad_mask & (1 << 15)) gb |= GB_A;       // SNES B  → GB A
	if (snes_pad_mask & (1 << 14)) gb |= GB_B;       // SNES Y  → GB B
	if (snes_pad_mask & (1 << 12)) gb |= GB_START;
	if (snes_pad_mask & (1 << 13)) gb |= GB_SELECT;
	if (snes_pad_mask & (1 << 11)) gb |= GB_UP;
	if (snes_pad_mask & (1 << 10)) gb |= GB_DOWN;
	if (snes_pad_mask & (1 <<  9)) gb |= GB_LEFT;
	if (snes_pad_mask & (1 <<  8)) gb |= GB_RIGHT;
	JoypadSet(impl_->joypad, impl_->mem, gb);
}

// ICD2 packet decoder. The GB drives $FF00 bits 4/5 in four states:
//   $00 — reset pulse: start a new packet
//   $10 — 1-bit
//   $20 — 0-bit
//   $30 — idle / clock-high
// Bits accumulate LSB-first into a byte, then into assembly_buf[0..15]
// which is pushed onto the packet queue on the 16th byte.
// A rising edge on P15 (bit 5) while NOT in a packet advances the MLT_REQ
// player index (see Pan Docs SGB multi-player handshake).
static void IcdFeedJoypad(Emulator::Impl::Icd2 &icd, uint8_t value)
{
	const uint8_t sel = value & 0x30;

	// Player-select edge detection fires only between packets — during
	// packet assembly these same transitions encode data bits.
	if (!icd.in_packet)
	{
		const bool p15_rose = !(icd.input_value & 0x20) && (value & 0x20);
		if (p15_rose)
			icd.input_index = static_cast<uint8_t>((icd.input_index + 1) & 0x03);
	}
	icd.input_value = value;

	if (sel == 0x00)
	{
		// Reset pulse — arm the packet assembler.
		icd.in_packet        = true;
		icd.packet_byte      = 0;
		icd.packet_bit       = 0;
		icd.bit_accumulator  = 0;
		return;
	}

	if (!icd.in_packet) return;
	if (sel != 0x10 && sel != 0x20) return;  // $30 (idle) doesn't latch a bit

	const uint16_t bit = (sel == 0x10) ? 1u : 0u;
	icd.bit_accumulator |= static_cast<uint16_t>(bit << icd.packet_bit);
	icd.packet_bit++;

	if (icd.packet_bit >= 8)
	{
		if (icd.packet_byte < 16)
			icd.assembly_buf[icd.packet_byte] = static_cast<uint8_t>(icd.bit_accumulator);
		icd.packet_byte++;
		icd.packet_bit      = 0;
		icd.bit_accumulator = 0;

		if (icd.packet_byte >= 16)
		{
			icd.in_packet = false;
			// Push the fully-assembled packet onto the queue. Counter
			// bookkeeping (packets_received, last_cmd_ids, f1_packets)
			// is handled inside IcdPushQueue for consistency with the
			// synth-staging path.
			IcdPushQueue(icd, icd.assembly_buf);
		}
	}
}

void Emulator::OnJoyserWrite(uint8_t value)
{
	// Only meaningful when the cart declares SGB features. Feeding always
	// is harmless (non-SGB games don't produce RESET pulses) but we gate
	// on run_mode anyway so the packet state doesn't accumulate noise.
	if (impl_->run_mode == RunMode::DMG) return;

	// BIOS-less path — our internal packet assembler fires the dispatch
	// callback into sgb_state.cpp (palettes / border / mask).
	PacketFeed(impl_->sgb_pkt, value);

	// BIOS-mode path — independent decoder that parks completed packets
	// in the single-slot ICD2 FIFO. Harmless when no BIOS is running
	// (nothing reads $7000-$700F).
	const uint32_t pre_received = impl_->icd2.packets_received;
	IcdFeedJoypad(impl_->icd2, value);
	// If a new packet completed (packets_received grew), and our cache
	// isn't full yet, append a copy. The SGB2 BIOS's handshake validator
	// at $B119 compares each packet's byte 1 across TWO separate GB
	// resets ($7E:1718 is the reference, populated by $BFD3 from a prior
	// iteration's packets). If our re-booted GB produces byte-different
	// packets, validation fails and $02FA=1 triggers cart error. Caching
	// the first 6 real packets lets us re-queue byte-identical copies on
	// subsequent releases, guaranteeing the BIOS's byte-1 reference check
	// passes.
	if (impl_->icd2.packets_received > pre_received &&
	    impl_->cached_count < 6)
	{
		std::memcpy(impl_->cached_packets[impl_->cached_count],
		            impl_->icd2.assembly_buf, 16);
		impl_->cached_count++;
		if (impl_->cached_count == 6) impl_->cache_valid = true;
	}
}

void Emulator::OnSgbCommandInternal(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	// *_TRN commands source their 4KB from GB VRAM $8000..$8FFF — our
	// first half of Ppu::vram. MASK_EN freeze also needs the current
	// GB framebuffer. Non-consuming commands ignore both pointers.
	SgbHandleCommand(impl_->sgb_state, cmd, data, len,
	                 impl_->ppu.vram,
	                 impl_->ppu.framebuffer);
}

// ===================================================================
// State serialization
//
// Layout:
//   [0..3]   magic "SGB!"
//   [4..7]   version (u32 LE)
//   [8..11]  payload length (u32 LE)
//   [12..]   payload fields in Visit() order
//
// Version 1: initial format.
// ===================================================================

namespace {

constexpr uint32_t SGB_STATE_MAGIC   = 0x21424753u;  // 'S''G''B''!' LE
constexpr uint32_t SGB_STATE_VERSION = 1;

enum class IoMode : uint8_t { Size, Save, Load };

struct IoCtx
{
	uint8_t       *wbuf;
	const uint8_t *rbuf;
	size_t         pos;
	size_t         cap;
	IoMode         mode;
	bool           ok;
};

inline void IoBytes(IoCtx &c, void *data, size_t n)
{
	if (!c.ok) return;
	if (c.mode != IoMode::Size && c.pos + n > c.cap) { c.ok = false; return; }
	if      (c.mode == IoMode::Save) std::memcpy(c.wbuf + c.pos, data, n);
	else if (c.mode == IoMode::Load) std::memcpy(data, c.rbuf + c.pos, n);
	c.pos += n;
}

template <typename T>
inline void IoField(IoCtx &c, T &v)
{
	IoBytes(c, &v, sizeof v);
}

void VisitState(Emulator::Impl &impl, IoCtx &c)
{
	// ----- CPU -----
	IoField(c, impl.cpu.State());

	// ----- Memory (skip pointer fields — they're relinked after load) -----
	IoBytes(c, impl.mem.wram, sizeof impl.mem.wram);
	IoBytes(c, impl.mem.hram, sizeof impl.mem.hram);
	IoField(c, impl.mem.ie);
	IoField(c, impl.mem.if_);
	IoField(c, impl.mem.serial_data);

	// ----- Cart MBC + SRAM (ROM is static, not serialized) -----
	IoField(c, impl.cart.mbc);

	uint32_t sram_size = static_cast<uint32_t>(impl.cart.sram.size());
	IoField(c, sram_size);
	if (c.mode == IoMode::Load) impl.cart.sram.resize(sram_size);
	if (sram_size > 0) IoBytes(c, impl.cart.sram.data(), sram_size);

	// ----- PPU -----
	IoBytes(c, impl.ppu.vram, sizeof impl.ppu.vram);
	IoBytes(c, impl.ppu.oam,  sizeof impl.ppu.oam);
	IoField(c, impl.ppu.lcdc);
	IoField(c, impl.ppu.stat);
	IoField(c, impl.ppu.scy);
	IoField(c, impl.ppu.scx);
	IoField(c, impl.ppu.ly);
	IoField(c, impl.ppu.lyc);
	IoField(c, impl.ppu.bgp);
	IoField(c, impl.ppu.obp0);
	IoField(c, impl.ppu.obp1);
	IoField(c, impl.ppu.wy);
	IoField(c, impl.ppu.wx);
	IoField(c, impl.ppu.mode);
	IoField(c, impl.ppu.mode_clock);
	IoField(c, impl.ppu.window_line);
	IoField(c, impl.ppu.stat_line_high);
	IoBytes(c, impl.ppu.framebuffer,      sizeof impl.ppu.framebuffer);
	IoBytes(c, impl.ppu.scanline_bg_raw,  sizeof impl.ppu.scanline_bg_raw);
	IoField(c, impl.ppu.frame_ready);

	// ----- APU channels + master regs + frame sequencer -----
	// (sample buffer + accumulators are transient and reset on load.)
	IoField(c, impl.apu.ch1);
	IoField(c, impl.apu.ch2);
	IoField(c, impl.apu.ch3);
	IoField(c, impl.apu.ch4);
	IoField(c, impl.apu.nr50);
	IoField(c, impl.apu.nr51);
	IoField(c, impl.apu.master_enabled);
	IoField(c, impl.apu.frame_seq_timer);
	IoField(c, impl.apu.frame_seq_step);

	// ----- Timer / Joypad -----
	IoField(c, impl.timer);
	IoField(c, impl.joypad);

	// ----- SGB command layer -----
	IoField(c, impl.sgb_pkt);
	IoField(c, impl.sgb_state);

	// ----- Emulator-level config -----
	IoField(c, impl.run_mode);
	IoField(c, impl.clock_mul);
}

} // anonymous

size_t Emulator::StateSize() const
{
	IoCtx c{nullptr, nullptr, 0, 0, IoMode::Size, true};
	VisitState(const_cast<Impl &>(*impl_), c);
	// +12 for header: magic + version + payload_size.
	return c.pos + 12;
}

void Emulator::StateSave(uint8_t *buffer) const
{
	if (!buffer) return;

	// Compute payload size first.
	const size_t payload = StateSize() - 12;

	uint32_t magic   = SGB_STATE_MAGIC;
	uint32_t version = SGB_STATE_VERSION;
	uint32_t plen    = static_cast<uint32_t>(payload);
	std::memcpy(buffer + 0, &magic,   4);
	std::memcpy(buffer + 4, &version, 4);
	std::memcpy(buffer + 8, &plen,    4);

	IoCtx c{buffer + 12, nullptr, 0, payload, IoMode::Save, true};
	VisitState(const_cast<Impl &>(*impl_), c);
}

bool Emulator::StateLoad(const uint8_t *buffer, size_t size)
{
	if (!buffer || size < 12) return false;

	uint32_t magic, version, plen;
	std::memcpy(&magic,   buffer + 0, 4);
	std::memcpy(&version, buffer + 4, 4);
	std::memcpy(&plen,    buffer + 8, 4);

	if (magic != SGB_STATE_MAGIC)     return false;
	if (version != SGB_STATE_VERSION) return false;  // future: accept v<=current
	if (size < 12 + plen)             return false;

	IoCtx c{nullptr, buffer + 12, 0, plen, IoMode::Load, true};
	VisitState(*impl_, c);
	if (!c.ok) return false;

	// Relink Memory's pointer fields — they were serialized as garbage.
	impl_->mem.ppu    = &impl_->ppu;
	impl_->mem.apu    = &impl_->apu;
	impl_->mem.timer  = &impl_->timer;
	impl_->mem.joypad = &impl_->joypad;
	impl_->mem.cart   = &impl_->cart;

	// Reset transient audio output state — the ring buffer content is
	// not serialized because it's sub-frame scratch.
	impl_->apu.sample_accum_l   = 0;
	impl_->apu.sample_accum_r   = 0;
	impl_->apu.sample_accum_cnt = 0;
	impl_->apu.sample_head      = 0;
	impl_->apu.sample_tail      = 0;
	impl_->apu.sample_timer     = impl_->apu.cycles_per_sample;

	// Re-point the exposed FrameBuffer at the (now-restored) PPU fb.
	impl_->fb.pixels = impl_->ppu.framebuffer;
	impl_->fb.width  = GB_SCREEN_WIDTH;
	impl_->fb.height = GB_SCREEN_HEIGHT;
	impl_->fb.pitch  = GB_SCREEN_WIDTH;

	return true;
}

void  Emulator::SetClockMultiplier(float m) { impl_->clock_mul = m; }
float Emulator::GetClockMultiplier() const  { return impl_->clock_mul; }

Emulator &Instance()
{
	static Emulator g;
	return g;
}

} // namespace SGB

// C-style facade used by snes9x integration code.
bool S9xSGBInit(void)               { return SGB::Instance().Init(); }
void S9xSGBDeinit(void)             { SGB::Instance().Deinit(); }
void S9xSGBReset(void)              { SGB::Instance().ColdReset(); }
bool S9xSGBIsActive(void)           { return SGB::Instance().HasROM(); }
void S9xSGBRunFrame(void)           { SGB::Instance().RunFrame(); }
void S9xSGBRunCycles(int tcycles)   { SGB::Instance().RunCycles(static_cast<int32_t>(tcycles)); }

namespace {
	int32_t g_snes_cycle_accum = 0;
	int32_t g_sync_anchor      = 0;
	int32_t g_h_max            = 1364;  // NTSC default; overwritten per-frame by cpuexec
}

void S9xSGBResetClockSync(void)
{
	g_snes_cycle_accum = 0;
	g_sync_anchor      = 0;
}

void S9xSGBTickSnes(int snes_master_cycles)
{
	if (snes_master_cycles <= 0) return;
	g_snes_cycle_accum += snes_master_cycles;

	// Clock ratio depends on RunMode:
	//   SGB1: GB clock = SNES master / 5 (≈ 4.295 MHz, slightly faster
	//                    than DMG — matches the ICD2 cart's wiring).
	//   SGB2: GB clock = real DMG clock (4.194 MHz). The SNES still
	//                    runs at 21.477 MHz, so the ratio is ~5.121,
	//                    NOT 5. Using /5 in SGB2 mode makes the GB run
	//                    2.4% too fast; over a frame that's ~6 scan-
	//                    lines of drift, which desyncs the BIOS's
	//                    bank-read timing against our slice writes
	//                    and produces visible vertical row drift.
	//   DMG:  same as SGB2 — real GB clock.
	int32_t gb_cycles;
	const SGB::RunMode mode = SGB::Instance().GetRunMode();
	if (mode == SGB::RunMode::SGB)
	{
		gb_cycles = g_snes_cycle_accum / 5;
		if (gb_cycles > 0)
		{
			g_snes_cycle_accum -= gb_cycles * 5;
			SGB::Instance().RunCycles(gb_cycles);
		}
	}
	else
	{
		// 64-bit math to avoid overflow: ratio = 4194304 / 21477272.
		// gb_cycles = accum * 4194304 / 21477272.
		const int64_t scaled = static_cast<int64_t>(g_snes_cycle_accum) * 4194304;
		gb_cycles = static_cast<int32_t>(scaled / 21477272);
		if (gb_cycles > 0)
		{
			// Subtract back the SNES-cycle equivalent of what we ran.
			const int64_t consumed = (static_cast<int64_t>(gb_cycles) * 21477272) / 4194304;
			g_snes_cycle_accum -= static_cast<int32_t>(consumed);
			SGB::Instance().RunCycles(gb_cycles);
		}
	}
}

void S9xSGBResetSyncAnchor(int32_t cpu_cycles)
{
	g_sync_anchor = cpu_cycles;
}

void S9xSGBSetHMax(int32_t h_max)
{
	if (h_max > 0) g_h_max = h_max;
}

void S9xSGBSyncToSnesCycle(int32_t cpu_cycles)
{
	int32_t delta = cpu_cycles - g_sync_anchor;
	// Scanline wrap: snes9x's H-event subtracts H_Max from CPU.Cycles,
	// so a legitimate forward step across the wrap appears as a large
	// negative delta. Adding H_Max back recovers the real delta, as
	// long as we sync at least once per scanline (trivially true given
	// per-opcode sync points).
	if (delta < 0) delta += g_h_max;
	g_sync_anchor = cpu_cycles;
	// GB is held in reset (control bit 7 = 0) — advance the anchor but
	// do NOT step the GB core. Otherwise a BIOS write of $6003=$01
	// (reset line held low) still lets the GB progress, which breaks
	// the "toggle reset to re-boot GB" pattern the SGB BIOS uses after
	// the splash animation. Match bsnes: GB thread only runs while
	// r6003.d7 is 1.
	if (!S9xSGBBIOSGBIsReleased()) return;
	if (delta > 0) S9xSGBTickSnes(delta);
}

void S9xSGBOnPpuHBlank(void) { SGB::Instance().OnPpuHBlank(); }
void S9xSGBOnPpuVBlank(void) { SGB::Instance().OnPpuVBlank(); }
void S9xSGBCaptureScanline(const unsigned char *pixels)
{
	SGB::Instance().CaptureScanline(static_cast<const uint8_t *>(pixels));
}
void S9xSGBSetJoypad(uint16_t m)    { SGB::Instance().SetJoypad(m); }
void S9xSGBOnJoyserWrite(uint8_t v) { SGB::Instance().OnJoyserWrite(v); }

void S9xSGBBlitScreen(uint16_t *dest, uint32_t pitch_pixels)
{
	SGB::Instance().BlitScreen(dest, pitch_pixels);
}

int32_t S9xSGBGetSampleCount(void)
{
	int32_t count = SGB::Instance().GetAudioSamplesAvailable();
	// Cap reported avail. ProcessSound's audio-sync wait engages when
	// freeBytes/2 < availableSamples; bigger cap → bigger wait spike
	// when GB ring backs up → more aggressive throttle. With cap at
	// one SNES frame (1600 int16/48k), spikes drain ~3 buffers per
	// wait engagement (~24 ms), throttling emu to ~35 fps wall in
	// SGB BIOS mode. Cap at 1/4 frame (400) → ~6 ms wait spike →
	// roughly 4× less throttling, target ~60 fps wall.
	// Hard ceiling stays as a deadlock safeguard (avail must stay
	// well below queue max free, ~6144 int16 at 48 kHz / 64 ms).
	const int32_t out_rate    = SGB::Instance().GetAudioSampleRate();
	int32_t       cap         = (out_rate * 2) / 252;  // ~381 at 48k
	if (cap < 64)   cap = 64;
	if (cap > 6000) cap = 6000;
	if (count > cap) count = cap;
	return count;
}

int32_t S9xSGBDrainSamples(int16_t *dest, int32_t count_int16s)
{
	if (!dest || count_int16s <= 0) return 0;
	const int32_t frames = count_int16s / 2;
	const int32_t got    = SGB::Instance().DrainAudio(dest, frames);
	return got * 2;
}

void S9xSGBSetAudioRate(int32_t rate_hz)
{
	SGB::Instance().SetAudioRate(rate_hz);
}

int32_t S9xSGBGetAudioClockHz(void)
{
	return SGB::Instance().GetAudioClockHz();
}

int32_t S9xSGBGetAudioCyclesPerSample(void)
{
	return SGB::Instance().GetAudioCyclesPerSample();
}

int32_t S9xSGBGetAudioCpsRemainderStep(void)
{
	return SGB::Instance().GetAudioCpsRemainderStep();
}

void S9xSGBSetRunMode(uint8_t mode)
{
	SGB::RunMode m;
	switch (mode)
	{
		case 2:  m = SGB::RunMode::SGB2; break;
		case 0:  m = SGB::RunMode::DMG;  break;
		case 1:
		default: m = SGB::RunMode::SGB;  break;
	}
	SGB::Instance().SetRunMode(m);
}

void S9xSGBSetClockMultiplier(float mul)
{
	SGB::Instance().SetClockMultiplier(mul);
}

size_t S9xSGBStateSize(void)
{
	return SGB::Instance().StateSize();
}

void S9xSGBStateSave(uint8_t *buffer)
{
	SGB::Instance().StateSave(buffer);
}

bool S9xSGBStateLoad(const uint8_t *buffer, size_t size)
{
	return SGB::Instance().StateLoad(buffer, size);
}

bool S9xSGBSaveStateToFile(const char *filename)
{
	if (!filename) return false;

	const size_t need = SGB::Instance().StateSize();
	std::vector<uint8_t> buf(need);
	SGB::Instance().StateSave(buf.data());

	FILE *f = fopen(filename, "wb");
	if (!f) return false;
	const size_t w = fwrite(buf.data(), 1, need, f);
	fclose(f);
	return w == need;
}

void S9xSGBGetStatus(char *buf, size_t cap)
{
	SGB::Instance().GetStatus(buf, cap);
}

bool S9xSGBLoadStateFromFile(const char *filename)
{
	if (!filename) return false;

	FILE *f = fopen(filename, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	const long sz = ftell(f);
	if (sz <= 12 || sz > 4 * 1024 * 1024) { fclose(f); return false; }
	fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	const size_t got = fread(buf.data(), 1, static_cast<size_t>(sz), f);
	fclose(f);
	if (got != static_cast<size_t>(sz)) return false;

	return SGB::Instance().StateLoad(buf.data(), buf.size());
}

bool S9xSGBLoadROMBytes(const unsigned char *data, size_t size, const char *path_for_sram)
{
	if (!data || size < 0x150) return false;
	return SGB::Instance().LoadROM(static_cast<const uint8_t *>(data), size, path_for_sram);
}

bool S9xSGBLoadBootROMBytes(const unsigned char *data, size_t size)
{
	return SGB::Instance().LoadBootROM(static_cast<const uint8_t *>(data), size);
}

bool S9xSGBLoadEmbeddedBootROM(unsigned char mode)
{
	const uint8_t *src = (mode == 2) ? SGB::kSgb2BootRom : SGB::kSgbBootRom;
	return SGB::Instance().LoadBootROM(src, 256);
}

void S9xSGBPrimeBIOSHandshake(void)
{
	SGB::Instance().PrimeBIOSHandshake();
}

bool S9xSGBLoadROM(const char *filename)
{
	if (!filename || !*filename) return false;

	FILE *f = fopen(filename, "rb");
	if (!f) return false;

	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	const long sz = ftell(f);
	if (sz <= 0 || sz > 16 * 1024 * 1024)   // 16 MB is the MBC5/MBC6 ceiling
	{ fclose(f); return false; }
	fseek(f, 0, SEEK_SET);

	std::vector<uint8_t> buf(static_cast<size_t>(sz));
	const size_t got = fread(buf.data(), 1, static_cast<size_t>(sz), f);
	fclose(f);
	if (got != static_cast<size_t>(sz)) return false;

	return SGB::Instance().LoadROM(buf.data(), buf.size(), filename);
}

// ICD2 bridge — 0x6000-0x7FFF on the SNES side. P2b just stores writes in a
// raw register file and returns them on read. Real semantics (reset gating,
// packet FIFO, joypad multiplex, VRAM readback) land in P2c-P2e.
unsigned char S9xSGBGetICD2(unsigned short addr)
{
	return SGB::Instance().GetICD2(addr);
}

void S9xSGBSetICD2(unsigned char value, unsigned short addr)
{
	SGB::Instance().SetICD2(value, addr);
}

bool S9xSGBBIOSGBIsReleased(void)
{
	return SGB::Instance().IsGBReleased();
}

bool S9xSGBBIOSHandshakePending(void)
{
	return SGB::Instance().IsHandshakePending();
}
