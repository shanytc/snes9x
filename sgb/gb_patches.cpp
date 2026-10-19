/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_patches.h"

#include <cstring>

namespace SGB {

	namespace {

		// Super Game Boy compatibility: the cart's own SGB path stops under a BIOS the
		// game never handled. Applied under any SGB BIOS.
		const GbCartPatch kSgbCartPatches[] = {
			// Boot A $FF (SGB2) survives the detect, and the first scene then loops
			// forever (01:$4052 `cp $FF / jr nz`): take the jump for every type.
			{ 0x20D7, "TUWAMONO   A55J\200", "SGB2 park skipped", 1,
			  { { 0x004057, 2, { 0x20, 0x05 }, { 0x18, 0x05 } } } },
			// The MASK_EN cancel after its border goes out with IME on and the VBlank
			// joypad poll lands in the packet ($B9 00 -> $9A 0B): send it via a DI/EI stub.
			{ 0xE11B, "Super Fight's", "MASK_EN cancel sent with IRQs off", 3,
			  { { 0x0002AD, 3, { 0xCD, 0xC0, 0x02 }, { 0xCD, 0xCA, 0x3F } },
				{ 0x003FCA, 3, { 0xFF, 0xFF, 0xFF }, { 0xF3, 0xCD, 0xC0 } },
				{ 0x003FCD, 3, { 0xFF, 0xFF, 0xFF }, { 0x02, 0xFB, 0xC9 } } } },
			// Its SGB packet waits run with IRQs off for frames and lose 4-player adapter
			// bytes: EI before the LY wait (01:$7271) and around the between-packet delay.
			{ 0xC29F, "UNO2SMALL WORLD", "SGB packet waits with IRQs on", 6,
			  { { 0x007271, 3, { 0xF0, 0x44, 0xA7 }, { 0xF1, 0xE0, 0xFF } },
				{ 0x007274, 3, { 0x20, 0xFB, 0xF1 }, { 0xFB, 0xF0, 0x44 } },
				{ 0x007277, 2, { 0xE0, 0xFF }, { 0xA7, 0x20 } },
				{ 0x0072AC, 3, { 0xCD, 0x37, 0x73 }, { 0xCD, 0xD0, 0x3F } },
				{ 0x003FD0, 3, { 0xE1, 0x7B, 0xFF }, { 0xFB, 0xCD, 0x37 } },
				{ 0x003FD3, 3, { 0x83, 0x7D, 0xE7 }, { 0x73, 0xF3, 0xC9 } } } },
		};

		bool TitleMatches(const uint8_t *rom, const char *title)
		{
			for (int i = 0; i < 16; ++i)
			{
				const uint8_t want = static_cast<uint8_t>(title[i]);
				if (want == 0) return rom[0x134 + i] == 0;
				if (rom[0x134 + i] != want) return false;
			}
			return true;
		}

		const GbCartPatch *FindIn(const GbCartPatch *table, size_t count, const uint8_t *rom, size_t size)
		{
			if (!rom || size < 0x150) return nullptr;
			const uint16_t gsum = static_cast<uint16_t>((rom[0x14E] << 8) | rom[0x14F]);
			for (size_t i = 0; i < count; ++i)
				if (table[i].global_sum == gsum && TitleMatches(rom, table[i].title)) return &table[i];
			return nullptr;
		}

	}

	const GbCartPatch *FindSgbCartPatch(const uint8_t *rom, size_t size)
	{
		return FindIn(kSgbCartPatches, sizeof kSgbCartPatches / sizeof kSgbCartPatches[0], rom, size);
	}

	bool ApplyGbCartPatch(const GbCartPatch &p, uint8_t *rom, size_t size)
	{
		if (!rom) return false;
		const int n = p.edit_count < kGbCartPatchMaxEdits ? p.edit_count : kGbCartPatchMaxEdits;
		for (int i = 0; i < n; ++i)
		{
			const GbCartEdit &e = p.edits[i];
			if (e.len > 3 || e.addr + e.len > size) return false;
			if (std::memcmp(rom + e.addr, e.old, e.len) != 0) return false;
		}
		for (int i = 0; i < n; ++i)
		{
			const GbCartEdit &e = p.edits[i];
			std::memcpy(rom + e.addr, e.neu, e.len);
		}
		return true;
	}

} // namespace SGB
