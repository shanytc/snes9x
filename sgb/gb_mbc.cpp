/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Memory Bank Controllers. MBC1, MBC3, MBC5 cover ~95% of commercial
// GB carts; MBC2, HuC1, HuC3, MMM01, and Sachen MMC1 are also
// handled, as are MBC6 (dual half-bank windows + flash) and MBC7
// (accelerometer + 93LC56 EEPROM).
//
// Register meanings per Pan Docs:
//
//   MBC1:
//     0x0000-0x1FFF  RAM enable (write 0x0A low nibble to enable)
//     0x2000-0x3FFF  ROM bank lower 5 bits (0 auto-corrects to 1)
//     0x4000-0x5FFF  RAM bank / ROM bank upper 2 bits (mode-dependent)
//     0x6000-0x7FFF  Mode select — 0=ROM banking, 1=RAM banking
//
//   MBC3:
//     0x0000-0x1FFF  RAM/RTC enable
//     0x2000-0x3FFF  ROM bank (7 bits, 0 auto-corrects to 1)
//     0x4000-0x5FFF  RAM bank (0..7) or RTC register select (0x08..0x0C)
//     0x6000-0x7FFF  RTC latch — 0 then 1 latches the RTC counters
//
//   MBC5:
//     0x0000-0x1FFF  RAM enable
//     0x2000-0x2FFF  ROM bank lower 8 bits (0 stays 0)
//     0x3000-0x3FFF  ROM bank bit 8 (only bit 0 of value matters)
//     0x4000-0x5FFF  RAM bank 0..0x0F (bit 3 of value = rumble on rumble carts)
//
//   HuC1: (MBC1-like, with an infrared link)
//     0x0000-0x1FFF  RAM/IR select — value 0x0E routes 0xA000-0xBFFF to the
//                    IR register; any other value routes it to cart RAM.
//                    HuC1 RAM has no separate enable (always live).
//     0x2000-0x3FFF  ROM bank (low 6 bits)
//     0x4000-0x5FFF  RAM bank (low 2 bits)
//
//   HuC3: (MBC-like, with an RTC command interface + IR)
//     0x0000-0x1FFF  $A000-$BFFF window mode (low nibble): $0/$A = RAM (ro/rw),
//                    $B = RTC command, $C = response, $D = semaphore, $E = IR
//     0x2000-0x3FFF  ROM bank (7 bits, no 0->1 remap)
//     0x4000-0x5FFF  RAM bank (low 2 bits)
//
//   TAMA5: (Bandai Tamagotchi — a nibble register port, not the usual banking)
//     0xA001  register select — which TAMA5 register the next 0xA000 access hits
//     0xA000  nibble data port. Registers: 0/1 = ROM bank lo/hi, 4/5 = write
//             data lo/hi, 6/7 = address hi/lo (writing reg 7 commits), 0xA =
//             ready handshake (reads 0xF1), 0xC/0xD = read result lo/hi. The
//             5-bit address + a 3-bit mode pick a 32-byte EEPROM cell or a
//             TAMA6 RTC register / command.

#include "gb_mbc.h"
#include "gb_cart.h"

#include <ctime>
#include <cstring>

static bool (*g_gb_camera_cb)(unsigned char *, int, int) = nullptr;

void S9xGBSetCameraCallback(bool (*cb)(unsigned char *, int, int))
{
	g_gb_camera_cb = cb;
}

int g_cam_countdown = 0;
uint8_t g_cam_shade[128 * 112] = {0};
int g_cam_live = 0;
int g_cam_brightness = 0;

namespace SGB {

// TAMA5 reuses existing MbcState fields (it never coexists with the mappers
// that own them), keeping MbcState's size — and the savestate layout — fixed:
//   rom_bank          = ROM bank (BANK_LO | BANK_HI << 4)
//   rtc_select        = selected TAMA5 register (the 0xA001 latch)
//   sachen_outer_bank = packed write byte (WRITE_HI << 4 | WRITE_LO)
//   sachen_outer_mask = packed address byte (ADDR_HI << 4 | ADDR_LO);
//                       low 5 bits = RAM/command address, top 3 bits = op mode
//   rtc_regs[0..4] + rtc_latched[0..1] = TAMA6 BCD timer page, one nibble each
namespace {

inline void Tama5SetRtcNib(MbcState &s, uint8_t idx, uint8_t nib)
{
	nib &= 0x0F;
	const uint8_t b = static_cast<uint8_t>(idx >> 1);
	uint8_t &cell = (b < 5) ? s.rtc_regs[b] : s.rtc_latched[b - 5];
	cell = (idx & 1) ? static_cast<uint8_t>((cell & 0x0F) | (nib << 4))
	                 : static_cast<uint8_t>((cell & 0xF0) | nib);
}

inline uint8_t Tama5RtcNib(const MbcState &s, uint8_t idx)
{
	const uint8_t b = static_cast<uint8_t>(idx >> 1);
	const uint8_t cell = (b < 5) ? s.rtc_regs[b] : s.rtc_latched[b - 5];
	return (idx & 1) ? static_cast<uint8_t>(cell >> 4)
	                 : static_cast<uint8_t>(cell & 0x0F);
}

inline void Tama5SeedRtc(MbcState &s)
{
	std::time_t t = std::time(nullptr);
	const std::tm *lt = std::localtime(&t);
	if (!lt) return;
	auto put2 = [&](uint8_t lo, int v) {
		Tama5SetRtcNib(s, lo, static_cast<uint8_t>(v % 10));
		Tama5SetRtcNib(s, static_cast<uint8_t>(lo + 1), static_cast<uint8_t>((v / 10) % 10));
	};
	put2(0x0, lt->tm_sec);
	put2(0x2, lt->tm_min);
	put2(0x4, lt->tm_hour);
	Tama5SetRtcNib(s, 0x6, static_cast<uint8_t>(lt->tm_wday));
	put2(0x7, lt->tm_mday);
	put2(0x9, lt->tm_mon + 1);
	put2(0xB, lt->tm_year % 100);
}

} // anonymous

void MbcReset(MbcState &s)
{
	g_cam_brightness = 0;
	s.rom_bank   = 1;
	s.ram_bank   = 0;
	// HuC1 RAM is always live — its $0000-$1FFF register only routes the
	// $A000-$BFFF window between RAM and the IR register — so default it
	// enabled. (Cart type is assigned before MbcReset runs.)
	s.ram_enable = (s.type == MbcType::HuC1);
	s.mbc1_mode  = false;
	s.rtc_latch  = false;
	s.rtc_select = 0;
	for (int i = 0; i < 5; ++i) { s.rtc_regs[i] = 0; s.rtc_latched[i] = 0; }
	s.sachen_outer_bank = 0;
	s.sachen_outer_mask = 0;
	s.sachen_locked     = true;
	s.sachen_unlock_ctr = 0;

	s.mmm01_locked            = false;
	s.mmm01_mbc1_mode         = false;
	s.mmm01_mbc1_mode_disable = false;
	s.mmm01_multiplex_mode    = false;
	s.mmm01_rom_bank_low      = 0;
	s.mmm01_rom_bank_mid      = 0;
	s.mmm01_rom_bank_high     = 0;
	s.mmm01_ram_bank_low      = 0;
	s.mmm01_ram_bank_high     = 0;
	s.mmm01_rom_bank_mask     = 0;
	s.mmm01_ram_bank_mask     = 0xFF;
	s.mmm01_just_locked       = false;

	if (s.type == MbcType::M161) s.rom_bank = 0;   // power-on page 0 = menu
	if (s.type == MbcType::TAMA5) Tama5SeedRtc(s);
	if (s.type == MbcType::MBC7)
	{
		s.rtc_regs[1] = 0xFF;
		s.rtc_regs[2] = 0xFF;
		s.rtc_select  = 0x01;
	}
	if (s.type == MbcType::MBC6)
	{
		s.rom_bank           = 2;
		s.mmm01_rom_bank_low = 3;
		s.ram_bank           = 0;
		s.mmm01_rom_bank_mid = 1;
	}
}

// MBC3 RTC — one-second cadence with the documented mask/carry quirks:
// registers hold their written bits (S/M 6-bit, H 5-bit, DH bits 0/6/7);
// out-of-range values count up and wrap through the field width without
// carrying, the carry chain only fires on the exact 59->60 / 23->24
// boundaries, and the 512-day overflow sets the sticky DH bit 7.
void MbcTickRtc(MbcState &s, int32_t tcycles)
{
	if (s.type != MbcType::MBC3) return;
	if (s.rtc_regs[4] & 0x40) return;   // halt bit freezes the oscillator
	s.rtc_sub_cycles += tcycles;
	while (s.rtc_sub_cycles >= 4194304)
	{
		s.rtc_sub_cycles -= 4194304;
		uint8_t sec = static_cast<uint8_t>((s.rtc_regs[0] + 1) & 0x3F);
		s.rtc_regs[0] = sec;
		if (sec != 60) continue;
		s.rtc_regs[0] = 0;
		uint8_t min = static_cast<uint8_t>((s.rtc_regs[1] + 1) & 0x3F);
		s.rtc_regs[1] = min;
		if (min != 60) continue;
		s.rtc_regs[1] = 0;
		uint8_t hour = static_cast<uint8_t>((s.rtc_regs[2] + 1) & 0x1F);
		s.rtc_regs[2] = hour;
		if (hour != 24) continue;
		s.rtc_regs[2] = 0;
		if (++s.rtc_regs[3] == 0)
		{
			if (s.rtc_regs[4] & 0x01)
				s.rtc_regs[4] = static_cast<uint8_t>((s.rtc_regs[4] & ~0x01) | 0x80);
			else
				s.rtc_regs[4] |= 0x01;
		}
	}
}

namespace {

inline uint32_t ReadRom(const std::vector<uint8_t> &rom, uint32_t offset)
{
	if (rom.empty()) return 0xFF;
	return rom[offset % rom.size()];
}

// Makon/NT "weird mode" bank-bit reorders and the bank-write transform
// (hhugboy's switchOrder + writeMemory, taizou's RE). reorder[x] names the
// source bit (from MSB) that lands in result bit 7-x.
inline uint8_t NtSwitchOrder(uint8_t v, const uint8_t *reorder)
{
	uint8_t out = 0;
	for (int x = 0; x < 8; ++x)
		out |= static_cast<uint8_t>(((v >> (7 - reorder[x])) & 1) << (7 - x));
	return out;
}

inline uint32_t NtBankValue(const MbcState &s, uint8_t value)
{
	static const uint8_t kFlipOld1[8] = { 0, 1, 2, 4, 3, 6, 5, 7 };
	static const uint8_t kFlipOld2[8] = { 0, 1, 2, 3, 4, 7, 5, 6 };
	uint8_t v = value;
	if (s.type == MbcType::NtOld1) v &= 0x1F;
	if (v == 0) v = 1;
	if (s.mbc1_mode)
		v = NtSwitchOrder(v, s.type == MbcType::NtOld1 ? kFlipOld1 : kFlipOld2);
	return v;
}

// $4000-$7FFF bank, masked to the sub-game window: sachen_outer_mask holds
// 1..5 = 32K<<n-1 sub-game size ($5002 write), 0 = whole file (power-on).
inline uint32_t NtBankN(const MbcState &s)
{
	uint32_t bank = s.rom_bank;
	if (s.sachen_outer_mask)
		bank &= (2u << (s.sachen_outer_mask - 1)) - 1;
	return bank + s.sachen_outer_bank * 2u;
}

// Scramble tables for the BBD-family and Sintax mappers (taizou's and
// NewRisingSun's RE in hhugboy; same reorder[x]-names-source-bit format).
constexpr uint8_t kBbdData[8][8] = {
	{0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7}, {0,1,5,6,4,2,3,7}, {0,1,2,3,4,5,6,7},
	{0,1,5,3,4,6,2,7}, {0,1,2,6,4,5,3,7}, {0,1,2,3,4,5,6,7}, {0,1,5,3,4,2,6,7},
};
constexpr uint8_t kBbdBank[8][8] = {
	{0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7}, {0,1,2,6,7,5,3,4},
	{0,1,2,3,4,5,6,7}, {0,1,2,7,3,4,5,6}, {0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7},
};
constexpr uint8_t kGgb81Data[8][8] = {
	{0,1,2,3,4,5,6,7}, {0,2,1,3,4,6,5,7}, {0,6,5,3,4,2,1,7}, {0,1,5,3,4,6,2,7},
	{0,1,6,3,4,5,2,7}, {0,6,2,3,4,1,5,7}, {0,2,5,3,4,1,6,7}, {0,6,1,3,4,2,5,7},
};
constexpr uint8_t kHitekData[8][8] = {
	{0,1,2,3,4,5,6,7}, {0,5,6,3,4,2,1,7}, {0,6,5,3,4,1,2,7}, {0,6,2,3,4,5,1,7},
	{0,5,2,3,4,6,1,7}, {0,5,2,3,4,1,6,7}, {0,2,6,3,4,1,5,7}, {0,2,6,3,4,5,1,7},
};
constexpr uint8_t kHitekBank[8][8] = {
	{0,1,2,3,4,5,6,7}, {0,1,2,3,7,6,5,4}, {0,1,2,3,4,7,6,5}, {0,1,2,3,5,4,7,6},
	{0,1,2,3,6,5,4,7}, {0,1,2,3,6,7,4,5}, {0,1,2,3,5,6,7,4}, {0,1,2,3,6,4,7,5},
};
// Sintax bank reorders keyed by the low nibble of the $5x1x mode.
inline const uint8_t *SintaxBankTable(uint8_t mode)
{
	static const uint8_t k00[8] = {0,7,2,1,4,3,6,5};
	static const uint8_t k01[8] = {7,6,1,0,3,2,5,4};
	static const uint8_t k05[8] = {0,1,6,7,4,5,2,3};
	static const uint8_t k07[8] = {5,7,4,6,2,3,0,1};
	static const uint8_t k09[8] = {3,2,5,4,7,6,1,0};
	static const uint8_t k0b[8] = {5,4,7,6,1,0,3,2};
	static const uint8_t k0d[8] = {6,7,0,1,2,3,4,5};
	static const uint8_t kid[8] = {0,1,2,3,4,5,6,7};
	switch (mode & 0x0F)
	{
		case 0x00: return k00;  case 0x01: return k01;
		case 0x05: return k05;  case 0x07: return k07;
		case 0x09: return k09;  case 0x0B: return k0b;
		case 0x0D: return k0d;  default:   return kid;
	}
}
// SkobLee8: modes 5 and 7 share one table, everything else is identity.
inline const uint8_t *SkobLeeBankTable(uint8_t mode)
{
	static const uint8_t k57[8] = {1,3,2,0,5,4,7,6};
	static const uint8_t kid[8] = {0,1,2,3,4,5,6,7};
	return ((mode & 0x07) == 5 || (mode & 0x07) == 7) ? k57 : kid;
}

// Banked-area read transform for the scrambled families.
inline uint8_t UnlDataTransform(const MbcState &s, const MbcUnl &u, uint8_t v)
{
	switch (s.type)
	{
		case MbcType::BBD:      return NtSwitchOrder(v, kBbdData[u.mode & 7]);
		case MbcType::Ggb81:    return NtSwitchOrder(v, kGgb81Data[u.mode & 7]);
		case MbcType::Hitek:    return NtSwitchOrder(v, kHitekData[u.mode & 7]);
		case MbcType::Sintax:
		case MbcType::SkobLee8: return static_cast<uint8_t>(v ^ u.cur_xor);
		default:                return v;
	}
}

inline uint8_t ReadSram(const std::vector<uint8_t> &sram, uint32_t offset)
{
	if (sram.empty()) return 0xFF;
	return sram[offset % sram.size()];
}

inline void WriteSram(Cart &c, uint32_t offset, uint8_t value)
{
	if (c.sram.empty()) return;
	uint8_t &cell = c.sram[offset % c.sram.size()];
	if (cell != value)
	{
		cell = value;
		c.sram_dirty = true;
	}
}

// Effective 0x0000-0x3FFF bank for MBC1 — normally 0, but mode 1 with
// a >= 1MB cart can expose banks 0x20/0x40/0x60. On a multicart the 2-bit
// BANK2 register drives one bit lower (A18-A19), so it shifts by 4 and the
// game slot's bank 0 (the menu / sub-game header) appears here.
inline uint32_t Mbc1Bank0(const MbcState &s, bool multicart)
{
	if (!s.mbc1_mode) return 0;
	return (s.ram_bank & 0x03) << (multicart ? 4 : 5);
}

inline uint32_t Mbc1BankN(const MbcState &s, bool multicart)
{
	uint32_t lo = s.rom_bank & 0x1F;
	if (lo == 0) lo = 1;
	// Multicart wiring leaves BANK1 only 4 effective bits and shifts BANK2
	// down to bits 4-5, so each BANK2 value selects a 256 KiB game slot.
	if (multicart)
		return (lo & 0x0F) | ((s.ram_bank & 0x03) << 4);
	return lo | ((s.ram_bank & 0x03) << 5);
}

inline uint32_t Mbc1RamBank(const MbcState &s, bool multicart)
{
	// Multicarts carry no cart RAM — BANK2 is the game selector, not a RAM bank.
	if (multicart) return 0;
	return s.mbc1_mode ? (s.ram_bank & 0x03) : 0;
}

// 23-in-1 multicart overlay: the latched base replaces the masked bank bits.
// mask defaults to 0, so a normal MBC5 cart passes rb through untouched.
inline uint32_t Mbc5MultiBank(const MbcState &s, uint32_t rb)
{
	const uint32_t mask = s.sachen_outer_mask;
	return (rb & ~mask) | (s.sachen_outer_bank & mask);
}

// base/mask address up to 32 Mbit, so a smaller cart can select a bank its ROM
// chip doesn't answer for — that reads open bus, not a mirror of bank 0.
inline bool SachenBankAbsent(const std::vector<uint8_t> &rom, uint32_t bank)
{
	return static_cast<size_t>(bank) * 0x4000u >= rom.size();
}

inline uint32_t SachenBank0(const MbcState &s)
{
	return static_cast<uint32_t>(s.sachen_outer_bank & s.sachen_outer_mask);
}

inline uint32_t SachenBankN(const MbcState &s)
{
	const uint32_t outer = s.sachen_outer_bank & s.sachen_outer_mask;
	const uint32_t inner = s.rom_bank & (~static_cast<uint32_t>(s.sachen_outer_mask) & 0xFFu);
	return outer | inner;
}

// Sachen MMC1 header bit-permutation: while locked, reads in 0x0100-0x01FF pass
// through the cart's A0/A6 and A1/A4 swaps so the boot ROM's logo check sees the
// real Nintendo logo at 0x0104-0x0133. Whether the lock survives the boot hand-
// off is per-cart — see Cart::sachen_runs_raw and gb_memory.cpp 0xFF50.
inline uint16_t SachenLockedHeaderXform(uint16_t addr)
{
	if ((addr & 0xFF00u) != 0x0100u) return addr;
	return static_cast<uint16_t>((addr & ~0x53u)
	                           | ((addr >> 6) & 0x01u)
	                           | ((addr >> 3) & 0x02u)
	                           | ((addr << 3) & 0x10u)
	                           | ((addr << 6) & 0x40u));
}

// MMM01 effective bank for the $0000-$3FFF region. While unlocked the
// mapper forces all outer ROM lines to 1 so the last 32 KiB of ROM
// (the menu) is exposed. Once locked, the menu has populated the base
// (rom_bank_mid/high) and mask (rom_bank_mask) registers; the masked
// bits of rom_bank_low get overlaid with the base so the sub-game can
// only switch within its allotted window.
inline uint32_t Mmm01Rom0Bank(const MbcState &s, size_t rom_size)
{
	if (!s.mmm01_locked)
	{
		const uint32_t banks = static_cast<uint32_t>(rom_size / 0x4000u);
		return banks >= 2 ? banks - 2u : 0u;
	}
	const uint8_t lowmask = static_cast<uint8_t>(s.mmm01_rom_bank_mask << 1);
	const uint8_t mid = s.mmm01_multiplex_mode && s.mmm01_mbc1_mode
	                  ? 0u
	                  : (s.mmm01_multiplex_mode ? s.mmm01_ram_bank_low : s.mmm01_rom_bank_mid);
	return static_cast<uint32_t>(
	          (s.mmm01_rom_bank_low & lowmask)
	        | (static_cast<uint32_t>(mid) << 5)
	        | (static_cast<uint32_t>(s.mmm01_rom_bank_high) << 7));
}

inline uint32_t Mmm01RomBank(const MbcState &s, size_t rom_size)
{
	if (!s.mmm01_locked)
	{
		const uint32_t banks = static_cast<uint32_t>(rom_size / 0x4000u);
		return banks >= 1 ? banks - 1u : 1u;
	}
	const uint8_t mid = s.mmm01_multiplex_mode ? s.mmm01_ram_bank_low : s.mmm01_rom_bank_mid;
	uint32_t bank = static_cast<uint32_t>(s.mmm01_rom_bank_low)
	              | (static_cast<uint32_t>(mid) << 5)
	              | (static_cast<uint32_t>(s.mmm01_rom_bank_high) << 7);
	if (bank == Mmm01Rom0Bank(s, rom_size)) ++bank;
	return bank;
}

inline uint32_t Mmm01RamBank(const MbcState &s)
{
	if (!s.mmm01_locked) return 0;
	if (s.mmm01_multiplex_mode)
		return static_cast<uint32_t>(s.mmm01_rom_bank_mid
		                           | (static_cast<uint32_t>(s.mmm01_ram_bank_high) << 2));
	return static_cast<uint32_t>(s.mmm01_ram_bank_low
	                           | (static_cast<uint32_t>(s.mmm01_ram_bank_high) << 2));
}

// ---- HuC3 ----------------------------------------------------------------
// HuC3 adds an RTC reached through a small command interface, plus IR. It
// never coexists with MBC3, so it borrows the (otherwise MBC3-only) RTC
// fields as its own state — keeping MbcState's size, and therefore the
// savestate layout, unchanged:
//   rtc_select     = $A000-$BFFF window mode (low nibble of $0000-$1FFF write)
//   rtc_regs[0]    = RTC register-map access pointer
//   rtc_regs[1]    = last extended-command argument ($2 = boot status request)
//   rtc_regs[2]    = response nibble (result of the last read command)
//   rtc_regs[3..4] = minutes (12-bit), rtc_latched[0..1] = days (16-bit)
inline uint16_t Huc3Minutes(const MbcState &s) { return static_cast<uint16_t>(s.rtc_regs[3] | (s.rtc_regs[4] << 8)); }
inline uint16_t Huc3Days(const MbcState &s)    { return static_cast<uint16_t>(s.rtc_latched[0] | (s.rtc_latched[1] << 8)); }
inline void Huc3SetMinutes(MbcState &s, uint16_t v) { s.rtc_regs[3] = v & 0xFF; s.rtc_regs[4] = (v >> 8) & 0x0F; }
inline void Huc3SetDays(MbcState &s, uint16_t v)    { s.rtc_latched[0] = v & 0xFF; s.rtc_latched[1] = (v >> 8) & 0xFF; }

inline uint8_t Huc3ReadReg(const MbcState &s, uint8_t idx)
{
	if (idx < 3) return (Huc3Minutes(s) >> (idx * 4)) & 0x0F;
	if (idx < 7) return (Huc3Days(s) >> ((idx - 3) * 4)) & 0x0F;
	return 0;   // alarm/tone registers ($58+) not modeled
}

inline void Huc3WriteReg(MbcState &s, uint8_t idx, uint8_t nib)
{
	nib &= 0x0F;
	if (idx < 3)
	{
		const uint16_t m = Huc3Minutes(s);
		Huc3SetMinutes(s, static_cast<uint16_t>((m & ~(0x0Fu << (idx * 4))) | (nib << (idx * 4))));
	}
	else if (idx < 7)
	{
		const uint16_t d = Huc3Days(s);
		Huc3SetDays(s, static_cast<uint16_t>((d & ~(0x0Fu << ((idx - 3) * 4))) | (nib << ((idx - 3) * 4))));
	}
	// idx >= 7 (alarm/tone) intentionally dropped
}

// Execute one command written to the $B window. Per SameBoy, HuC3 runs the
// command immediately on write; the $D semaphore then always reads ready.
inline void Huc3Command(MbcState &s, uint8_t value)
{
	uint8_t &idx = s.rtc_regs[0];
	const uint8_t arg = value & 0x0F;
	switch (value >> 4)
	{
		case 1: s.rtc_regs[2] = Huc3ReadReg(s, idx); ++idx;             break;  // read + advance
		case 2: Huc3WriteReg(s, idx, arg);                              break;  // write
		case 3: Huc3WriteReg(s, idx, arg); ++idx;                       break;  // write + advance
		case 4: idx = static_cast<uint8_t>((idx & 0xF0) | arg);         break;  // pointer low nibble
		case 5: idx = static_cast<uint8_t>((idx & 0x0F) | (arg << 4));  break;  // pointer high nibble
		case 6: s.rtc_regs[1] = arg;                                    break;  // extended command
		default:                                                        break;
	}
}

// TAMA5: a write to register 7 (ADDR_LO) commits the operation selected by the
// top 3 bits of the packed address byte — RAM write, RTC command, or RTC-page
// write. RAM reads (mode 1) are serviced lazily in MbcRead.
inline void Tama5Trigger(Cart &c)
{
	MbcState &s = c.mbc;
	const uint8_t addr5 = static_cast<uint8_t>(s.sachen_outer_mask & 0x1F);
	const uint8_t mode  = static_cast<uint8_t>(s.sachen_outer_mask >> 5);
	const uint8_t data  = s.sachen_outer_bank;
	switch (mode)
	{
		case 0x0:
			WriteSram(c, addr5, data);
			break;
		case 0x2:
			switch (addr5)
			{
				case 0x0: s.rtc_latch = false; break;
				case 0x1: s.rtc_latch = true;  break;
				case 0x4: Tama5SetRtcNib(s, 0x2, data & 0x0F); Tama5SetRtcNib(s, 0x3, static_cast<uint8_t>(data >> 4)); break;
				case 0x5: Tama5SetRtcNib(s, 0x4, data & 0x0F); Tama5SetRtcNib(s, 0x5, static_cast<uint8_t>(data >> 4)); break;
				default: break;
			}
			break;
		case 0x4:
			if ((s.sachen_outer_mask & 0x0F) == 0x0)
			{
				const uint8_t idx = static_cast<uint8_t>(data & 0x0F);
				if (idx < 0x0D) Tama5SetRtcNib(s, idx, static_cast<uint8_t>(data >> 4));
			}
			break;
		default:
			break;
	}
}

inline uint16_t Mbc7EepromWord(const std::vector<uint8_t> &sram, uint8_t word)
{
	const uint32_t off = static_cast<uint32_t>(word & 0x7F) * 2u;
	return static_cast<uint16_t>(ReadSram(sram, off) | (ReadSram(sram, off + 1) << 8));
}

inline void Mbc7EepromSetWord(Cart &c, uint8_t word, uint16_t value)
{
	const uint32_t off = static_cast<uint32_t>(word & 0x7F) * 2u;
	WriteSram(c, off,     static_cast<uint8_t>(value));
	WriteSram(c, off + 1, static_cast<uint8_t>(value >> 8));
}

inline void Mbc7EepromFill(Cart &c, uint16_t value)
{
	for (uint8_t w = 0; w < 0x80; ++w) Mbc7EepromSetWord(c, w, value);
}

void Mbc7EepromClock(Cart &c, uint8_t value)
{
	MbcState &s = c.mbc;
	const bool cs       = (value & 0x80) != 0;
	const bool di       = (value & 0x02) != 0;
	const bool clk      = (value & 0x40) != 0;
	const bool clk_prev = (s.rtc_select & 0x40) != 0;
	bool do_bit = (s.rtc_select & 0x01) != 0;

	if (cs && clk && !clk_prev)
	{
		uint16_t read_bits = static_cast<uint16_t>(s.rtc_regs[1] | (s.rtc_regs[2] << 8));
		uint16_t command   = static_cast<uint16_t>(s.rtc_regs[3] | (s.rtc_regs[4] << 8));
		uint8_t  args_left = s.rtc_regs[0];
		bool     we        = s.mmm01_mbc1_mode;

		do_bit    = (read_bits >> 15) & 1;
		read_bits = static_cast<uint16_t>((read_bits << 1) | 1);

		if (args_left == 0)
		{
			command = static_cast<uint16_t>(((command << 1) | (di ? 1 : 0)) & 0x7FF);
			if (command & 0x400)
			{
				const uint8_t word = static_cast<uint8_t>(command & 0x7F);
				switch ((command >> 6) & 0xF)
				{
					case 0x8: case 0x9: case 0xA: case 0xB:
						read_bits = Mbc7EepromWord(c.sram, word);
						command = 0;
						break;
					case 0x3: we = true;  command = 0; break;
					case 0x0: we = false; command = 0; break;
					case 0x4: case 0x5: case 0x6: case 0x7:
						if (we) Mbc7EepromSetWord(c, word, 0);
						args_left = 16;
						break;
					case 0xC: case 0xD: case 0xE: case 0xF:
						if (we) { Mbc7EepromSetWord(c, word, 0xFFFF); read_bits = 0x3FFF; }
						command = 0;
						break;
					case 0x2:
						if (we) { Mbc7EepromFill(c, 0xFFFF); read_bits = 0xFF; }
						command = 0;
						break;
					case 0x1:
						if (we) Mbc7EepromFill(c, 0);
						args_left = 16;
						break;
				}
			}
		}
		else
		{
			--args_left;
			do_bit = true;
			if (di && we)
			{
				const uint16_t bit = static_cast<uint16_t>(1u << args_left);
				if (command & 0x100)
				{
					const uint8_t w = static_cast<uint8_t>(command & 0x7F);
					Mbc7EepromSetWord(c, w, static_cast<uint16_t>(Mbc7EepromWord(c.sram, w) | bit));
				}
				else
				{
					for (uint8_t w = 0; w < 0x80; ++w)
						Mbc7EepromSetWord(c, w, static_cast<uint16_t>(Mbc7EepromWord(c.sram, w) | bit));
				}
			}
			if (args_left == 0) { command = 0; read_bits = 0x3FFF; }
		}

		s.rtc_regs[0] = args_left;
		s.rtc_regs[1] = static_cast<uint8_t>(read_bits);
		s.rtc_regs[2] = static_cast<uint8_t>(read_bits >> 8);
		s.rtc_regs[3] = static_cast<uint8_t>(command);
		s.rtc_regs[4] = static_cast<uint8_t>(command >> 8);
		s.mmm01_mbc1_mode = we;
	}

	s.rtc_select = static_cast<uint8_t>((do_bit ? 0x01 : 0) | (di ? 0x02 : 0) | (clk ? 0x40 : 0) | (cs ? 0x80 : 0));
}

uint8_t Mbc6Read(const MbcState &s, const std::vector<uint8_t> &rom, const std::vector<uint8_t> &sram, uint16_t addr)
{
	if (addr < 0x4000)
		return static_cast<uint8_t>(ReadRom(rom, addr));
	if (addr < 0x6000)
		return s.mbc1_mode ? 0xFF : static_cast<uint8_t>(ReadRom(rom, (s.rom_bank * 0x2000u) + (addr - 0x4000u)));
	if (addr < 0x8000)
		return s.mmm01_mbc1_mode ? 0xFF : static_cast<uint8_t>(ReadRom(rom, (s.mmm01_rom_bank_low * 0x2000u) + (addr - 0x6000u)));
	if (addr >= 0xA000 && addr < 0xB000)
		return (s.ram_enable && !sram.empty()) ? ReadSram(sram, (s.ram_bank * 0x1000u) + (addr - 0xA000u)) : 0xFF;
	if (addr >= 0xB000 && addr < 0xC000)
		return (s.ram_enable && !sram.empty()) ? ReadSram(sram, (s.mmm01_rom_bank_mid * 0x1000u) + (addr - 0xB000u)) : 0xFF;
	return 0xFF;
}

} // anonymous


// Zook Z (USA): 4-byte codes at $7081 select a bank (first three matter) and
// $7x80 sequences are answered at $Ax80; both tables come from the Taiwanese build.
struct VfzBankCode { uint8_t b0, b1, b2, bank; };
static const VfzBankCode kVfzBankCodes[] = {
	{ 0x00, 0x1B, 0x0F, 0x17 }, { 0x00, 0x50, 0x06, 0x30 }, { 0x01, 0x1A, 0x11, 0x14 }, { 0x04, 0x1F, 0x30, 0x13 },
	{ 0x04, 0x32, 0x09, 0x2A }, { 0x04, 0x32, 0x0C, 0x2D }, { 0x0A, 0x31, 0x18, 0x0B }, { 0x0B, 0x15, 0x39, 0x07 },
	{ 0x0C, 0x31, 0x10, 0x0C }, { 0x0F, 0x32, 0x1E, 0x0B }, { 0x10, 0x24, 0x10, 0x09 }, { 0x10, 0x24, 0x19, 0x0C },
	{ 0x10, 0x24, 0x26, 0x06 }, { 0x10, 0x4B, 0x10, 0x12 }, { 0x10, 0x4B, 0x15, 0x11 }, { 0x10, 0x4B, 0x1E, 0x1C },
	{ 0x12, 0x51, 0x2B, 0x2B }, { 0x13, 0x50, 0x1C, 0x2B }, { 0x14, 0x20, 0x30, 0x02 }, { 0x16, 0x22, 0x16, 0x0A },
	{ 0x18, 0x43, 0x1C, 0x1E }, { 0x19, 0x22, 0x1E, 0x0C }, { 0x1A, 0x21, 0x1B, 0x05 }, { 0x1A, 0x51, 0x2E, 0x28 },
	{ 0x1C, 0x21, 0x2E, 0x01 }, { 0x1C, 0x50, 0x2F, 0x2B }, { 0x1C, 0x7A, 0x2C, 0x37 }, { 0x1D, 0x46, 0x3C, 0x17 },
	{ 0x1F, 0x44, 0x3E, 0x18 }, { 0x20, 0x70, 0x43, 0x31 }, { 0x21, 0x3A, 0x29, 0x18 }, { 0x22, 0x3C, 0x30, 0x03 },
	{ 0x22, 0x3C, 0x4D, 0x01 }, { 0x22, 0x64, 0x25, 0x1B }, { 0x23, 0x3D, 0x4D, 0x01 }, { 0x24, 0x3F, 0x2F, 0x14 },
	{ 0x24, 0x62, 0x27, 0x1D }, { 0x26, 0x50, 0x51, 0x27 }, { 0x27, 0x39, 0x4D, 0x01 }, { 0x28, 0x78, 0x2C, 0x30 },
	{ 0x2A, 0x5C, 0x3B, 0x25 }, { 0x2B, 0x35, 0x3E, 0x05 }, { 0x2B, 0x6D, 0x38, 0x12 }, { 0x2C, 0x5A, 0x2D, 0x29 },
	{ 0x2C, 0x6A, 0x33, 0x10 }, { 0x30, 0x44, 0x34, 0x0B }, { 0x30, 0x6B, 0x36, 0x1C }, { 0x30, 0x6B, 0x49, 0x16 },
	{ 0x30, 0x93, 0x31, 0x36 }, { 0x32, 0x71, 0x32, 0x29 }, { 0x34, 0x40, 0x5A, 0x03 }, { 0x38, 0x63, 0x39, 0x1A },
	{ 0x38, 0x63, 0x3E, 0x18 }, { 0x3A, 0x41, 0x3F, 0x02 }, { 0x3C, 0x47, 0x5E, 0x12 }, { 0x3E, 0x65, 0x49, 0x16 },
	{ 0x3F, 0x42, 0x52, 0x03 }, { 0x40, 0x5E, 0x55, 0x02 }, { 0x40, 0x76, 0x55, 0x2B }, { 0x42, 0x84, 0x47, 0x18 },
	{ 0x43, 0x5D, 0x4A, 0x07 }, { 0x44, 0x5F, 0x57, 0x18 }, { 0x46, 0x58, 0x54, 0x04 }, { 0x46, 0x70, 0x48, 0x22 },
	{ 0x47, 0x71, 0x4B, 0x25 }, { 0x48, 0x7E, 0x4C, 0x2D }, { 0x48, 0x8E, 0x4C, 0x14 }, { 0x48, 0x8E, 0x56, 0x1D },
	{ 0x4B, 0x55, 0x54, 0x04 }, { 0x4C, 0x8A, 0x50, 0x10 }, { 0x4D, 0x53, 0x51, 0x06 }, { 0x4D, 0x8B, 0x4E, 0x18 },
	{ 0x4E, 0x50, 0x50, 0x01 }, { 0x50, 0x8B, 0x67, 0x10 }, { 0x52, 0x91, 0x59, 0x28 }, { 0x54, 0x60, 0x59, 0x0C },
	{ 0x57, 0x94, 0x61, 0x2A }, { 0x58, 0x63, 0x5F, 0x09 }, { 0x5A, 0x61, 0x62, 0x03 }, { 0x5A, 0x61, 0x70, 0x02 },
	{ 0x5A, 0x8A, 0x63, 0x30 }, { 0x5C, 0x61, 0x5D, 0x05 }, { 0x5D, 0x86, 0x6F, 0x14 }, { 0x5E, 0x92, 0x6B, 0x27 },
	{ 0x60, 0xA6, 0x6B, 0x1C }, { 0x63, 0x7D, 0x65, 0x06 }, { 0x64, 0x92, 0x66, 0x2D }, { 0x67, 0xA1, 0x74, 0x18 },
	{ 0x68, 0x76, 0x7E, 0x05 }, { 0x68, 0xAE, 0x6C, 0x1E }, { 0x68, 0xB8, 0x77, 0x34 }, { 0x6C, 0x91, 0x71, 0x09 },
	{ 0x6C, 0x9A, 0x72, 0x22 }, { 0x6C, 0x9A, 0x89, 0x2A }, { 0x6C, 0x9A, 0x8C, 0x2D }, { 0x6C, 0xAA, 0x6E, 0x14 },
	{ 0x6D, 0xAB, 0x73, 0x10 }, { 0x70, 0xB3, 0x7C, 0x2A }, { 0x72, 0x86, 0x77, 0x02 }, { 0x72, 0x86, 0x7D, 0x04 },
	{ 0x73, 0xB0, 0x7B, 0x25 }, { 0x76, 0x82, 0x76, 0x06 }, { 0x76, 0xB5, 0x78, 0x26 }, { 0x78, 0x83, 0x84, 0x0A },
	{ 0x78, 0x8C, 0xA3, 0x0F }, { 0x78, 0xA3, 0x82, 0x13 }, { 0x78, 0xB3, 0x95, 0x2F }, { 0x7A, 0xA1, 0x88, 0x11 },
	{ 0x7C, 0x87, 0x7D, 0x09 }, { 0x7D, 0x89, 0x82, 0x0C }, { 0x7E, 0x83, 0x7E, 0x02 }, { 0x7E, 0x83, 0x80, 0x05 },
	{ 0x7E, 0x85, 0x80, 0x06 }, { 0x81, 0x9F, 0x95, 0x02 }, { 0x88, 0xCE, 0x8E, 0x18 }, { 0x8C, 0x92, 0x8D, 0x01 },
	{ 0x8C, 0xBA, 0x8F, 0x2E }, { 0x8C, 0xCA, 0xAC, 0x1E }, { 0x90, 0xCB, 0x91, 0x14 }, { 0x90, 0xD3, 0x93, 0x2D },
	{ 0x91, 0xCA, 0xBC, 0x17 }, { 0x95, 0xCE, 0x97, 0x13 }, { 0x96, 0xCD, 0xA4, 0x1A }, { 0x96, 0xD5, 0x9F, 0x2D },
	{ 0x98, 0xC3, 0x99, 0x16 }, { 0x98, 0xC3, 0xA1, 0x1D }, { 0x9A, 0xD1, 0xAB, 0x25 }, { 0x9B, 0xD0, 0xAF, 0x29 },
	{ 0x9C, 0xD0, 0xA8, 0x29 }, { 0x9C, 0xFA, 0xBE, 0x33 }, { 0x9D, 0xC6, 0xA1, 0x1D }, { 0xA0, 0xD6, 0xAD, 0x29 },
	{ 0xA1, 0xBA, 0xB0, 0x13 }, { 0xA4, 0xBA, 0xA8, 0x0C }, { 0xA4, 0xBA, 0xD5, 0x02 }, { 0xA7, 0xB9, 0xAC, 0x09 },
	{ 0xA9, 0xDF, 0xBC, 0x18 }, { 0xAA, 0xDC, 0xB6, 0x2B }, { 0xAA, 0xDC, 0xE1, 0x2F }, { 0xAB, 0xFA, 0xAF, 0x32 },
	{ 0xAC, 0xB2, 0xCC, 0x06 }, { 0xAC, 0xEA, 0xC8, 0x11 }, { 0xAF, 0xE9, 0xB3, 0x10 }, { 0xB0, 0xC4, 0xBA, 0x01 },
	{ 0xB0, 0xEB, 0xB8, 0x1B }, { 0xB0, 0xF3, 0xBA, 0x2E }, { 0xB4, 0xF7, 0xB5, 0x2C }, { 0xB8, 0xE3, 0xBC, 0x17 },
	{ 0xB8, 0xE3, 0xBF, 0x14 }, { 0xBC, 0xE7, 0xC2, 0x13 }, { 0xBE, 0xF2, 0xD4, 0x23 }, { 0xC0, 0xDB, 0xC2, 0x13 },
	{ 0xC0, 0xDE, 0xD4, 0x04 }, { 0xC0, 0xF6, 0xC4, 0x29 }, { 0xC1, 0xF7, 0xCB, 0x25 }, { 0xC3, 0xDD, 0xCF, 0x0C },
	{ 0xC7, 0xD9, 0xCD, 0x01 }, { 0xC8, 0x0E, 0xD3, 0x1A }, { 0xC8, 0x0E, 0xD7, 0x1C }, { 0xC8, 0x18, 0xDD, 0x35 },
	{ 0xC8, 0xFE, 0xCD, 0x26 }, { 0xCB, 0xFD, 0xD0, 0x2A }, { 0xCC, 0x0A, 0xD5, 0x11 }, { 0xCC, 0xD2, 0xD3, 0x05 },
	{ 0xCC, 0xFA, 0xD5, 0x2B }, { 0xCC, 0xFA, 0xDA, 0x29 }, { 0xD0, 0x0B, 0xD3, 0x18 }, { 0xD3, 0x10, 0xE9, 0x2E },
	{ 0xD3, 0xE7, 0xE9, 0x01 }, { 0xD4, 0x17, 0xDC, 0x2B }, { 0xD4, 0x17, 0xE9, 0x2E }, { 0xD4, 0xE0, 0xE7, 0x01 },
	{ 0xD5, 0x0E, 0xD9, 0x16 }, { 0xD6, 0x0D, 0xE7, 0x10 }, { 0xD8, 0xE3, 0xE9, 0x06 }, { 0xD9, 0x02, 0xDA, 0x18 },
	{ 0xDB, 0x00, 0xDD, 0x16 }, { 0xDB, 0xE0, 0xE2, 0x03 }, { 0xDC, 0x07, 0xFC, 0x17 }, { 0xDC, 0x0C, 0xE3, 0x30 },
	{ 0xDE, 0x05, 0x08, 0x11 }, { 0xE2, 0xFC, 0x15, 0x02 }, { 0xE5, 0xFB, 0xE6, 0x05 }, { 0xE6, 0x10, 0xE7, 0x2C },
	{ 0xE7, 0xFC, 0xF0, 0x13 }, { 0xE8, 0x2E, 0xF1, 0x16 }, { 0xE8, 0x2E, 0xFB, 0x15 }, { 0xEC, 0x11, 0xF1, 0x09 },
	{ 0xEC, 0xF2, 0x0C, 0x06 }, { 0xEF, 0x19, 0xFC, 0x2A }, { 0xF2, 0x06, 0xF9, 0x05 }, { 0xF3, 0x07, 0xF9, 0x05 },
	{ 0xF4, 0x12, 0xFB, 0x38 }, { 0xF6, 0x02, 0xF7, 0x02 }, { 0xF6, 0x02, 0xFC, 0x09 }, { 0xF6, 0x10, 0x13, 0x30 },
	{ 0xF8, 0x03, 0xFB, 0x0B }, { 0xFB, 0x00, 0x28, 0x01 }, { 0xFB, 0x20, 0x1C, 0x1E }, { 0xFC, 0x01, 0x12, 0x03 },
	{ 0xFC, 0x01, 0x2E, 0x01 }, { 0xFC, 0x0A, 0x09, 0x2A }, { 0xFC, 0x1A, 0x28, 0x31 }, { 0xFC, 0x27, 0xFE, 0x18 },
	{ 0xFE, 0x25, 0xFF, 0x17 }, { 0xFF, 0x02, 0xFF, 0x06 },
};

struct VfzReply { uint8_t len, reply; uint8_t seq[32]; };
static const VfzReply kVfzReplies[] = {
	{  6, 0x11, { 0x20, 0x12, 0xB8, 0x12, 0x8B, 0xA0 } },
	{  4, 0xC6, { 0x9B, 0x12, 0xA9, 0x88 } },
	{  4, 0x11, { 0x77, 0x45, 0x03, 0x96 } },
	{  4, 0x85, { 0xA8, 0x12, 0xA9, 0x9C } },
	{  6, 0x6B, { 0x20, 0x12, 0x30, 0x12, 0x56, 0x8D } },
	{  6, 0x93, { 0x14, 0x15, 0x16, 0x17, 0xA8, 0x8F } },
	{  5, 0x5D, { 0x33, 0x80, 0x60, 0x95, 0x85 } },
	{  2, 0x6E, { 0xA8, 0xB6 } },
	{  2, 0x19, { 0x20, 0x96 } },
	{  4, 0xC6, { 0x20, 0x12, 0x56, 0x88 } },
	{  4, 0x00, { 0x75, 0x12, 0xB8, 0x9C } },
	{  1, 0x00, { 0xB7 } },
	{  2, 0x57, { 0xA8, 0x8C } },
	{  4, 0x22, { 0xB9, 0x12, 0x12, 0xB4 } },
	{  2, 0x14, { 0xB9, 0xAA } },
	{  8, 0x4B, { 0x04, 0x0A, 0x76, 0xC4, 0x70, 0x66, 0x20, 0xB0 } },
	{  8, 0xA7, { 0x75, 0x12, 0x33, 0x9A, 0x12, 0x64, 0x98, 0x9C } },
	{  8, 0x9C, { 0x9A, 0x98, 0x12, 0x55, 0x70, 0x35, 0x9B, 0xBE } },
	{  8, 0x72, { 0x21, 0x66, 0x97, 0x85, 0x60, 0x21, 0x25, 0xBD } },
	{  8, 0x78, { 0x55, 0x12, 0x56, 0x30, 0x32, 0x12, 0x21, 0x87 } },
	{  8, 0x05, { 0x76, 0x33, 0xA5, 0xB5, 0x88, 0xC0, 0x1F, 0xAD } },
	{  8, 0x08, { 0x33, 0xB0, 0x95, 0xB8, 0x45, 0x99, 0x16, 0xB4 } },
	{  8, 0x08, { 0x30, 0x22, 0x11, 0x20, 0x15, 0x42, 0x23, 0xB4 } },
	{  8, 0x0F, { 0x65, 0x88, 0x46, 0x12, 0x12, 0xA8, 0x32, 0x9E } },
	{  8, 0x05, { 0x10, 0xA0, 0x83, 0x65, 0x77, 0x33, 0x23, 0x9E } },
	{  6, 0xD0, { 0x11, 0x81, 0x70, 0xF7, 0xEA, 0x98 } },
	{ 26, 0xA0, { 0x09, 0x19, 0x11, 0x81, 0x70, 0xF7, 0x2A, 0x66, 0x6F, 0xCD, 0x75, 0x06, 0xC9, 0x23, 0x3D, 0x4D, 0xA0, 0xF0, 0x40, 0xA7, 0xB9, 0xAC, 0xD8, 0x09, 0x42, 0x99 } },
	{  3, 0x22, { 0x77, 0x13, 0xB4 } },
	{  5, 0x80, { 0x1A, 0x77, 0x13, 0x1A, 0x9B } },
	{  5, 0x64, { 0x36, 0x31, 0xFB, 0xFE, 0x82 } },
	{  5, 0x13, { 0x36, 0x31, 0xFB, 0xFE, 0xA0 } },
	{ 17, 0xF0, { 0xE0, 0xA7, 0xE7, 0x34, 0x40, 0x5A, 0x33, 0xFA, 0x19, 0xD0, 0x6F, 0xFA, 0x1A, 0xD0, 0x67, 0xF0, 0xBA } },
	{  9, 0x44, { 0xE7, 0xA4, 0xBA, 0xD5, 0x44, 0xF0, 0x9B, 0xFE, 0x82 } },
	{ 14, 0x64, { 0xE7, 0x7E, 0x85, 0x80, 0x64, 0xCD, 0xA8, 0x75, 0xF0, 0xC2, 0xFE, 0x1E, 0x38, 0xA2 } },
	{ 12, 0x3B, { 0xE7, 0x7A, 0xA1, 0x88, 0x02, 0xCD, 0x00, 0x40, 0xF1, 0xEF, 0xC9, 0xA8 } },
	{  8, 0x08, { 0xE7, 0xC8, 0x0E, 0xD3, 0x27, 0xCD, 0x4F, 0x99 } },
	{ 15, 0x11, { 0xFA, 0x80, 0xA0, 0xD6, 0x50, 0x36, 0x31, 0xFB, 0xE0, 0x9B, 0x3E, 0x0E, 0xE0, 0xA3, 0xBC } },
	{  8, 0x46, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0xA8, 0x36, 0x8E } },
	{ 19, 0x88, { 0x2A, 0x12, 0x2A, 0x12, 0x2A, 0x12, 0x2A, 0x12, 0x2A, 0x12, 0x2A, 0x12, 0x2A, 0x12, 0xFA, 0x80, 0xA0, 0x86, 0x9B } },
	{  4, 0x66, { 0x87, 0x5F, 0x16, 0x82 } },
	{  3, 0x58, { 0x22, 0x64, 0xBD } },
	{  8, 0x08, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0xB9, 0x36, 0x84 } },
	{  8, 0x05, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0xB9, 0x36, 0x9E } },
	{ 30, 0x80, { 0x3E, 0x09, 0xCD, 0x17, 0x3F, 0xE0, 0xA5, 0xAF, 0xE0, 0xA6, 0xE0, 0xAB, 0x3E, 0x01, 0xE0, 0xAA, 0xF0, 0x9B, 0xFE, 0x08, 0xC0, 0x3E, 0x01, 0xEA, 0x7C, 0xD1, 0x3E, 0x02, 0xE0, 0x84 } },
	{ 29, 0x0D, { 0x5F, 0x16, 0x00, 0x19, 0x11, 0x81, 0x70, 0xF7, 0xC9, 0xE7, 0xE5, 0xFB, 0xE6, 0x05, 0xCD, 0x3F, 0x45, 0xCD, 0xEA, 0x44, 0xCD, 0x00, 0x40, 0xCD, 0x7F, 0x14, 0xFA, 0xF5, 0xB8 } },
	{ 15, 0x25, { 0xE7, 0x7E, 0x83, 0x80, 0x5D, 0xCD, 0x64, 0x5B, 0xCD, 0x2B, 0x57, 0xCD, 0xF6, 0x53, 0xB8 } },
	{ 32, 0x16, { 0xE7, 0xAB, 0xFA, 0xAF, 0x3B, 0xCD, 0x00, 0x40, 0xCD, 0xBA, 0x41, 0xE7, 0x78, 0x83, 0x84, 0x99, 0xF0, 0x9B, 0xFE, 0x0C, 0x38, 0x03, 0x3E, 0x1A, 0xEF, 0xCD, 0x00, 0x40, 0x3E, 0x05, 0xEF, 0x80 } },
	{  8, 0xD5, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0xA8, 0x36, 0xAE } },
	{ 17, 0x19, { 0x11, 0x81, 0x70, 0xF7, 0x2A, 0x66, 0x6F, 0xE9, 0x90, 0xD3, 0x93, 0x56, 0x5D, 0x71, 0x50, 0x43, 0xBC } },
	{  6, 0x93, { 0x3E, 0x04, 0xCD, 0x17, 0x3F, 0x8F } },
	{  7, 0x75, { 0x2A, 0x12, 0xFA, 0x80, 0xA0, 0x86, 0x8C } },
	{ 10, 0x7D, { 0xCD, 0xBB, 0x57, 0xE7, 0xFC, 0x01, 0x12, 0x5F, 0x21, 0xAE } },
	{ 24, 0x09, { 0xE7, 0x3A, 0x41, 0x3F, 0x64, 0xCD, 0x00, 0x40, 0xAF, 0xE0, 0xA5, 0xE0, 0xA6, 0x3E, 0x01, 0xE0, 0xAA, 0x3E, 0xE4, 0xE0, 0x47, 0x3E, 0xE1, 0xB4 } },
	{  4, 0x66, { 0xAF, 0xCD, 0x17, 0xBB } },
	{  8, 0x08, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0x20, 0x36, 0x84 } },
	{  9, 0x99, { 0xE7, 0x22, 0x3C, 0x30, 0xAC, 0x2A, 0x66, 0x6F, 0x94 } },
	{ 26, 0x33, { 0x29, 0x29, 0x09, 0x09, 0x19, 0x11, 0x81, 0x70, 0xF7, 0x2A, 0x66, 0x6F, 0xE9, 0x00, 0x50, 0x06, 0x77, 0x30, 0x68, 0xDC, 0x0C, 0xE3, 0x66, 0x4F, 0x69, 0x94 } },
	{  5, 0x13, { 0xFB, 0xE0, 0x40, 0xF3, 0x92 } },
	{  8, 0x05, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0x9B, 0x36, 0x9E } },
	{ 12, 0x4C, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0x77, 0x36, 0x45, 0x36, 0x03, 0x36, 0xA2 } },
	{  8, 0x3C, { 0xF3, 0x21, 0x80, 0x70, 0x36, 0xA8, 0x36, 0xB7 } },
	{  2, 0x57, { 0xCE, 0x8A } },
	{  1, 0xAF, { 0x9C } },
	{  1, 0xDF, { 0x8C } },
};

static bool VfzBankFor(const uint8_t *code, uint32_t &bank)
{
	for (const VfzBankCode &e : kVfzBankCodes)
		if (e.b0 == code[0] && e.b1 == code[1] && e.b2 == code[2]) { bank = e.bank; return true; }
	return false;
}

static uint8_t VfzReplyFor(const MbcUnl &u)
{
	const char *trace = getenv("VFZ_TRACE");
	for (const VfzReply &e : kVfzReplies)
		if (e.len == u.vfz_len && std::memcmp(e.seq, u.vfz_seq, e.len) == 0)
		{
			if (trace && atoi(trace) >= 2)
			{
				fprintf(stderr, "vfz: reply %02X for", e.reply);
				for (int i = 0; i < u.vfz_len; ++i) fprintf(stderr, " %02X", u.vfz_seq[i]);
				fprintf(stderr, "\n");
			}
			return e.reply;
		}
	if (trace)
	{
		fprintf(stderr, "vfz: unknown sequence");
		for (int i = 0; i < u.vfz_len; ++i) fprintf(stderr, " %02X", u.vfz_seq[i]);
		fprintf(stderr, "\n");
	}
	return 0x00;
}

uint8_t MbcRead(MbcState &s, const std::vector<uint8_t> &rom, const std::vector<uint8_t> &sram, uint16_t addr, bool mbc1_multicart, MbcUnl *unl)
{
	if (s.type == MbcType::MBC6)
		return Mbc6Read(s, rom, sram, addr);
	// Zook Z: the chip answers a challenge at $Ax80; the read closes the
	// sequence so the game's trailing $31 write starts nothing.
	if (s.type == MbcType::Vf001 && unl && unl->vfz_on)
	{
		if ((addr & 0xF0FF) == 0xA080)
		{
			unl->vfz_after_read = 1;
			return VfzReplyFor(*unl);
		}
		// Nothing resets the code shift register: every transfer is exactly
		// four writes, and both helpers run with interrupts enabled while the
		// VBlank handler reads the bank marker at $7FFF, so a reset keyed on
		// that read would land mid-transfer and mangle the code.
	}
	// Vast Fame protection: an armed byte sequence hijacks ROM reads once
	// its trigger address is read, and the bank-0 overlay window shadows
	// part of $0000-$3FFF from another bank.
	if (s.type == MbcType::Vf001 && unl && addr < 0x8000)
	{
		MbcUnl &u = *unl;
		const bool trigger_zone =
			(u.vf_seq_bank == 0 && addr < 0x4000) ||
			(u.vf_seq_bank == s.rom_bank && addr >= 0x4000);
		if (trigger_zone && addr == u.vf_seq_addr &&
		    u.vf_seq_left == 0 && u.vf_seq_len)
			u.vf_seq_left = u.vf_seq_len;
		if (u.vf_seq_left > 0)
		{
			--u.vf_seq_left;
			return u.vf_seq[u.vf_seq_len - u.vf_seq_left - 1];
		}
		if (u.vf_repl_on && addr >= u.vf_repl_addr && addr < 0x4000)
			return static_cast<uint8_t>(ReadRom(rom,
				(static_cast<uint32_t>(u.vf_repl_bank) << 14) + addr));
	}
	// Makon NT-new split mode: each 8K half of the switchable window banks
	// independently once its register has been written.
	if (s.type == MbcType::NtNew && unl && unl->mode &&
	    addr >= 0x4000 && addr < 0x8000)
	{
		const bool low = addr < 0x6000;
		if (low ? unl->xors[2] : unl->xors[3])
		{
			uint32_t off = static_cast<uint32_t>(low ? unl->xors[0] : unl->xors[1]) << 13;
			if (!rom.empty()) off %= rom.size();
			if (off < 0x4000) off += 0x4000;   // banks 0-1 not selectable
			return static_cast<uint8_t>(ReadRom(rom, off + (addr & 0x1FFFu)));
		}
	}
	if (s.type == MbcType::M161 && addr < 0x8000)
	{
		// One 32 KiB page covers the whole $0000-$7FFF window.
		const uint32_t page = s.rom_bank & 0x07;
		return static_cast<uint8_t>(ReadRom(rom, (page << 15) | (addr & 0x7FFF)));
	}
	if (addr < 0x4000)
	{
		// Bank 0 region — mostly direct, except for MBC1 mode 1 quirk,
		// Sachen MMC1 outer-bank/header xform, and MMM01 menu mapping.
		uint32_t bank = 0;
		uint16_t eff_addr = addr;
		if (s.type == MbcType::MBC1)
		{
			bank = Mbc1Bank0(s, mbc1_multicart);
		}
		else if (s.type == MbcType::SachenMMC1)
		{
			bank = SachenBank0(s);
			if (s.sachen_locked) eff_addr = SachenLockedHeaderXform(addr);
			if (SachenBankAbsent(rom, bank)) return 0xFF;
		}
		else if (s.type == MbcType::MMM01)
		{
			bank = Mmm01Rom0Bank(s, rom.size());
		}
		else if (s.type == MbcType::MBC3)
		{
			bank = s.sachen_outer_bank;   // Duz multicart base bank; 0 for normal MBC3
		}
		else if (s.type == MbcType::MBC5)
		{
			bank = Mbc5MultiBank(s, 0);   // no-op until a 23-in-1 menu sets the mask
		}
		else if (s.type == MbcType::Rocket)
		{
			bank = s.sachen_outer_bank;   // 2-in-1 outer bank moves the fixed area too
		}
		else if (s.type == MbcType::NtOld1 || s.type == MbcType::NtOld2)
		{
			bank = s.sachen_outer_bank * 2u;   // multicart 32K page base
		}
		return static_cast<uint8_t>(ReadRom(rom, (bank * 0x4000u) + eff_addr));
	}
	if (addr < 0x8000)
	{
		uint32_t bank = 1;
		switch (s.type)
		{
			case MbcType::MBC1: bank = Mbc1BankN(s, mbc1_multicart); break;
			case MbcType::MBC3: bank = (s.rom_bank ? s.rom_bank : 1) + s.sachen_outer_bank; break;
			case MbcType::MBC5: bank = Mbc5MultiBank(s, s.rom_bank); break;
			case MbcType::MBC7: bank = s.rom_bank; break;
			case MbcType::HuC1: bank = s.rom_bank ? s.rom_bank : 1; break;
			case MbcType::HuC3: bank = s.rom_bank; break;
			case MbcType::TAMA5: bank = s.rom_bank; break;
			case MbcType::Camera: bank = s.rom_bank ? s.rom_bank : 1; break;
			case MbcType::MBC2: bank = (s.rom_bank & 0x0F) ? (s.rom_bank & 0x0F) : 1; break;
			case MbcType::SachenMMC1: bank = SachenBankN(s); break;
			case MbcType::MMM01:      bank = Mmm01RomBank(s, rom.size()); break;
			case MbcType::Rocket:
				bank = s.sachen_outer_bank | (s.rom_bank ? s.rom_bank : 1);
				break;
			case MbcType::NtOld1:
			case MbcType::NtOld2: bank = NtBankN(s); break;
			case MbcType::BBD:
			case MbcType::Ggb81:
			case MbcType::Hitek:
			case MbcType::Sintax:
			case MbcType::SkobLee8:
			case MbcType::LiCheng:
			case MbcType::NtNew:
			case MbcType::Vf001:  bank = s.rom_bank; break;
			case MbcType::PokeJadeDia:
				bank = s.rom_bank ? s.rom_bank : 1;
				break;
			default:            bank = 1; break;
		}
		if (s.type == MbcType::SachenMMC1 && SachenBankAbsent(rom, bank))
			return 0xFF;
		uint8_t v = static_cast<uint8_t>(ReadRom(rom, (bank * 0x4000u) + (addr - 0x4000u)));
		if (unl) v = UnlDataTransform(s, *unl, v);
		return v;
	}
	if (addr >= 0xA000 && addr < 0xC000)
	{
		// HuC1 routes this window between cart RAM and the IR register.
		// ram_enable doubles as the RAM/IR select (true = RAM, false = IR).
		// With no link partner the IR sensor idles at $C0 (no light).
		if (s.type == MbcType::HuC1)
		{
			if (!s.ram_enable) return 0xC0;
			return ReadSram(sram, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u));
		}

		// HuC3 multiplexes RAM, the RTC command/response, a ready semaphore
		// and IR onto this window, selected by the mode set at $0000-$1FFF.
		if (s.type == MbcType::HuC3)
		{
			switch (s.rtc_select)
			{
				case 0x0:        // RAM, read-only
				case 0xA:        // RAM, read/write
					return ReadSram(sram, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u));
				case 0xC:        // command response — $62 status must read back 1
					return (s.rtc_regs[1] == 0x02) ? 0x01 : s.rtc_regs[2];
				case 0xD: return 0x01;   // semaphore: always ready
				case 0xE: return 0x00;   // IR: no link partner
				default:  return 0xFF;   // open bus
			}
		}

		if (s.type == MbcType::TAMA5)
		{
			if (addr & 1) return 0xFF;
			const uint8_t addr5 = static_cast<uint8_t>(s.sachen_outer_mask & 0x1F);
			const uint8_t mode  = static_cast<uint8_t>(s.sachen_outer_mask >> 5);
			if (s.rtc_select == 0x0A) return 0xF1;
			if (s.rtc_select == 0x0C || s.rtc_select == 0x0D)
			{
				uint8_t value = 0x0F;
				if (mode == 0x1)
					value = ReadSram(sram, addr5);
				else if (mode == 0x2)
					value = (addr5 == 0x6) ? static_cast<uint8_t>((Tama5RtcNib(s, 0x3) << 4) | Tama5RtcNib(s, 0x2))
					      : (addr5 == 0x7) ? static_cast<uint8_t>((Tama5RtcNib(s, 0x5) << 4) | Tama5RtcNib(s, 0x4))
					      :                  addr5;
				else if (mode == 0x4)
				{
					const uint8_t idx = static_cast<uint8_t>(s.sachen_outer_bank & 0x0F);
					value = (idx < 0x0D) ? Tama5RtcNib(s, idx) : 0;
				}
				if (s.rtc_select == 0x0D) value = static_cast<uint8_t>(value >> 4);
				return static_cast<uint8_t>(value | 0xF0);
			}
			return 0xF1;
		}

		if (s.type == MbcType::MBC7)
		{
			if (!s.ram_enable || !s.mbc1_mode || addr >= 0xB000) return 0xFF;
			switch ((addr >> 4) & 0xF)
			{
				case 2: return static_cast<uint8_t>(s.rtc_latch ? 0xD0 : 0x00);
				case 3: return static_cast<uint8_t>(s.rtc_latch ? 0x81 : 0x80);
				case 4: return static_cast<uint8_t>(s.rtc_latch ? 0xD0 : 0x00);
				case 5: return static_cast<uint8_t>(s.rtc_latch ? 0x81 : 0x80);
				case 6: return 0x00;
				case 8: return s.rtc_select;
				default: return 0xFF;
			}
		}

		if (s.type == MbcType::Camera)
		{
			if (s.mbc1_mode)
			{
				if ((addr & 0x7F) == 0) return (g_cam_countdown > 0) ? 0x01 : 0x00;
				return 0x00;
			}
			if ((s.ram_bank & 0x0F) == 0 && addr >= 0xA100 && addr < 0xAF00) g_cam_live = 40;
			if (g_cam_countdown > 0) return 0x00;
			if (!s.ram_enable) return 0xFF;
			return ReadSram(sram, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u));
		}

		// Makon Pokemon Jade/Diamond: unused RTC selects $0D-$0F hide the
		// protection register pair; real RTC selects read as 0 (no RTC).
		if (s.type == MbcType::PokeJadeDia && unl)
		{
			if (!s.ram_enable) return 0xFF;
			const uint8_t r = unl->mode;
			if (r >= 0x08 && r <= 0x0C) return 0x00;
			if (r == 0x0D) return unl->xors[0];
			if (r == 0x0E) return unl->xors[1];
			if (r == 0x0F) return 0x00;
			return ReadSram(sram, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u));
		}
		// Rocket has no RAM-enable gate at all - its RAM is always wired.
		if (!s.ram_enable && s.type != MbcType::MBC5 &&
		    s.type != MbcType::Rocket) return 0xFF;

		// MBC3 RTC select exposes latched RTC values in this window;
		// the unmapped selects $0D-$0F read open bus.
		if (s.type == MbcType::MBC3 && s.rtc_select >= 0x08)
		{
			if (s.rtc_select <= 0x0C)
				return s.rtc_latched[s.rtc_select - 0x08];
			return 0xFF;
		}

		// MBC2 has internal 512 x 4-bit RAM — upper nibble reads as 0xF.
		if (s.type == MbcType::MBC2)
		{
			uint32_t off = (addr - 0xA000) & 0x01FF;
			return static_cast<uint8_t>(ReadSram(sram, off) | 0xF0);
		}

		uint32_t bank = 0;
		switch (s.type)
		{
			case MbcType::MBC1: bank = Mbc1RamBank(s, mbc1_multicart); break;
			case MbcType::MBC3:
				bank = s.ram_bank & 0x07;
				// Banks past the fitted SRAM read open bus (no wrap).
				if ((bank + 1) * 0x2000u > sram.size()) return 0xFF;
				break;
			case MbcType::MBC5: bank = s.ram_bank & 0x0F; break;
			case MbcType::MMM01: bank = Mmm01RamBank(s) & 0x0F; break;
			case MbcType::BBD:
			case MbcType::Ggb81:
			case MbcType::Hitek:
			case MbcType::Sintax:
			case MbcType::SkobLee8:
			case MbcType::LiCheng:
			case MbcType::NtNew:
			case MbcType::Vf001: bank = s.ram_bank & 0x0F; break;
			default:            bank = 0;               break;
		}
		return ReadSram(sram, (bank * 0x2000u) + (addr - 0xA000u));
	}
	return 0xFF;
}

static void GbCameraCapture(Cart &c)
{
	const uint8_t *reg = c.camera_regs;
	const int expo = (reg[2] << 8) | reg[3];
	g_cam_countdown = 129792 + expo * 64;
	if (g_cam_countdown > 200000) g_cam_countdown = 200000;
	const int W = 128, H = 112;
	unsigned char src[W * H];
	const bool have = g_gb_camera_cb && g_gb_camera_cb(src, W, H);
	if (!have)
		std::memset(src, 0x80, sizeof(src));

	for (int ty = 0; ty < H / 8; ++ty)
	for (int tx = 0; tx < W / 8; ++tx)
	{
		const uint32_t tile_off = 0x100u + static_cast<uint32_t>(ty * (W / 8) + tx) * 16u;
		for (int row = 0; row < 8; ++row)
		{
			uint8_t lo = 0, hi = 0;
			const int py = ty * 8 + row;
			for (int col = 0; col < 8; ++col)
			{
				const int px = tx * 8 + col;
				int v = src[py * W + px] * expo / 0x1000;
				if (v < 0) v = 0; else if (v > 255) v = 255;
				const int base = ((px & 3) + (py & 3) * 4) * 3 + 6;
				int shade;
				if      (v < reg[base + 0]) shade = 3;
				else if (v < reg[base + 1]) shade = 2;
				else if (v < reg[base + 2]) shade = 1;
				else                        shade = 0;
				lo |= static_cast<uint8_t>((shade & 1) << (7 - col));
				hi |= static_cast<uint8_t>(((shade >> 1) & 1) << (7 - col));
			}
			WriteSram(c, tile_off + row * 2 + 0, lo);
			WriteSram(c, tile_off + row * 2 + 1, hi);
		}
	}

	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x)
		{
			int v = src[y * W + x] * 23 / 16 + g_cam_brightness;
			if (v < 0) v = 0; else if (v > 255) v = 255;
			const int base = ((x & 3) + (y & 3) * 4) * 3 + 6;
			uint8_t shade;
			if      (v < reg[base + 0]) shade = 3;
			else if (v < reg[base + 1]) shade = 2;
			else if (v < reg[base + 2]) shade = 1;
			else                        shade = 0;
			g_cam_shade[y * W + x] = shade;
		}
	(void)have;
	g_cam_live = 40;
}

void MbcWrite(Cart &c, uint16_t addr, uint8_t value)
{
	MbcState &s = c.mbc;
	switch (s.type)
	{

	case MbcType::None:
		if (addr >= 0xA000 && addr < 0xC000)
		{
			WriteSram(c, addr - 0xA000, value);
		}
		break;

	case MbcType::NtOld1:
	case MbcType::NtOld2:
		// Makon/NT (taizou's RE in hhugboy). Banking like MBC1/MBC3 but a
		// $5000-$5FFF port picks a multicart 32K-page base (reg 1), the
		// sub-game ROM size (reg 2) and "weird mode" (reg 3 bit 4), which
		// runs bank numbers through a per-type bit reorder.
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0A) == 0x0A);
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = NtBankValue(s, value);
		}
		else if ((addr & 0xF000) == 0x5000)
		{
			switch (addr & 0x03)
			{
			case 0x01:
				s.sachen_outer_bank = value & 0x3F;
				break;
			case 0x02:
				switch (value & 0x0F)
				{
					case 0x00: s.sachen_outer_mask = 5; break;  // 512K
					case 0x08: s.sachen_outer_mask = 4; break;  // 256K
					case 0x0C: s.sachen_outer_mask = 3; break;  // 128K
					case 0x0E: s.sachen_outer_mask = 2; break;  //  64K
					case 0x0F: s.sachen_outer_mask = 1; break;  //  32K
					default:   s.sachen_outer_mask = 5; break;
				}
				break;
			case 0x03:
				// Mode flips take effect on the current bank at once.
				s.mbc1_mode = (value & 0x10) != 0;
				s.rom_bank  = NtBankValue(s, static_cast<uint8_t>(s.rom_bank));
				break;
			}
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (s.ram_enable) WriteSram(c, addr - 0xA000u, value);
		}
		break;

	case MbcType::BBD:
	case MbcType::Ggb81:
	case MbcType::Hitek:
	{
		// BBD family: MBC5 with a data scramble picked at $2001 and a bank
		// scramble at $2080; only exact $x000/$x001/$x080 addresses hit the
		// registers. Hitek powers up scrambled and has no $3xxx register.
		MbcUnl &u = c.unl;
		if ((addr & 0xF0FF) == 0x2001) { u.mode  = value & 0x07; break; }
		if ((addr & 0xF0FF) == 0x2080) { u.mode2 = value & 0x07; break; }
		if (s.type == MbcType::Hitek && (addr & 0xF000) == 0x3000) break;
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			uint32_t v = value;
			if ((addr & 0xF0FF) == 0x2000)
			{
				if (s.type == MbcType::BBD)   v = NtSwitchOrder(value, kBbdBank[u.mode2 & 7]);
				if (s.type == MbcType::Hitek) v = NtSwitchOrder(value, kHitekBank[u.mode2 & 7]);
			}
			s.rom_bank = (s.rom_bank & 0x100) | v;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::Sintax:
	{
		// Sintax: MBC5 whose banked reads XOR against one of four keys
		// ($7x2x-$7x5x) picked by raw bank & 3, and whose bank numbers pass
		// through a reorder picked at $5x1x. Mode changes re-select at once.
		MbcUnl &u = c.unl;
		if ((addr & 0xF0F0) == 0x5010)
		{
			u.mode = value & 0x0F;
			s.rom_bank = (s.rom_bank & 0x100) |
			             NtSwitchOrder(u.raw_bank, SintaxBankTable(u.mode));
			u.cur_xor = u.xors[u.raw_bank & 3];
			break;
		}
		if (addr >= 0x7000 && addr < 0x8000)
		{
			const int slot = static_cast<int>((addr & 0x00F0) >> 4) - 2;
			if (slot >= 0 && slot < 4) u.xors[slot] = value;
			u.cur_xor = u.xors[u.raw_bank & 3];
			break;
		}
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			u.raw_bank = value;
			s.rom_bank = (s.rom_bank & 0x100) |
			             NtSwitchOrder(value, SintaxBankTable(u.mode));
			u.cur_xor  = u.xors[value & 3];
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::SkobLee8:
	{
		// SKOB LEE8: Sintax-like, with preset power-on XOR keys, key slots
		// at $7000|(addr&3) and the reorder mode at $5xx1.
		MbcUnl &u = c.unl;
		if ((addr & 0xF003) == 0x5001)
		{
			u.mode = value & 0x07;
			const uint8_t v = NtSwitchOrder(u.raw_bank, SkobLeeBankTable(u.mode));
			s.rom_bank = (s.rom_bank & 0x100) | v;
			u.cur_xor  = u.xors[v & 3];
			break;
		}
		if (addr >= 0x7000 && addr < 0x8000)
		{
			u.xors[addr & 0x03] = value;
			u.cur_xor = u.xors[static_cast<uint8_t>(s.rom_bank) & 3];
			break;
		}
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			u.raw_bank = value;
			const uint8_t v = NtSwitchOrder(value, SkobLeeBankTable(u.mode));
			s.rom_bank = (s.rom_bank & 0x100) | v;
			u.cur_xor  = u.xors[v & 3];
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::LiCheng:
		// Li Cheng: MBC5 that ignores the decoy bank writes above $2100
		// (run as MBC5 those select garbage banks; $2100 itself must work).
		if (addr > 0x2100 && addr < 0x3000) break;
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			s.rom_bank = (s.rom_bank & 0x100) | value;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::NtNew:
	{
		// Makon later carts: MBC5 until $55 lands on $14xx, which arms two
		// independent 8K half-bank windows set through $20xx / $24xx.
		MbcUnl &u = c.unl;
		if ((addr & 0xFF00) == 0x1400 && value == 0x55) { u.mode = 1; break; }
		if (u.mode && (addr & 0xFF00) == 0x2000)
		{ u.xors[0] = value; u.xors[2] = 1; break; }
		if (u.mode && (addr & 0xFF00) == 0x2400)
		{ u.xors[1] = value; u.xors[3] = 1; break; }
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			s.rom_bank = (s.rom_bank & 0x100) | value;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::PokeJadeDia:
	{
		// Makon Pokemon Jade/Diamond: MBC3 whose unused RTC selects $0D-$0F
		// hide a protection register pair (taizou's RE).
		MbcUnl &u = c.unl;
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x4000)
		{
			const uint32_t v = value & 0x7F;
			s.rom_bank = v ? v : 1;
		}
		else if (addr < 0x6000)
		{
			u.mode = value;
			s.ram_bank = value & 0x03;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			if (u.mode == 0x0D) { u.xors[0] = value; break; }
			if (u.mode == 0x0E) { u.xors[1] = value; break; }
			if (u.mode == 0x0F)
			{
				switch (value)
				{
					case 0x11: --u.xors[0]; break;
					case 0x12: --u.xors[1]; break;
					case 0x41: u.xors[0] = static_cast<uint8_t>(u.xors[0] + u.xors[1]); break;
					case 0x42: u.xors[1] = static_cast<uint8_t>(u.xors[1] + u.xors[0]); break;
					case 0x51: ++u.xors[0]; break;
					case 0x52: --u.xors[1]; break;
				}
				break;
			}
			if (u.mode >= 0x08) break;
			WriteSram(c, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::Vf001:
	{
		// Vast Fame: MBC5 + a rotate-XOR protection engine configured via
		// $7000=$96 .. $700F=$96, arming read-triggered byte sequences and
		// a bank-0 overlay window (taizou's RE).
		MbcUnl &u = c.unl;
		if (u.vfz_on && addr >= 0x6000 && addr < 0x8000)
		{
			if ((addr & 0xF0FF) == 0x7081)
			{
				// Bank code: the fourth byte commits, the first three select.
				{
					const char *tr3 = getenv("VFZ_TRACE");
					if (tr3 && atoi(tr3) >= 3)
						fprintf(stderr, "vfz: code port %04X <- %02X (n=%d)\n",
						        addr, value, u.vfz_code_n);
				}
				u.vfz_code[u.vfz_code_n++ & 3] = value;
				if (u.vfz_code_n >= 4)
				{
					uint32_t bank;
					const char *trace = getenv("VFZ_TRACE");
					if (VfzBankFor(u.vfz_code, bank))
					{
						s.rom_bank = bank;
						if (trace && atoi(trace) >= 2)
							fprintf(stderr, "vfz: bank %02X for %02X %02X %02X %02X\n", bank,
							        u.vfz_code[0], u.vfz_code[1], u.vfz_code[2], u.vfz_code[3]);
					}
					else if (trace)
						fprintf(stderr, "vfz: unknown bank code %02X %02X %02X %02X\n",
						        u.vfz_code[0], u.vfz_code[1], u.vfz_code[2], u.vfz_code[3]);
					u.vfz_code_n = 0;
				}
			}
			else if ((addr & 0xF0FF) == 0x7080)
			{
				if (u.vfz_after_read)
				{
					u.vfz_after_read = 0;
					u.vfz_len = 0;
					if (value == 0x31) break;   // the game's close marker
				}
				if (u.vfz_len < sizeof u.vfz_seq) u.vfz_seq[u.vfz_len++] = value;
			}
			break;
		}
		if (addr >= 0x6000 && addr < 0x8000)
		{
			const uint16_t ea = addr & 0xF00F;
			if (ea == 0x7000 && value == 0x96)
			{
				u.vf_config  = 1;
				u.vf_running = c.vf_alt_board ? 0x10 : 0x00;
				break;
			}
			if (ea == 0x700F && value == 0x96) { u.vf_config = 0; break; }
			if (!u.vf_config) break;
			if (ea >= 0x700B || (ea > 0x6000 && ea < 0x7000)) break;
			u.vf_running = static_cast<uint8_t>(((u.vf_running & 1) ? 0x80 : 0) + (u.vf_running >> 1));
			u.vf_running ^= value;
			if (ea >= 0x7000)      u.vf_700x[ea & 0x0F] = u.vf_running;
			else if (ea == 0x6000) u.vf_6000 = u.vf_running;
			if (ea == 0x7000)
			{
				u.vf_seq_bank = u.vf_700x[3];
				u.vf_seq_addr = static_cast<uint16_t>((u.vf_700x[2] << 8) | u.vf_700x[1]);
				u.vf_seq[0] = u.vf_700x[4];  u.vf_seq[1] = u.vf_700x[5];
				u.vf_seq[2] = u.vf_700x[6];  u.vf_seq[3] = u.vf_700x[7];
				const uint8_t n = u.vf_700x[0] & 7;
				u.vf_seq_len  = (n >= 4) ? static_cast<uint8_t>(n - 3) : 0;
				u.vf_seq_left = 0;
			}
			if (ea == 0x7008)
			{
				u.vf_repl_addr = static_cast<uint16_t>((u.vf_700x[10] << 8) | u.vf_700x[9]);
				u.vf_repl_bank = u.vf_6000;
				u.vf_repl_on   = ((u.vf_700x[8] & 0x0F) == 0x0F);
			}
			break;
		}
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			s.rom_bank = (s.rom_bank & 0x100) | value;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;
	}

	case MbcType::Rocket:
		// Registers at two exact addresses (NewRisingSun's RE): $3F00 is
		// the switchable bank, $3FC0 the outer 16-bank slot on 2-in-1s.
		if (addr == 0x3F00)
		{
			s.rom_bank = value ? value : 1;
		}
		else if (addr == 0x3FC0)
		{
			s.sachen_outer_bank = static_cast<uint8_t>(value << 4);
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			WriteSram(c, addr - 0xA000u, value);
		}
		break;

	case MbcType::MBC1:
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x4000)
		{
			uint32_t v = value & 0x1F;
			if (v == 0) v = 1;
			s.rom_bank = v;
		}
		else if (addr < 0x6000)
		{
			// Stored raw; mode gate applied at read time via Mbc1BankN/Mbc1RamBank.
			s.ram_bank = value & 0x03;
		}
		else if (addr < 0x8000)
		{
			s.mbc1_mode = (value & 0x01) != 0;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, (Mbc1RamBank(s, c.mbc1_multicart) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::MBC2:
		if (addr < 0x4000)
		{
			// MBC2 consolidates RAM enable and ROM bank select on the same range:
			// bit 8 of the address selects which function.
			if ((addr & 0x0100) == 0)
			{
				s.ram_enable = ((value & 0x0F) == 0x0A);
			}
			else
			{
				uint32_t v = value & 0x0F;
				if (v == 0) v = 1;
				s.rom_bank = v;
			}
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			// 512 x 4-bit — only low nibble stored.
			WriteSram(c, (addr - 0xA000) & 0x01FF, static_cast<uint8_t>(value & 0x0F));
		}
		break;

	case MbcType::MBC3:
		if (addr < 0x2000)
		{
			if (c.duz_multicart && value == 0xC0) { s.sachen_outer_mask = 1; break; }   // unlock Duz register port
			if (c.duz_multicart) s.sachen_outer_mask = 0;
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x4000)
		{
			uint32_t v = value & 0x7F;
			if (v == 0) v = 1;
			s.rom_bank = v;
		}
		else if (addr < 0x6000)
		{
			// Only the low 4 bits decode: <=7 selects a RAM bank, 8-C an
			// RTC register, D-F nothing (cpp/rtc-invalid-banks-test).
			s.rtc_select = static_cast<uint8_t>(value & 0x0F);
			if (s.rtc_select <= 0x07) s.ram_bank = s.rtc_select;
		}
		else if (addr < 0x8000)
		{
			// RTC latch: ANY write to this range copies the live counters
			// into the latched bank — hardware doesn't actually require the
			// documented $00→$01 sequence (cpp/latch-rtc-test).
			for (int i = 0; i < 5; ++i) s.rtc_latched[i] = s.rtc_regs[i];
			s.rtc_latch = (value & 0x01) != 0;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (c.duz_multicart && s.sachen_outer_mask)
			{
				// Register port: $A000 latches the index, $A100 writes it. Only $A3 (ROM base, in 32 KiB pages) affects banking.
				if (addr & 0x0100) { if (s.sachen_unlock_ctr == 0xA3) s.sachen_outer_bank = static_cast<uint8_t>(value << 1); }
				else                 s.sachen_unlock_ctr = value;
				break;
			}
			if (!s.ram_enable) break;
			if (s.rtc_select >= 0x08 && s.rtc_select <= 0x0C)
			{
				static const uint8_t kRtcMask[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };
				const int idx = s.rtc_select - 0x08;
				s.rtc_regs[idx] = static_cast<uint8_t>(value & kRtcMask[idx]);
				// Writing seconds also clears the sub-second counter.
				if (idx == 0) s.rtc_sub_cycles = 0;
			}
			else if (s.rtc_select <= 0x07)
			{
				const uint32_t bank = s.ram_bank & 0x07;
				if ((bank + 1) * 0x2000u <= c.sram.size())
					WriteSram(c, (bank * 0x2000u) + (addr - 0xA000u), value);
			}
		}
		break;

	case MbcType::MBC5:
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x3000)
		{
			s.rom_bank = (s.rom_bank & 0x100) | value;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = (s.rom_bank & 0x0FF) | (static_cast<uint32_t>(value & 0x01) << 8);
		}
		else if (addr < 0x6000)
		{
			// 23-in-1 multicart: the menu latches a base bank ($5001) and a
			// bank mask ($5002), both in 2-bank units, then jumps to $0100.
			if (c.mbc5_multicart && addr >= 0x5000)
			{
				if ((addr & 0x0F) == 1)
					s.sachen_outer_bank = static_cast<uint8_t>(value << 1);
				else if ((addr & 0x0F) == 2)
					s.sachen_outer_mask = static_cast<uint8_t>(value << 1);
			}
			s.ram_bank = value & 0x0F;
			// bit 3 = rumble for rumble carts; ignored here (P7 may wire it).
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::SachenMMC1:
		// Outer-bank / inner-bank / mask. The outer registers latch only
		// while the inner-bank holds D5:D4 = 0b11 (per Tauwasser's RE).
		if (addr < 0x2000)
		{
			if ((s.rom_bank & 0x30) == 0x30) s.sachen_outer_bank = value;
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = value ? value : 1u;
		}
		else if (addr < 0x6000)
		{
			if ((s.rom_bank & 0x30) == 0x30) s.sachen_outer_mask = value;
		}
		break;

	case MbcType::MMM01:
		// 4 register ranges, all writable from unlocked menu mode; once
		// the menu writes bit 6 of $0000-$1FFF the mapper locks down and
		// only the MBC1-compatible fields (rom_bank_low / ram_bank_low /
		// mbc1_mode + ram_enable) remain writable.
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
			if (!s.mmm01_locked)
			{
				s.mmm01_ram_bank_mask = static_cast<uint8_t>(value >> 4);
				if (value & 0x40)
				{
					s.mmm01_locked      = true;
					s.mmm01_just_locked = true;
				}
			}
		}
		else if (addr < 0x4000)
		{
			if (!s.mmm01_locked)
			{
				s.mmm01_rom_bank_mid = static_cast<uint8_t>(value >> 5);
			}
			const uint8_t lowmask = static_cast<uint8_t>(s.mmm01_rom_bank_mask << 1);
			s.mmm01_rom_bank_low = static_cast<uint8_t>(
			    (s.mmm01_rom_bank_low & lowmask)
			  | (value & static_cast<uint8_t>(~lowmask)));
		}
		else if (addr < 0x6000)
		{
			s.mmm01_ram_bank_low = static_cast<uint8_t>(
			    value | static_cast<uint8_t>(~s.mmm01_ram_bank_mask));
			if (!s.mmm01_locked)
			{
				s.mmm01_ram_bank_high     = static_cast<uint8_t>((value >> 2) & 0x03);
				s.mmm01_rom_bank_high     = static_cast<uint8_t>((value >> 4) & 0x03);
				s.mmm01_mbc1_mode_disable = (value & 0x40) != 0;
			}
		}
		else if (addr < 0x8000)
		{
			if (!s.mmm01_mbc1_mode_disable)
			{
				s.mmm01_mbc1_mode = (value & 0x01) != 0;
			}
			if (!s.mmm01_locked)
			{
				s.mmm01_rom_bank_mask  = static_cast<uint8_t>((value >> 2) & 0x0F);
				s.mmm01_multiplex_mode = (value & 0x40) != 0;
			}
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable) break;
			WriteSram(c, ((Mmm01RamBank(s) & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::HuC1:
		if (addr < 0x2000)
		{
			// Value $0E selects the IR register at $A000-$BFFF; any other
			// value selects cart RAM. HuC1 RAM has no separate enable, so
			// the select line is modeled directly in ram_enable.
			s.ram_enable = ((value & 0x0F) != 0x0E);
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = value & 0x3F;
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x03;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			// RAM mode → cart RAM; IR mode → IR LED, which we discard.
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::HuC3:
		if (addr < 0x2000)
		{
			// $A000-$BFFF window mode; RAM is read/write only in mode $A.
			s.rtc_select = value & 0x0F;
			s.ram_enable = (s.rtc_select == 0x0A);
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = value & 0x7F;   // 7-bit, MBC5-style (no 0->1 remap)
		}
		else if (addr < 0x6000)
		{
			s.ram_bank = value & 0x03;
		}
		else if (addr < 0x8000)
		{
			// $6000-$7FFF: no effect on HuC3.
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			switch (s.rtc_select)
			{
				case 0xA: WriteSram(c, ((s.ram_bank & 0x03) * 0x2000u) + (addr - 0xA000u), value); break;
				case 0xB: Huc3Command(s, value); break;  // RTC command (executes immediately)
				default:  break;                          // RO RAM / semaphore / IR / undefined
			}
		}
		break;

	case MbcType::TAMA5:
		if (addr >= 0xA000 && addr < 0xC000)
		{
			if (addr & 1)
			{
				s.rtc_select = static_cast<uint8_t>(value & 0x0F);
			}
			else
			{
				const uint8_t v = static_cast<uint8_t>(value & 0x0F);
				switch (s.rtc_select)
				{
					case 0x0: s.rom_bank = (s.rom_bank & 0xF0u) | v; break;
					case 0x1: s.rom_bank = (s.rom_bank & 0x0Fu) | (static_cast<uint32_t>(v) << 4); break;
					case 0x4: s.sachen_outer_bank = static_cast<uint8_t>((s.sachen_outer_bank & 0xF0) | v); break;
					case 0x5: s.sachen_outer_bank = static_cast<uint8_t>((s.sachen_outer_bank & 0x0F) | (v << 4)); break;
					case 0x6: s.sachen_outer_mask = static_cast<uint8_t>((s.sachen_outer_mask & 0x0F) | (v << 4)); break;
					case 0x7:
						s.sachen_outer_mask = static_cast<uint8_t>((s.sachen_outer_mask & 0xF0) | v);
						Tama5Trigger(c);
						break;
					default: break;
				}
			}
		}
		break;

	case MbcType::M161:
		// Write-once page register: the first write to $0000-$7FFF latches the
		// selected 32 KiB page; later writes (e.g. a sub-game's own $2000 bank
		// poke) are ignored. mbc1_mode is reused as the "latched" flag.
		if (addr < 0x8000 && !s.mbc1_mode)
		{
			s.rom_bank  = value & 0x07;
			s.mbc1_mode = true;
		}
		break;

	case MbcType::Camera:
		if (addr < 0x2000)
		{
			s.ram_enable = ((value & 0x0F) == 0x0A);
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = value & 0x3F;
		}
		else if (addr < 0x6000)
		{
			s.mbc1_mode = (value & 0x10) != 0;
			if (!s.mbc1_mode) s.ram_bank = value & 0x0F;
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (s.mbc1_mode)
			{
				const uint16_t r = static_cast<uint16_t>((addr - 0xA000u) & 0x7F);
				if (r < 0x36)
				{
					c.camera_regs[r] = value;
					if (r == 0 && (value & 0x01)) GbCameraCapture(c);
				}
				break;
			}
			if (!s.ram_enable) break;
			WriteSram(c, ((s.ram_bank & 0x0F) * 0x2000u) + (addr - 0xA000u), value);
		}
		break;

	case MbcType::MBC6:
		if (addr < 0x0400)
		{
			s.ram_enable = (value == 0x0A);
		}
		else if (addr < 0x0800)
		{
			s.ram_bank = value;
		}
		else if (addr < 0x0C00)
		{
			s.mmm01_rom_bank_mid = value;
		}
		else if (addr >= 0x2000 && addr < 0x2800)
		{
			s.rom_bank = value;
		}
		else if (addr >= 0x2800 && addr < 0x3000)
		{
			s.mbc1_mode = (value & 0x08) != 0;
		}
		else if (addr >= 0x3000 && addr < 0x3800)
		{
			s.mmm01_rom_bank_low = value;
		}
		else if (addr >= 0x3800 && addr < 0x4000)
		{
			s.mmm01_mbc1_mode = (value & 0x08) != 0;
		}
		else if (addr >= 0xA000 && addr < 0xB000)
		{
			if (s.ram_enable && !c.sram.empty())
				WriteSram(c, (s.ram_bank * 0x1000u) + (addr - 0xA000u), value);
		}
		else if (addr >= 0xB000 && addr < 0xC000)
		{
			if (s.ram_enable && !c.sram.empty())
				WriteSram(c, (s.mmm01_rom_bank_mid * 0x1000u) + (addr - 0xB000u), value);
		}
		break;

	case MbcType::MBC7:
		if (addr < 0x2000)
		{
			s.ram_enable = (value == 0x0A);
		}
		else if (addr < 0x4000)
		{
			s.rom_bank = value;
		}
		else if (addr < 0x6000)
		{
			s.mbc1_mode = (value == 0x40);
		}
		else if (addr >= 0xA000 && addr < 0xC000)
		{
			if (!s.ram_enable || !s.mbc1_mode || addr >= 0xB000) break;
			switch ((addr >> 4) & 0xF)
			{
				case 0: if (value == 0x55) s.rtc_latch = false; break;
				case 1: if (value == 0xAA) s.rtc_latch = true;  break;
				case 8: Mbc7EepromClock(c, value); break;
				default: break;
			}
		}
		break;

	default:
		break;
	}
}

void MbcUnlReset(Cart &c)
{
	c.unl = MbcUnl();
	c.unl.vfz_on = c.vf_zook ? 1 : 0;
	// Hitek boards power up with both scrambles at 7; SKOB LEE8 boards with
	// reorder mode $0F and preset XOR keys (applied from the first bank
	// write - the power-on xor is 0, matching hhugboy).
	if (c.mbc.type == MbcType::Hitek)
	{
		c.unl.mode  = 7;
		c.unl.mode2 = 7;
	}
	else if (c.mbc.type == MbcType::SkobLee8)
	{
		c.unl.mode    = 0x0F;
		c.unl.xors[0] = 0x55;
		c.unl.xors[1] = 0xAA;
		c.unl.xors[2] = 0xF0;
		c.unl.xors[3] = 0x0F;
	}
}

void MbcNotifyHighWrite(MbcState &s, uint16_t /*addr*/, uint8_t value)
{
	if (s.type != MbcType::SachenMMC1) return;
	if (!s.sachen_locked) return;
	// Per Tauwasser's RE the unlock sequence is specifically value $31
	// written to addresses with A15 set. Filtering on $31 keeps the SGB
	// boot ROM's VRAM clear (writes garbage A) and packet-protocol
	// $FF00 traffic (writes $00/$20/$30) from prematurely unlocking.
	if (value != 0x31) return;
	if (s.sachen_unlock_ctr < 0x30)
	{
		if (++s.sachen_unlock_ctr >= 0x30) s.sachen_locked = false;
	}
}

} // namespace SGB
