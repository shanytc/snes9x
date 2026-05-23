/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_H_
#define _SGB_H_

#include <cstdint>
#include <cstddef>

namespace SGB {

struct FrameBuffer
{
	const uint8_t *pixels;  // 160x144 color-indexed (4 shades / SGB palette idx)
	uint32_t       width;
	uint32_t       height;
	uint32_t       pitch;
};

enum class RunMode : uint8_t
{
	DMG  = 0,  // Plain Game Boy, monochrome
	SGB  = 1,  // SGB1 — 4.295 MHz GB CPU, SGB features enabled
	SGB2 = 2   // SGB2 — exact 4.194 MHz GB CPU
};

struct CpuState;
struct Memory;
struct Ppu;
struct Apu;
struct Timer;
struct Joypad;
struct Cart;
struct SgbState;

class Emulator
{
public:
	Emulator();
	~Emulator();

	Emulator(const Emulator &) = delete;
	Emulator &operator=(const Emulator &) = delete;

	bool Init();
	void Deinit();
	void Reset();
	// Like Reset() but ALSO clears the BIOS-handshake packet cache and
	// counters. Use for SNES-side reset / new ROM load — Reset() alone
	// preserves the cache, which is correct for in-game $6003 0→1 GB
	// resets but wrong when the user invokes File→Reset.
	void ColdReset();

	bool LoadROM(const uint8_t *data, size_t size, const char *path = nullptr);
	void UnloadROM();
	bool HasROM() const;

	bool HasBattery() const;
	bool SaveBatteryToPath(const char *path) const;
	bool LoadBatteryFromPath(const char *path);
	bool TakeSramDirty();

	// Raw cart ROM accessor for hashing / external introspection. Returns
	// nullptr when no ROM is loaded.
	const uint8_t *GetROMData() const;
	size_t         GetROMSize() const;

	// Read a byte from the GB-side address space using the rcheevos
	// GameBoy / GameBoy Color memory map (0x0000-0xFFFF native + the
	// 0x10000-0x33FFF extended bank window). Side-effect-free; does not
	// touch I/O registers or PPU bus state. Returns 0 for unmapped or
	// out-of-range addresses.
	uint8_t        PeekRAByte(uint32_t addr) const;

	// Install the 256-byte DMG/SGB GB-side boot ROM. Takes effect on the
	// next Reset. Call with nullptr/size=0 to clear. Only loaded in
	// authentic BIOS mode — BIOS-less continues to start at 0x0100.
	bool LoadBootROM(const uint8_t *data, size_t size);

	// Populate the 5-packet boot-ROM handshake from the current cart's
	// header and queue the first packet as if the GB boot ROM had sent
	// it. The SGB BIOS waits on these to transition past its splash;
	// the GB's dumped boot ROM typically doesn't generate them, so we
	// synthesize them ourselves. Requires a cart already loaded.
	void PrimeBIOSHandshake();

	void SetRunMode(RunMode m);
	RunMode GetRunMode() const;

	// Advance the GB core by one whole video frame (ends at VBlank entry).
	void RunFrame();

	// Advance by N T-cycles. Used when snes9x drives the master clock directly.
	void RunCycles(int32_t tcycles);

	const FrameBuffer &GetFrameBuffer() const;

	// Consume pending audio samples. Returns the count written
	// (stereo frames — each frame stores L, R into out[2*i], out[2*i+1]).
	int32_t DrainAudio(int16_t *out, int32_t max_samples);
	int32_t GetAudioSampleRate() const;

	// Diagnostics: GB clock_hz and cycles_per_sample as currently
	// configured in the APU. Used by the SGB OSD to verify audio
	// rate math.
	int32_t GetAudioClockHz() const;
	int32_t GetAudioCyclesPerSample() const;
	int32_t GetAudioCpsRemainderStep() const;

	// Count of int16 values (NOT stereo frames) currently ready in the
	// ring buffer. Matches the convention S9xGetSampleCount uses —
	// useful when driving snes9x's host audio pull path.
	int32_t GetAudioSamplesAvailable() const;

	// Configure the APU's downsample target in Hz. Takes effect on the
	// next Reset(), or immediately for the next sample emitted.
	void    SetAudioRate(int32_t rate_hz);

	// Mapped from a SNES joypad bitmask — see sgb.cpp for the mapping table.
	void SetJoypad(uint16_t snes_pad_mask);

	// Called by the SNES $4016 write path; feeds the SGB command-packet sniffer.
	void OnJoyserWrite(uint8_t value);

	// Snapshot format — versioned block appended to snes9x's .state.
	size_t StateSize() const;
	void   StateSave(uint8_t *buffer) const;
	bool   StateLoad(const uint8_t *buffer, size_t size);

	// 1.0 = real hardware speed. <1 underclocks (ARM targets), >1 overclocks.
	void  SetClockMultiplier(float m);
	float GetClockMultiplier() const;

	// Internal — routes a decoded SGB command into the palette/attribute
	// state. Exposed because the packet callback trampoline needs it.
	void  OnSgbCommandInternal(uint8_t cmd, const uint8_t *data, uint32_t len);

	// Composite the SGB border + GB screen into a 256 × 224 BGR555
	// buffer. `pitch_pixels` is the stride in uint16_t units (use 256
	// for a tightly-packed buffer, or the SNES GFX.Screen's PPL).
	void  BlitScreen(uint16_t *dest, uint32_t pitch_pixels);

	// BIOS-mode overlay path — see S9xSGBOverlayBiosBorder for the
	// full description. Renders the captured custom border on top of
	// the SNES-rendered output, sparing the central 20×18 GB area.
	void  OverlayBiosBorder(uint16_t *dest, uint32_t pitch_pixels);

	// Write a one-line status snapshot — PC, SP, A, halt/stop flag,
	// total T-cycles, illegal-op count.
	void  GetStatus(char *buf, size_t cap) const;

	// ICD2 (BIOS-mode cart chip) register access — 0x6000-0x7FFF on the
	// SNES side. See S9xSGBGetICD2 / S9xSGBSetICD2 below for the C facade.
	// Non-const because reading $7000-$700F drains a single-slot FIFO
	// (clears the packet-ready flag exposed at $6002).
	uint8_t GetICD2(uint16_t addr);
	void    SetICD2(uint8_t value, uint16_t addr);

	// True once the BIOS has released the GB CPU (bit 7 of $6003). Used
	// by cpuexec to gate per-frame GB core stepping.
	bool    IsGBReleased() const;

	// True while synth handshake packets are still pending drain.
	bool    IsHandshakePending() const;

	bool    IsBootHandoffCaptured() const;

	// GB PPU scanline event hooks for SGB row/bank counter advance.
	void    OnPpuHBlank();
	void    OnPpuVBlank();
	void    CaptureScanline(const uint8_t *pixels);

	// Opaque implementation struct — forward-declared so file-local
	// helpers in sgb.cpp can reference the full type. External code
	// has no way to obtain one.
	struct Impl;

	Impl *DebugImpl() const { return impl_; }

private:
	Impl *impl_;
};

// Global singleton — snes9x keeps one active SGB core.
Emulator &Instance();

} // namespace SGB

// C-style facade used by snes9x integration code (matches S9x* naming).
bool S9xSGBInit(void);
void S9xSGBDeinit(void);
void S9xSGBReset(void);
bool S9xSGBLoadROM(const char *filename);

// Hand the 256-byte GB-side boot ROM to the SGB core. Only used in
// authentic BIOS mode — boot ROM scrolls the Nintendo logo and sends
// 5 SGB handshake packets that the BIOS is waiting for before it
// transitions out of its splash screen.
bool S9xSGBLoadBootROMBytes(const unsigned char *data, size_t size);

// Install the built-in SGB1 (mode=1) or SGB2 (mode=2) boot ROM. Used
// as the default in BIOS mode — most publicly-dumped sgb*.boot.rom
// files are plain DMG boot ROMs without the handshake, so we ship the
// real SGB-specific variants (from LIJI32/SameBoy under MIT).
bool S9xSGBLoadEmbeddedBootROM(unsigned char mode);

// Prime the 5-packet SGB handshake from the loaded cart. Call after
// S9xSGBLoadROM/S9xSGBLoadROMBytes in BIOS mode — boot ROM dumps in
// the wild almost never contain the SGB-specific handshake code, so
// we synthesize the packets from cart header bytes $0104-$014F.
void S9xSGBPrimeBIOSHandshake(void);

// Load a GB ROM from an in-memory buffer. Callers that already have the
// bytes (e.g. after snes9x's FileLoader has unzipped a .zip/.jma/.7z
// container) use this to skip the stdio re-read.
bool S9xSGBLoadROMBytes(const unsigned char *data, size_t size, const char *path_for_sram);
void S9xSGBRunFrame(void);
void S9xSGBRunCycles(int tcycles);

// Accumulator-based SNES→GB cycle stepping for BIOS mode. Call per
// SNES opcode (or per H event) with the number of SNES master cycles
// just consumed. Internally divides by 5 (SGB1 clock ratio) and steps
// the GB core by the resulting GB T-cycle count, carrying the fractional
// remainder to the next call. Much finer-grained than per-scanline
// batch stepping — the BIOS's $B9BE row-diff check needs continuous
// row advancement to progress.
void S9xSGBTickSnes(int snes_master_cycles);
void S9xSGBResetClockSync(void);

// Coroutine-style mid-opcode catch-up. Call with current CPU.Cycles
// before every ICD2 register read/write and at end of each SNES
// opcode iteration. Internally tracks the last-synced anchor cycle;
// compensates for SNES scanline wrap using the h_max value set via
// S9xSGBSetHMax(). The GB core is always caught up to the SNES's
// exact master-cycle position before the host side reads $6000/
// $6002/etc., so cross-domain reads reflect the most recent GB PPU
// state — matches bsnes's libco thread sync-point semantics without
// the coroutine runtime.
void S9xSGBSyncToSnesCycle(int32_t cpu_cycles);

// Tell sgb.cpp the SNES scanline period (Timings.H_Max) so it can
// recover from CPU.Cycles wrap. Call once per frame from cpuexec.
// Kept out of S9xSGBSyncToSnesCycle's signature so getset.h inlines
// don't have to see the STimings struct.
void S9xSGBSetHMax(int32_t h_max);

// Reset the sync anchor to the given SNES cycle. Call at known-good
// restart points — start of each SNES frame, just after $6003 bit-7
// release (since Reset() zeroes the GB state). Without a reset, a
// stale anchor from before a scanline wrap can make the first delta
// appear huge (or negative beyond h_max correction).
void S9xSGBResetSyncAnchor(int32_t cpu_cycles);

// GB PPU scanline-event hooks used to advance the SGB row/bank counters
// exposed at $6000. Call at HBlank end of each visible line (mode 0 →
// mode 2 transition for lines 0..142) and at VBlank entry (line 143 →
// 144 transition). Benign no-ops in BIOS-less mode.
void S9xSGBOnPpuHBlank(void);
void S9xSGBOnPpuVBlank(void);

// Capture a drawn GB scanline (160 palette indices, 0..3) into the SGB
// char-transfer ring. Call immediately after RenderScanline; writes to
// lcd_ring[sgb_bank] at row (sgb_row & 7). Benign no-op in BIOS-less mode.
void S9xSGBCaptureScanline(const unsigned char *pixels);
void S9xSGBSetJoypad(uint16_t snes_pad_mask);
void S9xSGBOnJoyserWrite(uint8_t value);
bool S9xSGBIsActive(void);

// Composite the current frame (border + GB screen + mask mode) into a
// 256 × 224 BGR555 buffer. `pitch_pixels` is the stride in uint16_t
// units. Call after S9xSGBRunFrame.
void S9xSGBBlitScreen(uint16_t *dest, uint32_t pitch_pixels);

// BIOS-mode border overlay. In BIOS mode the SNES 65816 runs the SGB2
// BIOS, which renders its OWN device-frame border into GFX.Screen via
// normal SNES BG layers. Once the loaded GB game uploads a custom
// border via PCT_TRN/CHR_TRN (captured by our framebuffer-capture
// path even in BIOS mode), this routine overlays our captured border
// onto GFX.Screen — but ONLY the outer ring, leaving the central
// 20×18 GB area at offset (48,40) untouched so the BIOS's authentic
// GB rendering stays visible. No-op if no custom border has been
// captured yet (the BIOS's default frame stays).
//   `dest`         — must point at a 256-wide BGR555 buffer (typically
//                    GFX.Screen).
//   `pitch_pixels` — uint16 stride per row.
// Wired into S9xEndScreenRefresh AFTER FLUSH_REDRAW (which finalizes
// the PPU output into GFX.Screen) and BEFORE S9xDisplayMessages, so
// the overlay isn't clobbered by the PPU blit and the snes9x OSD
// still draws on top.
void S9xSGBOverlayBiosBorder(uint16_t *dest, uint32_t pitch_pixels);

// Audio bridge — match snes9x's S9xGetSampleCount / S9xMixSamples
// contract. `count_int16s` is the number of int16 samples (stereo frame
// × 2), returning the number actually drained.
int32_t S9xSGBGetSampleCount(void);
int32_t S9xSGBDrainSamples(int16_t *dest, int32_t count_int16s);
void    S9xSGBSetAudioRate(int32_t rate_hz);
int32_t S9xSGBGetAudioRate(void);

// Diagnostics — see Emulator::GetAudioClockHz / GetAudioCyclesPerSample.
int32_t S9xSGBGetAudioClockHz(void);
int32_t S9xSGBGetAudioCyclesPerSample(void);
int32_t S9xSGBGetAudioCpsRemainderStep(void);

// Timing knobs — push Settings.GameBoyRunMode and GBClockMultiplier
// into the emulator from snes9x's per-frame dispatch. Both take effect
// from the next RunFrame forward; SetRunMode does NOT implicitly reset
// so applying it mid-game is harmless (but won't rewrite boot-state
// registers until the next Reset).
void    S9xSGBSetRunMode(uint8_t mode /* 0=DMG, 1=SGB1, 2=SGB2 */);
void    S9xSGBSetClockMultiplier(float mul);

// Save-state bridge — snapshot.cpp branches on Settings.SuperGameBoy
// and writes/reads the SGB state directly to/from the save file.
// Format: 4-byte "SGB!" magic + version + payload size + opaque blob.
size_t  S9xSGBStateSize(void);
void    S9xSGBStateSave(uint8_t *buffer);
bool    S9xSGBStateLoad(const uint8_t *buffer, size_t size);

// Fill `buf` with a one-line status snapshot:
//   "SGB PC=abcd T=xxxxxx(+nnnnn) HALT illegal=N"
// Used by cpuexec.cpp's periodic OSD hook to diagnose hangs. `cap`
// is the buffer size; truncated on overflow.
void    S9xSGBGetStatus(char *buf, size_t cap);

// Convenience file I/O wrappers — write the full blob to `filename`,
// or read it back. Return false on I/O error or format mismatch.
bool    S9xSGBSaveStateToFile(const char *filename);
bool    S9xSGBLoadStateFromFile(const char *filename);

// ---- P2 — ICD2 bridge ------------------------------------------------------
// The real SGB cart contains a custom "ICD2" chip that memory-maps a set of
// registers at 0x6000-0x7FFF on the SNES side. The BIOS reads/writes these
// to talk to the GB subsystem on the cart. Our bridge routes those accesses
// into our GB core.
//
// P2b scope: skeleton handlers that track register writes so the BIOS can
// read back consistent values. P2c+ add real semantics (packet FIFO,
// joypad routing, VRAM readback, reset gating).
unsigned char S9xSGBGetICD2 (unsigned short addr);
void          S9xSGBSetICD2 (unsigned char value, unsigned short addr);

// True when the BIOS has released the GB CPU from reset (via the ICD2
// reset register). cpuexec.cpp gates per-frame GB core stepping on this —
// before release the BIOS has sole control of the SNES, after release
// the GB runs in parallel. P2c defines the actual release bit.
bool          S9xSGBBIOSGBIsReleased (void);

// True while synth handshake packets are still queued in the single-slot
// FIFO. cpuexec uses this as an additional gate on GB core stepping —
// real GB packets must not clobber a pending synth packet before the
// BIOS reads it, so the GB is paused until the handshake drains out.
bool          S9xSGBBIOSHandshakePending (void);

bool          S9xSGBBootHandoffCaptured (void);

struct S9xSGBDebugState
{
	bool     has_rom;
	uint8_t  cgb_flag;
	uint8_t  sgb_flag;
	uint8_t  cart_type;
	uint32_t rom_size;
	uint32_t ram_size;
	char     title[17];

	uint16_t pc, af, bc, de, hl, sp;
	bool     ime, ime_pending, halted, stopped, halt_bug;
	uint64_t t_cycles;
	uint32_t illegal_ops;
	uint8_t  opcode_at_pc[4];

	uint8_t  ie, if_;
	bool     boot_rom_enabled;

	uint8_t  lcdc, stat, ly, scx, scy, bgp, obp0, obp1, wy, wx, lyc;

	uint8_t  mbc_type;
	uint32_t mbc_rom_bank, mbc_ram_bank;
	bool     mbc_ram_enable;
	bool     mbc1_mode;

	uint8_t  icd_control;
	uint32_t packets_received, f1_packets;
	uint8_t  mlt_players, input_index;
	uint8_t  sgb_row, sgb_bank;
	bool     released, handshake_pending;
	bool     boot_handoff_captured;
	uint32_t handoff_frames;
	uint16_t handoff_pc, handoff_af, handoff_bc, handoff_de, handoff_hl, handoff_sp;

	bool     border_tiles_loaded, border_map_loaded;
	uint8_t  mask_mode;
	uint16_t pal0_color0;

	uint8_t  queue_count, queue_head, queue_tail;
	uint8_t  synth_remaining;
	uint32_t r_6000, r_6002, r_6003, r_7000, r_7800;
	uint32_t w_6000, w_6001, w_6003, w_7000, w_6004;
	uint16_t last_read_addr, last_write_addr;
	uint8_t  last_write_val;
	uint8_t  last_cmd_ids[8];
	uint8_t  last_cmd_ids_len;
	uint8_t  joypad[4];
	uint8_t  input_value;
	uint16_t mlt_auto_drop_polls;

	// Live GB joypad object state. joypad[]/input_value above are the ICD2-
	// side mirrors of $6004-$6007 and the last byte the GB wrote to $FF00.
	// These fields below are the SGB::Joypad struct that actually answers
	// JoypadRead, so we can see what the running cart sees.
	//   joypad_select : upper-nibble select lines from $FF00 (bits 4/5)
	//   joypad_dpad   : internal active-low D-pad nibble (used when sgb_active=0)
	//   joypad_btns   : internal active-low buttons nibble (used when sgb_active=0)
	//   joypad_prev_mask : last GB_* mask, for IRQ edge detection
	//   joypad_sgb_active : true when JoypadRead routes through the bridge
	//   joypad_sgb_index  : current player rotation index
	//   joypad_sgb_pads[] : per-player bridge mirrors (same as icd2.joypad[])
	//   joypad_ff00       : live JoypadRead() result with current select bits
	//   joypad_snes_mask  : raw SNES controller 0 bitmask we forward via SetJoypad
	uint8_t  joypad_select;
	uint8_t  joypad_dpad;
	uint8_t  joypad_btns;
	uint8_t  joypad_prev_mask;
	bool     joypad_sgb_active;
	uint8_t  joypad_sgb_index;
	uint8_t  joypad_sgb_pads[4];
	uint8_t  joypad_ff00;
	uint16_t joypad_snes_mask;

	// $FF00 access tracking. The game must read $FF00 with one of P14/P15
	// pulled low for the joypad input to actually reach its code. ff00_reads
	// counts every IO read of $FF00; ff00_dpad_reads counts reads where the
	// returned value's upper nibble has P14=0 (dpad selected); ff00_btns_reads
	// counts the same for P15. ff00_last_returns is a ring of the last 8 bytes
	// JoypadRead returned to the CPU; ff00_last_writes is a ring of the last
	// 8 bytes the GB wrote into $FF00 select. If a game responds to A but
	// not D-pad, watching ff00_dpad_reads stay flat would confirm the game
	// never selects the dpad nibble.
	// Full HRAM dump ($FF80-$FFFE, 127 bytes). Each game uses HRAM differently
	// for input state, so dumping the whole region lets the user diff
	// pressed/released snapshots and locate the input byte.
	uint8_t  hram_peek[128];

	// WRAM peek at $C000 (game state region — start of WRAM bank 0).
	// First 64 bytes of WRAM cover Alleyway's main game state block.
	uint8_t  wram_peek[64];

	uint32_t ff00_reads;
	uint32_t ff00_writes;
	uint32_t ff00_dpad_reads;
	uint32_t ff00_btns_reads;
	uint32_t ff00_idle_reads;
	uint8_t  ff00_last_returns[8];
	uint8_t  ff00_dpad_returns[8];
	uint8_t  ff00_btns_returns[8];
	uint8_t  ff00_last_writes[8];
	uint8_t  ff00_last_returns_idx;
	uint8_t  ff00_dpad_returns_idx;
	uint8_t  ff00_btns_returns_idx;
	uint8_t  ff00_last_writes_idx;

	uint8_t  bios_state_0101;
	uint8_t  bios_substate_0102;
	uint8_t  bios_dma_swap_0280;
	uint16_t bios_dma_ptr_a_0282, bios_dma_ptr_b_0284;
	uint32_t vram_hash, oam_hash;
	uint8_t  stack_peek[32];

	uint32_t data_snd_packets;
	uint32_t data_trn_packets;
	uint32_t jump_packets;
	uint16_t last_jump_addr;
	uint8_t  last_jump_bank;
	uint16_t last_data_snd_addr;
	uint8_t  last_data_snd_bank;
	uint16_t last_data_trn_addr;
	uint8_t  last_data_trn_bank;

	uint8_t  data_snd_hist[16][16];
	uint8_t  data_snd_hist_count;
	uint8_t  data_snd_hist_head;

	uint32_t irq_serviced[5];

	struct StatIrqEvent {
		uint8_t  ly;
		uint8_t  lyc;
		uint8_t  scx;
		uint8_t  scy;
		uint8_t  bgp;
		uint8_t  stat;
		uint8_t  source;
		uint8_t  frame_phase;
		uint64_t t_cycles;
	} stat_irq_ring[16];
	uint8_t  stat_irq_ring_count;
	uint8_t  stat_irq_ring_head;

	uint64_t stat_irq_raise_count;
	uint64_t frame_count;

	// Diagnostic peeks for Zerd no Densetsu STAT IRQ investigation.
	// wram_c2cc_peek covers $C2C0-$C2CF — at $C2CC the game installs a
	// "JP <handler>" trampoline so $0048 (LCDSTAT vector) can JP $C2CC
	// → JP $XXXX. If $C2CC isn't $C3 ($D6 $64) or $C3 ($EE $78), the
	// STAT IRQ jumps to garbage.
	uint8_t  wram_c2cc_peek[16];
	uint32_t lyc_writes;   // count of CPU writes to $FF45 since reset
	uint32_t stat_writes;  // count of CPU writes to $FF41 since reset

	// Per-instruction PC trace ring — last 64 entries before crash, or
	// last 64 entries if no crash detected. Frozen after RST 38 loop
	// detection (PC=$0038/$0039 for 200 consecutive instructions) so the
	// pre-crash trail isn't overwritten by the loop spinning forever.
	struct PcTraceSlot {
		uint16_t pc;
		uint16_t sp;
		uint16_t af;
		uint8_t  opcode;
		uint8_t  ime;
	} pc_trace[64];
	uint8_t  pc_trace_count;
	bool     pc_trace_frozen;

	// PPU register-write ring — last 32 writes to LCDC / STAT / SCY / SCX /
	// LYC / WY / WX, captured with the PPU's LY+mode+dot-into-mode at the
	// moment the CPU did the write. Used to see whether the game's STAT
	// IRQ handler is updating LCDC.1 / WX during mode 2 (good — applies to
	// next mode 3) or mid-mode 3 (bad — half the scanline already rendered
	// with old config). One Piece dialog-boundary glitch lives in here.
	struct PpuRegWriteSlot {
		uint16_t addr;
		uint8_t  value;
		uint8_t  prev;
		uint8_t  ly;
		uint8_t  mode;          // 0=HBl 1=VBl 2=OAM 3=Trf
		uint8_t  sprite_count;  // 0..10, sprites covering the scanline
		uint16_t mode_clock;
		uint64_t t_cycles;
	} reg_write_ring[32];
	uint8_t  reg_write_ring_count;
	uint8_t  reg_write_ring_head;

	// Full GB VRAM snapshot ($8000-$9FFF, 8 KiB). The CPU debugger formats
	// the visible BG tilemap region and a slice of tile data — having the
	// whole window means we can decide at format time which tilemap (LCDC.3)
	// and which tile-data base (LCDC.4 signed vs unsigned) the running game
	// is using, without needing a separate copy per addressing mode.
	uint8_t  vram_dump[0x2000];

	// Cart RAM dump ($A000-$BFFF, up to 8 KiB — first ram_size bytes are
	// valid; rest is unused). When ram_enable=0 reads return $FF, but the
	// underlying storage is preserved across enable/disable cycles, so
	// inspecting this can tell us whether the game's writes actually
	// landed in SRAM or were dropped because RAM wasn't enabled at write
	// time. The mbc_ram_enable_* counters track every MBC1/3/5 enable
	// register write (value & 0x0F == 0x0A enables, else disables) so we
	// can spot games that fail to enable RAM before reading from $Axxx.
	uint8_t  cart_ram_dump[0x2000];
	uint32_t mbc_ram_enables;   // count of writes that set ram_enable=true
	uint32_t mbc_ram_disables;  // count of writes that set ram_enable=false
	uint32_t mbc_sram_reads_when_disabled;  // $A000-$BFFF reads that returned $FF
	uint32_t mbc_sram_writes_when_disabled; // $A000-$BFFF writes that were dropped

	// Full OAM snapshot ($FE00-$FE9F = 40 sprites × 4 bytes). Each entry
	// is { Y, X, tile_index, flags }. Y=0 / X=0 means hidden (off-screen).
	// Used when in-game sprites (car, opponents) are suspect.
	uint8_t  oam_dump[0xA0];
};

void          S9xSGBGetDebugState (S9xSGBDebugState *out);

// ---- Integration hooks for RetroAchievements -------------------------------
// Expose the loaded GB ROM so the platform can hash it under the GameBoy /
// GameBoy Color console IDs (the SNES Memory.ROM only holds the SGB BIOS in
// authentic-BIOS mode, and is empty in BIOS-less mode). Returns false if no
// GB ROM is loaded. The buffer is owned by the SGB module and is valid until
// the next LoadROM/Deinit.
bool          S9xSGBGetROMBytes (const unsigned char **out_data, size_t *out_size);

// One-byte read from the GB address space for RetroAchievements memory
// inspection. Reads are done against raw backing storage (WRAM, HRAM,
// cart ROM bank-mapped, cart SRAM bank-mapped) — no I/O side effects,
// no interrupt or PPU bus contention. `addr` is the rcheevos GameBoy /
// GameBoy Color memory map address (0x0000-0xFFFF for native space,
// 0x10000-0x33FFF for the extended SRAM/CGB-WRAM bank window). Returns
// 0 for unmapped or out-of-range addresses.
unsigned char S9xSGBPeekRAByte (unsigned int addr);

bool S9xSGBHasBattery(void);
bool S9xSGBSaveBatteryToPath(const char *path);
bool S9xSGBLoadBatteryFromPath(const char *path);
bool S9xSGBTakeSramDirty(void);

#endif
