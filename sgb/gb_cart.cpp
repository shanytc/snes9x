/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cart.h"
#include "gb_ppu.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace SGB {

namespace {

// Translate the header byte at 0x0149 into the physical RAM size.
// Values per Pan Docs: 0=none, 1=2KB (rare), 2=8KB, 3=32KB, 4=128KB, 5=64KB.
uint32_t DecodeRamSize(uint8_t code)
{
	switch (code)
	{
		case 0: return 0;
		case 1: return   2 * 1024;
		case 2: return   8 * 1024;
		case 3: return  32 * 1024;
		case 4: return 128 * 1024;
		case 5: return  64 * 1024;
		default: return 0;
	}
}

// Cart-type → MBC + battery/rtc/rumble flags. Returns false for unknown.
bool DecodeCartType(uint8_t code, MbcType &mbc, bool &battery, bool &rtc, bool &rumble)
{
	battery = rtc = rumble = false;
	switch (code)
	{
		case 0x00: mbc = MbcType::None; return true;
		case 0x01: mbc = MbcType::MBC1; return true;
		case 0x02: mbc = MbcType::MBC1; return true;
		case 0x03: mbc = MbcType::MBC1; battery = true; return true;
		case 0x05: mbc = MbcType::MBC2; return true;
		case 0x06: mbc = MbcType::MBC2; battery = true; return true;
		case 0x08: mbc = MbcType::None; return true;           // ROM+RAM
		case 0x09: mbc = MbcType::None; battery = true; return true;
		case 0x0B: mbc = MbcType::MMM01; return true;
		case 0x0C: mbc = MbcType::MMM01; return true;
		case 0x0D: mbc = MbcType::MMM01; battery = true; return true;
		case 0x0F: mbc = MbcType::MBC3; battery = true; rtc = true; return true;
		case 0x10: mbc = MbcType::MBC3; battery = true; rtc = true; return true;
		case 0x11: mbc = MbcType::MBC3; return true;
		case 0x12: mbc = MbcType::MBC3; return true;
		case 0x13: mbc = MbcType::MBC3; battery = true; return true;
		case 0x19: mbc = MbcType::MBC5; return true;
		case 0x1A: mbc = MbcType::MBC5; return true;
		case 0x1B: mbc = MbcType::MBC5; battery = true; return true;
		case 0x1C: mbc = MbcType::MBC5; rumble = true; return true;
		case 0x1D: mbc = MbcType::MBC5; rumble = true; return true;
		case 0x1E: mbc = MbcType::MBC5; rumble = true; battery = true; return true;
		case 0x20: mbc = MbcType::MBC6; battery = true; return true;
		case 0x22: mbc = MbcType::MBC7; rumble = true; battery = true; return true;
		case 0xFC: mbc = MbcType::Camera; battery = true; return true;
		case 0xFD: mbc = MbcType::TAMA5; battery = true; rtc = true; return true;
		case 0xFE: mbc = MbcType::HuC3; battery = true; rtc = true; return true;
		case 0xFF: mbc = MbcType::HuC1; battery = true; return true;
		default:                                 return false;
	}
}

// Sachen unlicensed multicarts (4B-007 etc.) hide the Nintendo logo under
// an address bit-permutation across 0x0104-0x01FF. If the standard logo
// slot doesn't match but the permuted bytes do, this is a Sachen cart and
// we route it through the Sachen mapper regardless of the header byte at
// 0x0147 (which is forged — 4B-007 reports MBC1 + 32KB).
constexpr uint8_t kGbNintendoLogo[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
	0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
	0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
	0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
	0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
	0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
};

inline uint16_t SachenScrambleAddr(uint16_t addr)
{
	return static_cast<uint16_t>((addr & ~0x53u)
	                           | ((addr >> 6) & 0x01u)
	                           | ((addr >> 3) & 0x02u)
	                           | ((addr << 3) & 0x10u)
	                           | ((addr << 6) & 0x40u));
}

// Per Tauwasser's RE the mapper forces ROM address bit 7 high while locked, so
// a cart may stash its logo in either half of the permuted $0100-$01FF window.
bool SachenLogoAt(const std::vector<uint8_t> &rom, bool ra7_high)
{
	if (rom.size() < 0x0200) return false;
	for (int i = 0; i < 48; ++i)
	{
		uint16_t a = SachenScrambleAddr(static_cast<uint16_t>(0x0104 + i));
		if (ra7_high) a |= 0x80;
		if (rom[a] != kGbNintendoLogo[i]) return false;
	}
	return true;
}

bool LooksLikeSachenScrambledLogo(const std::vector<uint8_t> &rom)
{
	return SachenLogoAt(rom, false) || SachenLogoAt(rom, true);
}

bool SachenHeaderRunsRaw(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x0200) return false;
	auto rd = [&](uint16_t a) { return rom[SachenScrambleAddr(a)]; };
	const uint8_t e0 = rd(0x0100), e1 = rd(0x0101), e2 = rd(0x0102), e3 = rd(0x0103);
	uint16_t target;
	if (e0 == 0xC3)                    target = static_cast<uint16_t>(e1 | (e2 << 8));
	else if (e0 == 0x00 && e1 == 0xC3) target = static_cast<uint16_t>(e2 | (e3 << 8));
	else                               return false;
	return target < 0x0200;
}

// Logo-less Sachen single-game carts (e.g. Beast Fighter) carry no Nintendo
// logo anywhere — not even under the address swap — so LooksLikeSachenScrambledLogo
// can't see them and DecodeCartType rejects their forged $0147. Identify them
// structurally instead: every Sachen cart presents a *valid Game Boy header*
// once its A0/A6 + A1/A4 address swap is undone — a NOP;JP (or JP) entry into
// ROM plus a header checksum that matches $014D (the standard $0134-$014C
// algorithm the DMG boot uses). A non-GB ROM has essentially no chance of
// satisfying both the entry shape and the checksum, so no logo bytes are needed
// to recognise the cart.
bool LooksLikeSachenScrambledHeader(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x0200) return false;
	auto rd = [&](uint16_t a) { return rom[SachenScrambleAddr(a)]; };
	uint16_t entry;
	if (rd(0x0100) == 0x00 && rd(0x0101) == 0xC3)
		entry = static_cast<uint16_t>(rd(0x0102) | (rd(0x0103) << 8));
	else if (rd(0x0100) == 0xC3)
		entry = static_cast<uint16_t>(rd(0x0101) | (rd(0x0102) << 8));
	else
		return false;
	if (entry >= 0x8000) return false;
	uint8_t sum = 0;
	for (uint16_t a = 0x0134; a <= 0x014C; ++a) sum = static_cast<uint8_t>(sum - rd(a) - 1);
	return sum == rd(0x014D);
}

// MMM01: the menu lives in the LAST 32 KiB of ROM and carries its own
// valid Nintendo logo at offset 0x0104 within that bank — that's how
// real hardware powers on into the menu (upper ROM address lines are
// forced to 1 until the menu unlocks the mapper). The forged $0147
// byte at file offset 0 lies (Mani carts report MBC1/MBC3), so we
// identify the cart by the trailing-bank logo.
bool LooksLikeMmm01(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x10000) return false;          // need >= 64 KiB
	if (rom.size() % 0x8000)  return false;          // and a 32 KiB-aligned size
	const size_t base = rom.size() - 0x8000 + 0x0104;
	if (base + 48 > rom.size()) return false;
	for (int i = 0; i < 48; ++i)
	{
		if (rom[base + i] != kGbNintendoLogo[i]) return false;
	}
	return true;
}

// M161 (Mani 4-in-1 "Tetris Set"): a write-once register selects one of up to
// eight raw 32 KiB pages into $0000-$7FFF — the menu is page 0, the sub-games
// follow. The $0147 byte is forged (this cart claims MBC3+TIMER+BATTERY), so
// identify it structurally: a second Nintendo logo at the first sub-game page
// ($8000) that no single-game cart carries.
bool LooksLikeM161(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x10000) return false;          // need >= menu + one game
	if (rom.size() % 0x8000)  return false;          // 32 KiB-page aligned
	auto logo_at = [&](size_t base) {
		if (base + 0x0104 + 48 > rom.size()) return false;
		for (int i = 0; i < 48; ++i)
			if (rom[base + 0x0104 + i] != kGbNintendoLogo[i]) return false;
		return true;
	};
	if (rom[0x8148] != 0x00) return false;           // page 1 must be a 32 KiB game, not a banked 2-in-1 header
	return logo_at(0x0000) && logo_at(0x8000);
}

// MBC1 multicart (MBC1M): a "menu + games" compilation where the 2-bit BANK2
// register drives ROM A18-A19 (instead of A19-A20), so it selects a 256 KiB
// game slot and BANK1 keeps only its low 4 bits. The header still reports plain
// MBC1, so identify it structurally: a second Nintendo logo at the start of the
// bank-0x10 slot ($40000) that a single-game cart never carries.
bool LooksLikeMbc1Multicart(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x44000) return false;          // need at least slot 1's header
	for (int i = 0; i < 48; ++i)
		if (rom[0x40104 + i] != kGbNintendoLogo[i]) return false;
	return true;
}

// Duz "2-in-1" multicart (e.g. Pokemon Red/Blue): a multi-MB ROM that reports
// plain MBC3 but carries a boot menu in banks 0-1 and each sub-game starting at
// a 32 KiB page boundary, each with its own Nintendo logo. The menu programs the
// ROM base via a register port in the SRAM window. Identify it by the second
// logo at $8000 (which a single game never carries) over a banked sub-game
// header — the same marker M161 rejects, but here the cart is genuinely MBC3.
bool LooksLikeDuzMulticart(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x80000) return false;
	if (rom[0x8148] == 0x00)  return false;          // $8000 is a real banked game, not a 32 KiB M161 page
	for (int i = 0; i < 48; ++i)
	{
		if (rom[0x0104 + i] != kGbNintendoLogo[i]) return false;
		if (rom[0x8104 + i] != kGbNintendoLogo[i]) return false;
	}
	return true;
}

// Datel "Rocket Games" GBC carts (ATV Racing, Karate Joe, ...): the header
// stores the Rocket logo, identified by its byte sum the way hhugboy does
// (2756; 4850 is the Smartcom variant). The real mapper morphs those bytes
// into the Nintendo logo for the CGB boot's reads — see RocketBootLogoByte.
bool LooksLikeRocket(const std::vector<uint8_t> &rom)
{
	int sum = 0;
	for (int i = 0; i < 48; ++i) sum += rom[0x0104 + i];
	return sum == 2756 || sum == 4850;
}

// Flat-banking unlicensed carts hiding behind garbage $0147 bytes, identified
// by title: Gowin's Taiwan releases ($30/$38/$B0), the "SONIC5" pirates
// (Sonic 3D Blast 5 / Super Donkey Kong 3 — $EA is code spilling into the
// header, per hhugboy), and the Xploder GB cheat cart ($69). All of them
// bank like a plain MBC1.
bool LooksLikeFlatUnlicensed(const CartHeader &h)
{
	if (std::strncmp(h.title, "GOWIN", 5) == 0) return true;
	if (std::strstr(h.title, "SONIC5") != nullptr) return true;
	if (h.cart_type == 0x69 &&
	    std::strncmp(h.title, "*Future Console", 15) == 0) return true;
	return false;
}

// Makon/NT early carts (taizou's RE in hhugboy): forged MBC headers, real
// mapper picked by licensee "MK" + known title (the single-cart series), or
// by the known multicart menus. Returns None when this is not a Makon cart.
MbcType LooksLikeMakonOld(const std::vector<uint8_t> &rom, const CartHeader &h)
{
	const bool mk = rom[0x144] == 'M' && rom[0x145] == 'K';
	if (std::strcmp(h.title, "POKEBOM USA") == 0 && rom.size() > 512 * 1024)
	{
		if (rom[0x102] == 0xE0) return MbcType::NtOld2;   // 23-in-1 w/ Mario
		if (rom[0x102] == 0xC0) return MbcType::NtOld1;   // 25-in-1 w/ Rockman
	}
	if ((std::strcmp(h.title, " - TRUMP  BOY -") == 0 ||
	     std::strcmp(h.title, "QBILLION") == 0) && rom.size() > 512 * 1024)
		return MbcType::NtOld2;
	if (std::strcmp(h.title, "ROCKMAN 99") == 0 && !mk &&
	    rom.size() > 0x8001 && rom[0x8001] != 0xB7)
		return MbcType::NtOld1;
	if (mk && rom[0x148] == 0x03 &&
	    (std::strcmp(h.title, "SONIC 7") == 0 ||
	     std::strcmp(h.title, "SUPER MARIO 3") == 0 ||
	     std::strcmp(h.title, "DONKEY\tKONG 5") == 0 ||
	     std::strcmp(h.title, "ROCKMAN 99") == 0))
		return MbcType::NtOld2;
	return MbcType::None;
}

// Canonical $0184 header logos of the scrambled unlicensed families, taken
// byte-exact from verified carts. hhugboy keys on byte sums, but sums
// collide with licensed games (Lemmings, Jungle Book, Q-bert...), so the
// full bitmap is matched instead.
constexpr uint8_t kLogoBbd[48] = {
	0x00, 0x00, 0x00, 0x00, 0xFB, 0xBB, 0xEF, 0x3E, 0x00, 0x00, 0xFB, 0xBB,
	0xEF, 0x3E, 0x00, 0x00, 0xFB, 0xBB, 0xEF, 0x33, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xBB, 0xBF, 0xF3, 0xFE, 0x00, 0x00, 0xBB, 0xBF,
	0xF3, 0xFE, 0x00, 0x00, 0xBB, 0xBF, 0x33, 0xFE, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kLogoBbdSgbc[48] = {
	0x36, 0x63, 0xE3, 0x0E, 0x00, 0x07, 0x00, 0x0E, 0x36, 0x66, 0xE3, 0x07,
	0x76, 0x67, 0xE3, 0x3E, 0x36, 0x66, 0xE3, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x06, 0x30, 0x33, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x66, 0x30, 0x33, 0xF0,
	0x66, 0x70, 0x33, 0xE0, 0x66, 0x30, 0x03, 0xE0, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kLogoBbdFiver[48] = {
	0xF8, 0x8E, 0x54, 0x44, 0x88, 0x88, 0xD9, 0x99, 0xE0, 0x0C, 0xF8, 0x8F,
	0x08, 0x80, 0xF8, 0x8E, 0x55, 0x55, 0xE1, 0x1E, 0x46, 0x65, 0x13, 0x35,
	0x88, 0x80, 0x44, 0x40, 0x55, 0x20, 0x11, 0x10, 0x00, 0xE0, 0xA9, 0x80,
	0x00, 0x80, 0x88, 0x80, 0x55, 0x50, 0x42, 0x10, 0x54, 0x40, 0x59, 0x90,
};
constexpr uint8_t kLogoGgb81[48] = {
	0x07, 0x22, 0x0F, 0x00, 0x00, 0x88, 0x0F, 0x22, 0x08, 0x00, 0x07, 0x88,
	0x0F, 0x0F, 0x00, 0x08, 0x0F, 0x22, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x70, 0x00, 0xF0, 0x88, 0x00, 0x22, 0xF0, 0x00, 0x80, 0x88, 0x70,
	0x11, 0xF0, 0x88, 0x00, 0x22, 0xF0, 0x00, 0x80, 0x0E, 0xE0, 0x00, 0x00,
};
constexpr uint8_t kLogoHitek[48] = {
	0x01, 0x33, 0x0F, 0x10, 0x00, 0x80, 0x0C, 0xCD, 0x06, 0xC8, 0x00, 0x00,
	0x00, 0x00, 0x0F, 0xCC, 0x0C, 0x66, 0x01, 0x10, 0x08, 0x8D, 0x0C, 0xC8,
	0x33, 0x31, 0x71, 0x1F, 0x88, 0x80, 0xFD, 0xCC, 0x08, 0xC6, 0x03, 0x30,
	0x08, 0x80, 0xFD, 0xCC, 0xC8, 0xC6, 0x00, 0x11, 0x7D, 0x88, 0x08, 0xCC,
};
constexpr uint8_t kLogoLiCheng[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
	0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
	0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDC, 0xBB, 0xBE, 0x33, 0x33, 0x66, 0x63,
	0x66, 0x6C, 0xDD, 0xD7, 0xBB, 0xB1, 0x11, 0x1F, 0xBB, 0xB1, 0x3F, 0x0E,
};
constexpr uint8_t kLogoSintax[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
	0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
	0x99, 0x89, 0x93, 0x73, 0x22, 0x0D, 0x44, 0x0B, 0xCC, 0xC3, 0xCD, 0xC3,
	0x3F, 0x3C, 0x20, 0x2F, 0x57, 0x78, 0x22, 0x87, 0x66, 0xE1, 0xDD, 0x1E,
};
constexpr uint8_t kLogoSintax2[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
	0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
	0x99, 0x89, 0x93, 0x73, 0x22, 0x0D, 0x44, 0x0B, 0xCC, 0xC3, 0xCD, 0xC3,
	0x3F, 0x3C, 0x2D, 0x2F, 0x57, 0x78, 0x22, 0x87, 0x66, 0xE1, 0xDD, 0x1E,
};
constexpr uint8_t kLogoSkobLee8[48] = {
	0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
	0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
	0x14, 0x97, 0xCC, 0x81, 0xDD, 0xD8, 0xBB, 0xBF, 0x33, 0x3B, 0x66, 0x63,
	0x66, 0x6C, 0xDD, 0xD7, 0x99, 0x9C, 0x99, 0x9F, 0x88, 0x9D, 0x6C, 0x8E,
};
constexpr uint8_t kLogoVf[48] = {
	0x33, 0x31, 0x11, 0x1B, 0x88, 0x81, 0x36, 0x6F, 0x84, 0x08, 0x00, 0xF0,
	0x00, 0x8C, 0x00, 0x36, 0x00, 0xB4, 0x00, 0x8D, 0x00, 0xF8, 0x00, 0x8C,
	0x10, 0x00, 0xBE, 0xE4, 0x00, 0x66, 0x66, 0x66, 0x01, 0x10, 0xF8, 0x8F,
	0xCC, 0xC7, 0x66, 0x66, 0x44, 0x44, 0xDD, 0xDC, 0xF8, 0x8F, 0xC0, 0x48,
};
constexpr uint8_t kLogoVfDigi[48] = {
	0x00, 0x00, 0x33, 0xCC, 0xF0, 0x01, 0xC6, 0x68, 0x10, 0x11, 0xF6, 0x88,
	0x81, 0x36, 0xF8, 0x00, 0xE1, 0x10, 0x08, 0x80, 0x30, 0x33, 0xFC, 0x00,
	0x33, 0xCF, 0x00, 0x0F, 0x16, 0x68, 0x80, 0x17, 0x66, 0x8E, 0x01, 0x10,
	0xC8, 0x8F, 0x70, 0x1E, 0xFF, 0xCC, 0x00, 0x3F, 0xCC, 0x0C, 0x00, 0x00,
};
constexpr uint8_t kLogoVfSoul[48] = {
	0x01, 0x77, 0x7C, 0x0F, 0xF0, 0x0F, 0xCE, 0x08, 0x13, 0x7E, 0xF8, 0x01,
	0xF7, 0xEC, 0x13, 0x7E, 0xC8, 0x01, 0x37, 0xEC, 0x93, 0x7E, 0xC8, 0x00,
	0x0E, 0x70, 0x00, 0xF0, 0x37, 0xE0, 0x93, 0x30, 0xC8, 0xF0, 0x37, 0xE0,
	0x93, 0x30, 0xC8, 0xF0, 0x37, 0xE0, 0x93, 0x70, 0xC8, 0xF0, 0x00, 0xF0,
};

// The mapper for a $0184 family logo, or None. Decrypted re-dumps keep the
// logo but plain banking - hhugboy's last-bank-byte test spots them and
// they fall through to the standard header decode.
MbcType LooksLikeScrambledUnl(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x10000) return MbcType::None;
	const uint8_t *logo = &rom[0x184];
	auto is = [&](const uint8_t (&ref)[48]) {
		return std::memcmp(logo, ref, 48) == 0;
	};
	const bool bbd_plain    = rom[0x7FFF] == 0x01 && rom[0xBFFF] == 0x02;
	const bool sintax_plain = rom[0x7FFF] == 0x00 || rom[0x7FFF] == 0x01;
	if (is(kLogoBbd) || is(kLogoBbdSgbc) || is(kLogoBbdFiver))
		return bbd_plain ? MbcType::None : MbcType::BBD;
	if (is(kLogoGgb81))    return MbcType::Ggb81;
	if (is(kLogoHitek))    return MbcType::Hitek;
	if (is(kLogoLiCheng))  return MbcType::LiCheng;
	if (is(kLogoSintax) || is(kLogoSintax2))
		return sintax_plain ? MbcType::None : MbcType::Sintax;
	// Some decrypted SKOB dumps carry the bank markers swapped (D-3: 02/01).
	if (is(kLogoSkobLee8))
		return (bbd_plain || (rom[0x7FFF] == 0x02 && rom[0xBFFF] == 0x01))
		       ? MbcType::None : MbcType::SkobLee8;
	if (is(kLogoVf) || is(kLogoVfDigi) || is(kLogoVfSoul)) return MbcType::Vf001;
	return MbcType::None;
}

// "23 in 1"-style MBC5 multicart: a genuine MBC5 whose menu latches a base bank
// and bank mask through $5001/$5002. Identified by the crowd of sub-game Nintendo
// logos sitting on bank boundaries — a single-game cart carries exactly one.
bool LooksLikeMbc5Multicart(const std::vector<uint8_t> &rom)
{
	if (rom.size() < 0x80000) return false;
	int logos = 0;
	for (size_t base = 0x4000; base + 0x134 <= rom.size(); base += 0x4000)
	{
		if (memcmp(&rom[base + 0x104], kGbNintendoLogo, 48) == 0 && ++logos >= 4)
			return true;
	}
	return false;
}

std::string MakeSavPath(const std::string &rom_path)
{
	if (rom_path.empty()) return {};
	std::string p = rom_path;
	const size_t dot   = p.find_last_of('.');
	const size_t slash = p.find_last_of("/\\");
	// Only strip extension if the dot is part of the filename, not a directory name.
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
	{
		p.resize(dot);
	}
	p += ".sav";
	return p;
}

} // anonymous

bool SachenBootLogoByte(const Cart &c, uint16_t addr, uint8_t &out)
{
	if (c.mbc.type != MbcType::SachenMMC1) return false;
	if (addr < 0x0104u || addr > 0x0133u) return false;
	out = kGbNintendoLogo[addr - 0x0104u];
	return true;
}

bool RocketBootLogoByte(const Cart &c, uint16_t addr, uint8_t &out)
{
	if (c.mbc.type != MbcType::Rocket) return false;
	if (addr < 0x0104u || addr > 0x0133u) return false;
	out = kGbNintendoLogo[addr - 0x0104u];
	return true;
}

bool CartLoad(Cart &c, const uint8_t *data, size_t size, const char *path)
{
	// Minimum viable ROM must at least cover the header + one full bank.
	if (size < 0x0150 || data == nullptr) return false;

	c.rom.assign(data, data + size);
	// A console reset keeps an MBC3/HuC3/TAMA5 clock running; a new cart starts clean.
	c.mbc = MbcState();
	c.huc3_alarm_minutes = c.huc3_alarm_days = 0;
	c.huc3_alarm_on = false;
	std::memset(c.tama6_pages, 0, sizeof c.tama6_pages);

	// ----- Header parse -----
	CartHeader &h = c.header;

	// Title is 16 bytes starting at 0x0134. On CGB carts the last 4 are
	// repurposed for manufacturer code + CGB flag; the CGB flag byte lives
	// at 0x0143 either way so reading it separately is correct for both.
	std::memcpy(h.title, &c.rom[0x0134], 16);
	h.title[16] = 0;
	// Strip non-printable tail (padding is usually 0x00 or 0xFF).
	for (int i = 15; i >= 0; --i)
	{
		uint8_t b = static_cast<uint8_t>(h.title[i]);
		if (b == 0 || b < 0x20 || b >= 0x7F) h.title[i] = 0;
		else                                  break;
	}

	h.cgb_flag  = c.rom[0x0143];
	h.sgb_flag  = c.rom[0x0146];
	h.cart_type = c.rom[0x0147];

	uint8_t rom_code = c.rom[0x0148];
	h.rom_size  = (rom_code <= 8)
	              ? static_cast<uint32_t>(32u * 1024u * (1u << rom_code))
	              : 0;

	uint8_t ram_code = c.rom[0x0149];
	h.ram_size  = DecodeRamSize(ram_code);

	h.dest_code  = c.rom[0x014A];
	h.header_sum = c.rom[0x014D];
	h.global_sum = static_cast<uint16_t>(
		(static_cast<uint16_t>(c.rom[0x014E]) << 8) | c.rom[0x014F]);

	// Header checksum: x = 0; for i in 0x0134..0x014C: x = x - rom[i] - 1;
	{
		uint8_t sum = 0;
		for (int i = 0x0134; i <= 0x014C; ++i)
			sum = static_cast<uint8_t>(sum - c.rom[i] - 1);
		h.checksum_ok = (sum == h.header_sum);
	}

	// ----- MBC selection -----
	// Unlicensed/multicart detection takes priority — both Sachen and
	// Mani-style MMM01 carts forge their $0147 byte (Sachen claims MBC1,
	// Mani claims MBC1/MBC3), so we identify them by structural markers
	// instead. Sachen: bit-permuted logo at $0104-$01FF. MMM01: an
	// additional Nintendo logo at the last 32 KiB of ROM (the boot menu).
	c.flat_unlicensed = false;
	if (LooksLikeSachenScrambledLogo(c.rom))
	{
		c.mbc.type      = MbcType::SachenMMC1;
		c.has_battery   = false;
		c.has_rtc       = false;
		c.has_rumble    = false;
		c.sachen_logo_high = !SachenLogoAt(c.rom, false) && SachenLogoAt(c.rom, true);
		// The RA7-high series keeps its loader inside the permuted window, so
		// the xform has to survive the boot hand-off.
		c.sachen_runs_raw = c.sachen_logo_high ? false : SachenHeaderRunsRaw(c.rom);
	}
	else if (h.cart_type == 0x01 && c.rom.size() == 0x100000 &&
	         LooksLikeMbc1Multicart(c.rom))
	{
		// MBC1M (1 MiB, plain-MBC1 header, sub-game logos on the 256 KiB
		// slots). Checked before MMM01: its per-bank logos also satisfy the
		// end-of-ROM menu-logo probe, which would misfile it as MMM01.
		c.mbc.type    = MbcType::MBC1;
		c.has_battery = false;
		c.has_rtc     = false;
		c.has_rumble  = false;
	}
	else if (LooksLikeMmm01(c.rom))
	{
		c.mbc.type     = MbcType::MMM01;
		// The "real" header for an MMM01 cart lives in the menu bank at
		// the end of ROM, not at file offset 0 (which holds a sub-game
		// header that may be partially forged). Re-read the cart_type /
		// rom_size / ram_size bytes from there so RAM allocation and
		// downstream display reflect what real hardware sees first.
		const size_t menu_base = c.rom.size() - 0x8000;
		h.cart_type = c.rom[menu_base + 0x0147];
		const uint8_t menu_rom_code = c.rom[menu_base + 0x0148];
		h.rom_size = (menu_rom_code <= 8)
		           ? static_cast<uint32_t>(32u * 1024u * (1u << menu_rom_code))
		           : static_cast<uint32_t>(c.rom.size());
		h.ram_size = DecodeRamSize(c.rom[menu_base + 0x0149]);
		// Honor battery from the menu-bank header (cart-type $0D is
		// MMM01+RAM+BATTERY); Mani forgeries report MBC1/MBC3 there
		// so fall back to false unless we see the real MMM01 type.
		c.has_battery = (h.cart_type == 0x0D);
		c.has_rtc     = false;
		c.has_rumble  = false;
	}
	else if (LooksLikeM161(c.rom))
	{
		c.mbc.type    = MbcType::M161;
		c.has_battery = false;
		c.has_rtc     = false;
		c.has_rumble  = false;
	}
	else if (LooksLikeRocket(c.rom))
	{
		// Datel Rocket Games — $0147 reads $97/$99, neither a real type.
		c.mbc.type    = MbcType::Rocket;
		c.has_battery = false;
		c.has_rtc     = false;
		c.has_rumble  = false;
	}
	else if (LooksLikeMakonOld(c.rom, h) != MbcType::None)
	{
		c.mbc.type    = LooksLikeMakonOld(c.rom, h);
		c.has_rtc     = false;
		c.has_rumble  = (c.mbc.type == MbcType::NtOld2);
		// The big NT-old1 menu multicarts carry battery-backed 8K RAM the
		// header never declares (hhugboy's sizing).
		c.has_battery = (c.mbc.type == MbcType::NtOld1 &&
		                 c.rom.size() > 512 * 1024);
		if (c.has_battery && h.ram_size < 8 * 1024) h.ram_size = 8 * 1024;
	}
	else if (LooksLikeScrambledUnl(c.rom) != MbcType::None)
	{
		c.mbc.type    = LooksLikeScrambledUnl(c.rom);
		c.has_battery = false;
		c.has_rtc     = false;
		c.has_rumble  = false;
		// Per-family RAM fitting (hhugboy's sizing where headers lie).
		switch (c.mbc.type)
		{
			case MbcType::Ggb81:
				h.ram_size = 32 * 1024;
				break;
			case MbcType::LiCheng:
			case MbcType::Sintax:
				c.has_battery = true;
				h.ram_size = 32 * 1024;
				break;
			case MbcType::Vf001:
				// Vast Fame boards fit 8K when the header claims any RAM,
				// battery included; some claim none and have none.
				if (h.ram_size > 0)
				{
					c.has_battery = true;
					h.ram_size = 8 * 1024;
				}
				c.vf_alt_board = (std::memcmp(&c.rom[0x184], kLogoVfDigi, 48) == 0);
				c.vf_zook = (std::memcmp(&c.rom[0x134], "ZOCK2000", 8) == 0);
				break;
			default:
				break;
		}
	}
	else if ((c.rom[0x144] == 'M' ? c.rom[0x145] == 'K'
	                              : (c.rom[0x144] == 'G' && c.rom[0x145] == 'C')) &&
	         h.global_sum != 0x1ABB)
	{
		// ($1ABB = the MBC1-converted "Pokemon Adventure" re-dump, which
		// still carries a dead NT arm write - it runs as its header says.)
		// Every other Makon/NT cart (licensee "MK"/"GC"): the later NT
		// board. It banks exactly like MBC5 until a game arms the $14xx
		// split port, so decrypted re-dumps keep working too. Battery-backed
		// 32K RAM per hhugboy - many carts have it undeclared.
		c.mbc.type    = MbcType::NtNew;
		c.has_battery = true;
		c.has_rtc     = false;
		c.has_rumble  = true;
		if (h.ram_size < 32 * 1024) h.ram_size = 32 * 1024;
	}
	else if (DecodeCartType(h.cart_type, c.mbc.type, c.has_battery, c.has_rtc, c.has_rumble))
	{
		// Recognized standard cart type — fields set by DecodeCartType.
	}
	else if (LooksLikeSachenScrambledHeader(c.rom))
	{
		// Logo-less Sachen single-game cart: forged $0147 and no stored logo,
		// but a valid GB header hides under the Sachen address swap. The
		// scrambled header is the cart's real loader, so the xform stays on.
		c.mbc.type        = MbcType::SachenMMC1;
		c.has_battery     = false;
		c.has_rtc         = false;
		c.has_rumble      = false;
		c.sachen_runs_raw = false;
		c.sachen_logo_high = false;
	}
	else if (LooksLikeFlatUnlicensed(h))
	{
		c.mbc.type    = MbcType::MBC1;
		c.flat_unlicensed = true;
		c.has_battery = false;
		c.has_rtc     = false;
		c.has_rumble  = false;
	}
	else
	{
		c.rom.clear();
		return false;
	}
	c.mbc1_multicart = (c.mbc.type == MbcType::MBC1) && LooksLikeMbc1Multicart(c.rom);
	c.duz_multicart  = (c.mbc.type == MbcType::MBC3) && LooksLikeDuzMulticart(c.rom);
	c.mbc30          = (c.mbc.type == MbcType::MBC3) && (c.rom.size() > 0x200000 || h.ram_size > 0x8000);
	// DMG palette-transition unit personality: daid's ppu_scanline_bgp
	// reference photos come from a unit with a clean palette switch, unlike
	// the mealybug blob's OR pixel (Coffee GB keeps the same per-ROM split).
	c.pal_unit = (c.rom.size() == 0x8000 &&
	              c.rom[0x14D] == 0xE7 && c.rom[0x14E] == 0xB6 &&
	              c.rom[0x14F] == 0x86) ? 1 : 0;
	c.mbc5_multicart = (c.mbc.type == MbcType::MBC5) && LooksLikeMbc5Multicart(c.rom);
	// Dev override for unlicensed-cart bring-up: SGB_UNL_FORCE=<MbcType #>.
	if (const char *f = getenv("SGB_UNL_FORCE"))
	{
		const int t = atoi(f);
		if (t > 0 && t <= (int)MbcType::Vf001) c.mbc.type = (MbcType)t;
	}
	MbcReset(c.mbc);
	MbcUnlReset(c);
	// A TAMA5 clock with no save behind it follows the PC's (the load path
	// reseeds it from the RTC settings).
	if (c.mbc.type == MbcType::TAMA5) MbcTama6Seed(c.mbc, static_cast<int64_t>(time(nullptr)));

	// ----- Cart RAM allocation -----
	// Initialize fresh SRAM to $FF. Real hardware SRAM cells with no
	// battery (or first-ever boot with a fresh battery) read as $FF —
	// not zero — and several games use that as the "no save / new game"
	// sentinel. Initial D Gaiden is one: with a zero-init SRAM the game
	// thinks a save exists and walks an in-game-data code path that reads
	// strings out of cart RAM that the new-game flow would have populated,
	// so the dialog box renders empty. Match BGB/mGBA/SameBoy here.
	if (c.mbc.type == MbcType::MBC2)
	{
		// MBC2 has 512 x 4-bit internal RAM regardless of the header RAM
		// size. Real MBC2 internal RAM also defaults to $FF — but only the
		// low nibble is wired, so we'll see $F upper-bits at read time.
		c.sram.assign(512, 0xFF);
	}
	else if (c.mbc.type == MbcType::TAMA5)
	{
		// TAMA5 carts report no RAM in the header ($0149 = 0) but carry a
		// 32-byte EEPROM addressed by the mapper's 5-bit register interface.
		c.sram.assign(0x20, 0xFF);
	}
	else if (c.mbc.type == MbcType::MBC7)
	{
		c.sram.assign(0x100, 0xFF);
	}
	else if (h.ram_size > 0)
	{
		c.sram.assign(h.ram_size, 0xFF);
	}

	c.path = path ? path : "";

	// Pull battery state off disk if available.
	if (c.has_battery) CartLoadBattery(c);

	return true;
}

void CartUnload(Cart &c)
{
	c.rom.clear();
	c.sram.clear();
	c.header      = {};
	c.path.clear();
	c.has_battery    = false;
	c.has_rtc        = false;
	c.has_rumble     = false;
	c.sram_dirty     = false;
	c.mbc1_multicart = false;
	c.duz_multicart  = false;
	c.mbc30          = false;
	c.mbc5_multicart = false;
	c.sachen_runs_raw  = false;
	c.sachen_logo_high = false;
	c.flat_unlicensed  = false;
	c.mbc = MbcState();
	c.huc3_alarm_minutes = c.huc3_alarm_days = 0;
	c.huc3_alarm_on = false;
	std::memset(c.tama6_pages, 0, sizeof c.tama6_pages);
	c.unl = MbcUnl();
	c.vf_alt_board = false;
	c.vf_zook = false;
}

bool CartHasClock(const Cart &c)
{
	return (c.mbc.type == MbcType::MBC3 && c.has_rtc) || c.mbc.type == MbcType::HuC3 ||
	       c.mbc.type == MbcType::TAMA5;
}

namespace {

// BGB's MBC3 block (bgb.bircd.org/rtcsave.html): ten little-endian dwords,
// the counters then the latched copy, and a 64-bit Unix time. Old VBA wrote
// a 32-bit time (44 bytes). VBA-M's HuC3 block: the 64-bit Unix time the
// clock read day 0 0:00, then dwords day << 12 | minute latched and written,
// read mode and nibble shift; it reads no other size. BGB and SameBoy's: the
// time, then 16-bit minutes, days, alarm minutes and days, and the alarm
// switch; BGB keeps the RAM of VBA-M's but not its clock. mGBA's HuC3 block:
// the chip's 256 register nibbles packed low first, then the 64-bit Unix time
// they were read at. mGBA's TAMA5 block: the TAMA6's four pages (timer, alarm,
// two RAM), 16 nibbles each packed low first, then the 64-bit Unix time.
constexpr size_t kMbc3Rtc = 48, kMbc3RtcOld = 44, kHuc3Rtc = 24, kHuc3RtcSameBoy = 17, kHuc3RtcMgba = 136,
                 kTama5Rtc = 40, kRtcMax = 136;
// Before 1997 there were no RTC carts: an older time isn't clock data.
constexpr int64_t kRtcEpochFloor = 852076800;

int64_t RtcNow(const CartRtcOptions &o)
{
	return o.now >= 0 ? o.now : static_cast<int64_t>(time(nullptr));
}

void Put(uint8_t *p, uint64_t v, int bytes)
{
	for (int i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t Get(const uint8_t *p, int bytes)
{
	uint64_t v = 0;
	for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
	return v;
}

size_t RtcEncode(const Cart &c, int64_t now, Huc3RtcFormat huc3, uint8_t out[kRtcMax])
{
	const MbcState &s = c.mbc;
	if (s.type == MbcType::HuC3 && huc3 == Huc3RtcFormat::MGBA)
	{
		// The clock at $10 (mGBA's 12-bit days) and its read copy at $00,
		// where register 6 holds the top day nibble as our clock reads it;
		// the alarm at $58. The time is less the part-minute, as BGB's.
		uint8_t nib[256] = {};
		const uint16_t minutes = MbcHuc3Minutes(s), days = MbcHuc3Days(s);
		for (int k = 0; k < 3; ++k)
		{
			nib[0x00 + k] = nib[0x10 + k] = (minutes >> (4 * k)) & 0x0F;
			nib[0x03 + k] = nib[0x13 + k] = (days >> (4 * k)) & 0x0F;
			nib[0x58 + k] = (c.huc3_alarm_minutes >> (4 * k)) & 0x0F;
		}
		nib[0x06] = days >> 12;
		for (int k = 0; k < 4; ++k) nib[0x5B + k] = (c.huc3_alarm_days >> (4 * k)) & 0x0F;
		nib[0x5F] = c.huc3_alarm_on ? 1 : 0;
		for (int i = 0; i < 128; ++i) out[i] = static_cast<uint8_t>(nib[i * 2] | (nib[i * 2 + 1] << 4));
		Put(out + 128, static_cast<uint64_t>(now - s.rtc_sub_cycles / 4194304), 8);
		return kHuc3RtcMgba;
	}
	if (s.type == MbcType::HuC3 && huc3 == Huc3RtcFormat::BGB)
	{
		// The part-minute counted so far moves the time back, so the next
		// load's catch-up keeps the minute boundaries where they were.
		Put(out, static_cast<uint64_t>(now - s.rtc_sub_cycles / 4194304), 8);
		Put(out + 8, MbcHuc3Minutes(s), 2);
		Put(out + 10, MbcHuc3Days(s), 2);
		Put(out + 12, c.huc3_alarm_minutes, 2);
		Put(out + 14, c.huc3_alarm_days, 2);
		out[16] = c.huc3_alarm_on ? 1 : 0;
		return kHuc3RtcSameBoy;
	}
	if (s.type == MbcType::HuC3)
	{
		// The part-minute counted so far moves the start back too. The written
		// dword keeps all 16 day bits (VBA-M reads 12), so start + clock tells
		// a load when the save was. The write-only alarm doesn't go in.
		const uint16_t minutes = MbcHuc3Minutes(s), days = MbcHuc3Days(s);
		const int64_t  start   = now - s.rtc_sub_cycles / 4194304 - days * int64_t(86400) - minutes * int64_t(60);
		Put(out, static_cast<uint64_t>(start), 8);
		Put(out + 8, ((days & 0xFFFu) << 12) | minutes, 4);
		Put(out + 12, (uint32_t(days) << 12) | minutes, 4);
		Put(out + 16, 1, 4);
		Put(out + 20, 0, 4);
		return kHuc3Rtc;
	}
	if (s.type == MbcType::TAMA5)
	{
		uint8_t pg[4][16] = {};
		for (uint8_t r = 0; r < 13; ++r) pg[0][r] = MbcTama6Reg(s, r);
		for (int p = 0; p < 3; ++p) std::memcpy(pg[p + 1], c.tama6_pages[p], 13);
		pg[1][0xA] = (s.rtc_latched[2] & 1) ? 0 : 1;   // 24-hour
		pg[1][0xB] = s.rtc_latched[3] & 3;   // leap counter
		// The page register: run and alarm bits, then the page number.
		const uint8_t page = static_cast<uint8_t>((s.rtc_latch ? 0x08 : 0) | (s.rtc_latched[4] & 0x04));
		for (int p = 0; p < 4; ++p)
		{
			pg[p][0xD] = static_cast<uint8_t>(page | p);
			for (int i = 0; i < 8; ++i) out[p * 8 + i] = static_cast<uint8_t>(pg[p][i * 2] | (pg[p][i * 2 + 1] << 4));
		}
		Put(out + 32, static_cast<uint64_t>(now), 8);
		return kTama5Rtc;
	}
	std::memset(out, 0, kMbc3Rtc);
	for (int i = 0; i < 5; ++i)
	{
		out[i * 4]      = s.rtc_regs[i];
		out[20 + i * 4] = s.rtc_latched[i];
	}
	Put(out + 40, static_cast<uint64_t>(now), 8);
	return kMbc3Rtc;
}

// Takes the clock from a block of `n` bytes and runs it forward to `now`.
bool RtcDecode(Cart &c, const uint8_t *in, size_t n, int64_t now)
{
	MbcState &s = c.mbc;
	int64_t   saved, oldest = kRtcEpochFloor;
	if (s.type == MbcType::HuC3 && (n == kHuc3Rtc || n == kHuc3Rtc - 4))
	{
		// VBA-M's clock reads now - start, which may lie before 1997. It
		// takes 20 bytes too (old 32-bit builds), the same fields cut short.
		const int64_t  start   = static_cast<int64_t>(Get(in, 8));
		const uint32_t latched = static_cast<uint32_t>(Get(in + 8, 4));
		const uint32_t written = static_cast<uint32_t>(Get(in + 12, 4));
		// Ours repeats the latched clock in the written dword, all 16 day bits.
		const uint32_t clock   = (written & 0xFFFFFF) == latched ? written : latched;
		const uint16_t minutes = clock & 0xFFF, days = static_cast<uint16_t>(clock >> 12);
		// When the clock last read that (ours: the save, less the part-minute).
		const int64_t  stamp   = static_cast<int64_t>(static_cast<uint64_t>(start) + days * 86400ull + minutes * 60ull);
		c.huc3_alarm_minutes = c.huc3_alarm_days = 0;
		c.huc3_alarm_on = false;
		oldest = INT64_MIN;
		if (minutes >= 60 * 24 || stamp > now)
		{
			// Past the day's end (VBA-M's clock can't hold that), or the PC's
			// clock moved back since: as last read, run forward from then.
			MbcHuc3SetClock(s, minutes, days);
			saved = stamp;
		}
		else
		{
			// The day count wraps at 16 bits, so any start further back reads
			// the same as one within the wrap.
			MbcHuc3SetClock(s, 0, 0);
			saved = now - static_cast<int64_t>((static_cast<uint64_t>(now) - static_cast<uint64_t>(start)) % (65536ull * 86400));
		}
	}
	else if (s.type == MbcType::HuC3 && n == kHuc3RtcMgba)
	{
		uint8_t nib[256];
		for (int i = 0; i < 128; ++i)
		{
			nib[i * 2]     = in[i] & 0x0F;
			nib[i * 2 + 1] = static_cast<uint8_t>(in[i] >> 4);
		}
		auto field = [&](int at, int count) {
			uint16_t v = 0;
			for (int k = 0; k < count; ++k) v = static_cast<uint16_t>(v | (nib[at + k] << (4 * k)));
			return v;
		};
		MbcHuc3SetClock(s, field(0x10, 3), static_cast<uint16_t>(field(0x13, 3) | (nib[0x06] << 12)));
		c.huc3_alarm_minutes = field(0x58, 3);
		c.huc3_alarm_days    = field(0x5B, 4);
		c.huc3_alarm_on      = (nib[0x5F] & 1) != 0;
		saved = static_cast<int64_t>(Get(in + 128, 8));
	}
	else if (s.type == MbcType::HuC3)
	{
		if (n != kHuc3RtcSameBoy) return false;
		saved = static_cast<int64_t>(Get(in, 8));
		MbcHuc3SetClock(s, static_cast<uint16_t>(Get(in + 8, 2)), static_cast<uint16_t>(Get(in + 10, 2)));
		c.huc3_alarm_minutes = static_cast<uint16_t>(Get(in + 12, 2));
		c.huc3_alarm_days    = static_cast<uint16_t>(Get(in + 14, 2));
		c.huc3_alarm_on      = (in[16] & 1) != 0;
	}
	else if (s.type == MbcType::TAMA5)
	{
		if (n != kTama5Rtc) return false;
		uint8_t pg[4][16];
		for (int p = 0; p < 4; ++p)
			for (int i = 0; i < 8; ++i)
			{
				pg[p][i * 2]     = in[p * 8 + i] & 0x0F;
				pg[p][i * 2 + 1] = static_cast<uint8_t>(in[p * 8 + i] >> 4);
			}
		for (uint8_t r = 0; r < 13; ++r) MbcTama6SetReg(s, r, pg[0][r]);
		for (int p = 0; p < 3; ++p) std::memcpy(c.tama6_pages[p], pg[p + 1], 13);
		s.rtc_latched[2] = (pg[1][0xA] & 1) ? 0 : 1;   // 12-hour mode
		s.rtc_latched[3] = pg[1][0xB] & 3;
		s.rtc_latched[4] = pg[0][0xD] & 0x04;
		s.rtc_latch      = (pg[0][0xD] & 0x08) != 0;
		saved = static_cast<int64_t>(Get(in + 32, 8));
	}
	else
	{
		if (n != kMbc3Rtc && n != kMbc3RtcOld) return false;
		static const uint8_t kMask[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };
		for (int i = 0; i < 5; ++i)
		{
			s.rtc_regs[i]    = in[i * 4] & kMask[i];
			s.rtc_latched[i] = in[20 + i * 4] & kMask[i];
		}
		saved = static_cast<int64_t>(Get(in + 40, n == kMbc3Rtc ? 8 : 4));
	}
	s.rtc_sub_cycles = 0;
	// A time in the future (or the "can't tell" $7FFFFFFF7FFFFFFF) or before
	// any RTC cart existed runs nothing forward.
	if (saved >= oldest && saved <= now)
		MbcRtcAdvance(s, now - saved);
	return true;
}

// Game.sav -> Game.rtc, keeping a link seat's digit (Game.sav2 -> Game.rtc2).
std::string RtcPath(const char *sav)
{
	std::string p(sav);
	const size_t dot   = p.find_last_of('.');
	const size_t slash = p.find_last_of("/\\");
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
	    p.size() >= dot + 4 &&
	    tolower(static_cast<unsigned char>(p[dot + 1])) == 's' &&
	    tolower(static_cast<unsigned char>(p[dot + 2])) == 'a' &&
	    tolower(static_cast<unsigned char>(p[dot + 3])) == 'v')
		p.replace(dot + 1, 3, "rtc");
	else
		p += ".rtc";
	return p;
}

bool ReadWhole(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	uint8_t chunk[4096];
	for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0; )
		out.insert(out.end(), chunk, chunk + n);
	fclose(f);
	return true;
}

bool WriteWhole(const std::string &path, const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = (an == 0 || fwrite(a, 1, an, f) == an) && (bn == 0 || fwrite(b, 1, bn, f) == bn);
	ok = (fclose(f) == 0) && ok;
	return ok;
}

} // anonymous

bool CartSaveBatteryToPath(const Cart &c, const char *path, const CartRtcOptions &rtc)
{
	const bool clock = CartHasClock(c);
	if (!c.has_battery || (c.sram.empty() && !clock) || !path || !*path) return false;

	uint8_t block[kRtcMax];
	const size_t n = clock ? RtcEncode(c, RtcNow(rtc), rtc.huc3, block) : 0;
	if (!clock || !rtc.separate)
		return WriteWhole(path, c.sram.data(), c.sram.size(), block, n);

	// A clock-only cart has no RAM to put in a .sav of its own.
	const bool ram = c.sram.empty() || WriteWhole(path, c.sram.data(), c.sram.size(), nullptr, 0);
	return WriteWhole(RtcPath(path), block, n, nullptr, 0) && ram;
}

bool CartLoadBatteryFromPath(Cart &c, const char *path, const CartRtcOptions &rtc)
{
	const bool clock = CartHasClock(c);
	if (!c.has_battery || (c.sram.empty() && !clock) || !path || !*path) return false;

	std::vector<uint8_t> sav;
	const bool have_sav = ReadWhole(path, sav);
	const size_t want   = c.sram.size();
	if (have_sav && !sav.empty() && want > 0)
		std::memcpy(c.sram.data(), sav.data(), sav.size() < want ? sav.size() : want);
	bool ok = have_sav && sav.size() >= want;
	if (!clock) return ok;

	// The chosen place first, the other one if it has nothing.
	std::vector<uint8_t> file;
	const bool have_rtc = ReadWhole(RtcPath(path), file);
	const uint8_t *tail = sav.size() > want ? sav.data() + want : nullptr;
	const size_t   tail_n = sav.size() > want ? sav.size() - want : 0;
	const int64_t  now = RtcNow(rtc);
	bool got;
	if (rtc.separate)
		got = (have_rtc && RtcDecode(c, file.data(), file.size(), now)) || (tail && RtcDecode(c, tail, tail_n, now));
	else
		got = (tail && RtcDecode(c, tail, tail_n, now)) || (have_rtc && RtcDecode(c, file.data(), file.size(), now));
	// No TAMA5 clock on disk (a new cart, or a save from before it was kept):
	// it starts at "now", the PC's clock or the picked time.
	if (!got && c.mbc.type == MbcType::TAMA5) MbcTama6Seed(c.mbc, now);
	return ok || (want == 0 && got);
}

bool CartSaveBattery(const Cart &c)
{
	if (c.path.empty()) return false;
	const std::string sav = MakeSavPath(c.path);
	return CartSaveBatteryToPath(c, sav.c_str());
}

bool CartLoadBattery(Cart &c)
{
	if (c.path.empty()) return false;
	const std::string sav = MakeSavPath(c.path);
	return CartLoadBatteryFromPath(c, sav.c_str());
}

} // namespace SGB
