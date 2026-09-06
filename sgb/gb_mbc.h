/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_MBC_H_
#define _SGB_GB_MBC_H_

#include <cstdint>
#include <vector>

namespace SGB {

enum class MbcType : uint8_t
{
	None       = 0,
	MBC1       = 1,
	MBC2       = 2,
	MBC3       = 3,
	MBC5       = 5,
	MBC6       = 6,  // dual half-bank ROM/flash windows + split SRAM (Net de Get)
	MBC7       = 7,  // accelerometer + 93LC56 serial EEPROM (Kirby Tilt 'n' Tumble)
	HuC1       = 8,  // MBC1-like, with infrared (RAM<->IR via $0000-$1FFF)
	HuC3       = 9,  // MBC-like, with RTC command interface + IR
	MMM01      = 10, // Nintendo multicart meta-mapper (Mani / Taito 4-in-1)
	SachenMMC1 = 11, // Sachen 4B-series unlicensed multicarts
	TAMA5      = 12, // Bandai Tamagotchi — register port + EEPROM + RTC
	M161       = 13, // write-once 32 KiB-page multicart (Mani 4-in-1 "Tetris Set")
	Camera     = 14, // Game Boy Camera (Pocket Camera) — MBC3-like + M64282FP sensor
	Rocket     = 15, // Datel "Rocket Games" unlicensed GBC carts (per NewRisingSun's RE)
	NtOld1     = 16, // Makon/NT early carts: $5000-port mode set, 5-bit bank flip
	NtOld2     = 17, // Makon/NT later singles (Sonic Adventure 8 etc.): 8-bit bank flip
	BBD        = 18, // BBD: MBC5 + bit-scrambled banked ROM + scrambled bank numbers
	Ggb81      = 19, // DSH-GGB-81 (BBD sibling): data scramble only
	Hitek      = 20, // Gaoke/Hitek (BBD sibling): powers up scrambled, no $3xxx reg
	Sintax     = 21, // Sintax: MBC5 + per-(bank&3) XOR on banked reads + bank reorder
	SkobLee8   = 22, // SKOB LEE8 PCBs: Sintax-like XOR with preset defaults
	LiCheng    = 23, // Li Cheng/Niutoude: MBC5 that ignores $2101-$2FFF bank writes
	NtNew      = 24, // Makon/NT later: MBC5 + $55-armed 8K half-bank split windows
	PokeJadeDia= 25, // Makon Pokemon Jade/Diamond: MBC3 + fake-RTC D/E/F registers
	Vf001      = 26  // Vast Fame: MBC5 + rotate-XOR protection engine
};

// Extra state for the scrambled unlicensed mappers - lives beside MbcState in
// the Cart (v7 savestate blob, append-only). Field use per type:
//   BBD/Ggb81/Hitek : mode = data-scramble mode, mode2 = bank-scramble mode
//   Sintax          : mode = reorder mode, xors[4], raw_bank, cur_xor
//   SkobLee8        : mode = reorder mode, xors[4], cur_xor
//   NtNew           : mode = split armed, xors[0..1] = 8K half-bank regs
//   PokeJadeDia     : mode = "not-RTC" select, xors[0..1] = registers D/E
//   Vf001           : the vf_* protection engine (taizou's RE)
struct MbcUnl
{
	uint8_t  mode      = 0;
	uint8_t  mode2     = 0;
	uint8_t  xors[4]   = {0, 0, 0, 0};
	uint8_t  raw_bank  = 1;
	uint8_t  cur_xor   = 0;

	uint8_t  vf_config    = 0;
	uint8_t  vf_running   = 0;
	uint8_t  vf_6000      = 0;
	uint8_t  vf_700x[15]  = {0};
	uint8_t  vf_seq_bank  = 0;
	uint16_t vf_seq_addr  = 0;
	uint8_t  vf_seq_len   = 0;
	uint8_t  vf_seq[4]    = {0};
	uint8_t  vf_seq_left  = 0;
	uint8_t  vf_repl_on   = 0;
	uint16_t vf_repl_addr = 0;
	uint8_t  vf_repl_bank = 0;

	uint8_t  vf_pad       = 0;   // the v7 blob's tail padding, kept explicit

	// Zook Z (USA) board (v9 savestates, appended): the chip banks through
	// 4-byte codes at $7081 and answers $7x80 byte sequences at $Ax80.
	uint8_t  vfz_on         = 0;
	uint8_t  vfz_after_read = 0;
	uint8_t  vfz_len        = 0;
	uint8_t  vfz_code_n     = 0;
	uint8_t  vfz_code[4]    = {0};
	uint8_t  vfz_seq[32]    = {0};
};

struct MbcState
{
	MbcType  type       = MbcType::None;
	uint32_t rom_bank   = 1;
	uint32_t ram_bank   = 0;
	bool     ram_enable = false;
	bool     mbc1_mode  = false;    // 0 = ROM banking, 1 = RAM banking

	// MBC3 RTC — 5 8-bit counters + latch gate.
	uint8_t  rtc_regs[5]    = {0};
	uint8_t  rtc_latched[5] = {0};
	bool     rtc_latch      = false;
	uint8_t  rtc_select     = 0;

	// Sachen MMC1: outer-bank/mask + header lock. While locked, ROM reads in
	// 0x0100-0x01FF go through an A0/A6 and A1/A4 bit-permutation so the boot
	// ROM's logo check sees the real Nintendo logo at 0x0104-0x0133. Carts that
	// run raw after boot (Cart::sachen_runs_raw, e.g. 4B-005) drop the lock at
	// the 0xFF50 hand-off; the rest keep the xform for their scrambled high-bank
	// entry and clear it later via unlock_ctr.
	uint8_t  sachen_outer_bank  = 0;
	uint8_t  sachen_outer_mask  = 0;
	bool     sachen_locked      = true;
	uint8_t  sachen_unlock_ctr  = 0;

	// MMM01: Nintendo multicart meta-mapper. After power-on the last
	// 32 KiB of ROM (the menu) is mapped at $0000-$7FFF; the menu
	// configures rom/ram base+mask registers, then writes bit 6 of
	// $0000-$1FFF to lock into "game mode" where the selected sub-game
	// runs under MBC1-style banking inside its allotted ROM window.
	// Fields mirror SameBoy's mmm01 struct for direct spec parity.
	bool     mmm01_locked            = false;
	bool     mmm01_mbc1_mode         = false;  // bit 0 of $6000-$7FFF
	bool     mmm01_mbc1_mode_disable = false;  // bit 6 of $4000-$5FFF
	bool     mmm01_multiplex_mode    = false;  // bit 6 of $6000-$7FFF
	uint8_t  mmm01_rom_bank_low      = 0;      // MBC1-style low ROM bank
	uint8_t  mmm01_rom_bank_mid      = 0;      // upper 2 bits of $2000-$3FFF
	uint8_t  mmm01_rom_bank_high     = 0;      // upper outer ROM bank bits
	uint8_t  mmm01_ram_bank_low      = 0;
	uint8_t  mmm01_ram_bank_high     = 0;
	uint8_t  mmm01_rom_bank_mask     = 0;      // bits [5:2] of $6000-$7FFF
	uint8_t  mmm01_ram_bank_mask     = 0xFF;   // bits [5:4] of $0000-$1FFF

	// One-shot event flag: rises when the menu writes the lock bit. The
	// SGB layer polls and clears it, then injects PAL01/PAL23 packets
	// that force all 4 SGB system palettes to grayscale — without this
	// the sub-game inherits whatever palette the SGB BIOS assigned to
	// the menu's title during its one-shot handshake, which usually
	// looks badly inverted.
	bool     mmm01_just_locked       = false;

	// MBC3 RTC oscillator: T-cycles toward the next one-second tick.
	int32_t  rtc_sub_cycles          = 0;
};

struct Cart;

void MbcReset(MbcState &s);
// Reset the Cart's MbcUnl to the mapper's power-on state (Hitek and
// SkobLee8 boards power up with non-zero scramble settings).
void MbcUnlReset(Cart &c);

// Advance the MBC3 RTC by `tcycles` real-time T-cycles (one second per
// 4194304). No-op for carts without an RTC or with the halt bit set.
void MbcTickRtc(MbcState &s, int32_t tcycles);
// `unl` (the Cart's MbcUnl) is required for the scrambled unlicensed types;
// peeks may pass null and get raw bytes with no protection side effects.
uint8_t MbcRead(MbcState &s, const std::vector<uint8_t> &rom, const std::vector<uint8_t> &sram, uint16_t addr, bool mbc1_multicart = false, MbcUnl *unl = nullptr);
void    MbcWrite(Cart &c, uint16_t addr, uint8_t value);

// Notify the mapper of a CPU write to addr >= 0x8000 (VRAM/WRAM/SRAM/HRAM/IO).
// Used by Sachen MMC1 to advance its unlock counter — writes there are
// otherwise invisible to the MBC. Value-sensitive (Sachen counts $31
// writes per Tauwasser's RE) so the byte must be passed.
void    MbcNotifyHighWrite(MbcState &s, uint16_t addr, uint8_t value);

} // namespace SGB

#endif
