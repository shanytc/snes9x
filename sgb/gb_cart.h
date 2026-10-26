/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_CART_H_
#define _SGB_GB_CART_H_

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "gb_mbc.h"

namespace SGB {

// Cart header lives at 0x0100-0x014F on every GB ROM.
struct CartHeader
{
	char     title[17]   = {0};
	uint8_t  cgb_flag    = 0;
	uint8_t  sgb_flag    = 0;
	uint8_t  cart_type   = 0;
	uint32_t rom_size    = 0;  // bytes
	uint32_t ram_size    = 0;
	uint8_t  dest_code   = 0;
	uint8_t  header_sum  = 0;
	uint16_t global_sum  = 0;
	bool     checksum_ok = false;
};

struct Cart
{
	CartHeader            header;
	std::vector<uint8_t>  rom;
	std::vector<uint8_t>  sram;
	MbcState              mbc;
	std::string           path;
	bool                  has_battery = false;
	bool                  has_rtc     = false;
	bool                  has_rumble  = false;
	bool                  sram_dirty  = false;
	// MBC1 multicart wiring (MBC1M): BANK2 selects a 256 KiB game slot. Static
	// per-ROM, re-derived at load — kept out of MbcState so it never touches
	// the blob-serialized savestate layout.
	bool                  mbc1_multicart = false;
	// Duz "2-in-1" MBC3 multicart: $C0 to $0000 unlocks an $A000/$A100 register port whose reg $A3<<1 is the ROM base bank added to every access.
	bool                  duz_multicart  = false;
	// MBC30 (Pokemon Crystal, Japan): an MBC3 with twice the reach, an 8-bit
	// ROM bank for 4 MiB. Told apart, as SameBoy does, by ROM over 2 MiB or
	// RAM over 32 KiB; static per ROM like mbc1_multicart.
	bool                  mbc30          = false;
	// "23 in 1" MBC5 multicart: $5001/$5002 latch a base bank and a bank mask,
	// both in 2-bank units (register value << 1).
	bool                  mbc5_multicart = false;
	bool                  sachen_runs_raw = false;
	// $0147 was garbage but a real logo+entry ran it as plain MBC1
	// (Gowin / SONIC5 / Xploder-style flat unlicensed boards).
	bool                  flat_unlicensed = false;
	// HuC3 alarm registers ($58-$5F; the speaker that sounds it is not
	// emulated). Games can't read them back, so savestates leave them out;
	// the battery save's clock block carries them.
	uint16_t              huc3_alarm_minutes = 0;
	uint16_t              huc3_alarm_days    = 0;
	bool                  huc3_alarm_on      = false;
	// TAMA5's TAMA6 alarm page and two RAM pages (the timer page is in
	// MbcState). Like the HuC3 alarm, only the battery save carries them.
	uint8_t               tama6_pages[3][13] = {};
	// Scrambled-unlicensed mapper state (BBD/Sintax/VF etc.), v7 savestates.
	MbcUnl                unl;
	// Vast Fame "A" boards seed the protection running value with $10.
	bool                  vf_alt_board = false;
	// Zook Z (USA): the English board's code-driven banking and challenge chip.
	bool                  vf_zook = false;
	// Sachen carts that keep the boot logo in the RA7-high half of the
	// permuted $0100-$01FF window (the combo/31-in-1 series).
	bool                  sachen_logo_high = false;
	uint8_t               camera_regs[0x36] = {0};
	// DMG palette-transition unit this ROM's reference photos came from:
	// 0 = mealybug's blob unit (old|new transition pixel), 1 = daid's
	// (clean switch). Derived from the header at load.
	uint8_t               pal_unit = 0;
};

bool CartLoad(Cart &c, const uint8_t *data, size_t size, const char *path);
void CartUnload(Cart &c);

// Persist battery-backed SRAM (and RTC if present) to ".sav" alongside the ROM.
bool CartSaveBattery(const Cart &c);
bool CartLoadBattery(Cart &c);

// An MBC3 cart with its RTC, a HuC3 or a TAMA5: a clock the battery save carries.
bool CartHasClock(const Cart &c);

// A HuC3 clock's block, as Settings.GBRTCHuC3Format numbers them: VBA-M's 24
// bytes, BGB/SameBoy's 17 or mGBA's 136. Loads take any of them by size.
enum class Huc3RtcFormat : uint8_t { VBA = 0, BGB = 1, MGBA = 2 };

// Where the clock goes and what "now" is. The clock follows the RAM in the
// .sav (MBC3: BGB/VBA-M's 48 bytes, 44 also read; HuC3: as `huc3` picks;
// TAMA5: mGBA's 40), or sits in a .rtc beside it. `now` (Unix seconds, -1 =
// the PC's clock) is written with it, and on load the clock runs forward by
// the time since.
struct CartRtcOptions
{
	bool          separate = false;
	Huc3RtcFormat huc3     = Huc3RtcFormat::VBA;
	int64_t       now      = -1;
};

bool CartSaveBatteryToPath(const Cart &c, const char *path, const CartRtcOptions &rtc = CartRtcOptions());
bool CartLoadBatteryFromPath(Cart &c, const char *path, const CartRtcOptions &rtc = CartRtcOptions());

// While the boot ROM is mapped, a Sachen cart's mapper feeds the boot the
// Nintendo logo at 0x0104-0x0133 (logo-less single-game carts store none, and
// even the 4B multicarts only descramble to it) so the DMG/SGB boot logo check
// passes — exactly as the hardware does. Returns true and the canonical byte
// for a SachenMMC1 cart in that range; callers gate this on boot_rom_enabled so
// the running game still reads its real (address-swapped) header afterwards.
bool SachenBootLogoByte(const Cart &c, uint16_t addr, uint8_t &out);

// Rocket Games carts store Datel's own logo in the header; the mapper morphs
// $0104-$0133 into the Nintendo logo while the boot ROM reads it, so the CGB
// boot check passes and the game later reads its real (Rocket) header. DMG
// boots read it raw and fail — the carts are GBC-only, as on hardware.
bool RocketBootLogoByte(const Cart &c, uint16_t addr, uint8_t &out);

// A Hudson cart with its own infrared LED and sensor (GB KISS, gbkiss.org/cart).
bool CartHasIrPort(const Cart &c);

} // namespace SGB

void S9xGBSetCameraCallback(bool (*cb)(unsigned char *dst, int width, int height));

#endif
