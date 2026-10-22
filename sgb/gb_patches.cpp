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
		};

		// Link-cable fixes for code only an SGB BIOS runs: in the cart while a link session runs.
		const GbCartPatch kSgbLinkCartPatches[] = {
			// Its SGB packet waits run with IRQs off for frames and lose 4-player adapter
			// bytes: EI before the LY wait (01:$7271) and around the between-packet delay.
			{ 0xC29F, "UNO2SMALL WORLD", "SGB packet waits with IRQs on", 6,
			  { { 0x007271, 3, { 0xF0, 0x44, 0xA7 }, { 0xF1, 0xE0, 0xFF } },
				{ 0x007274, 3, { 0x20, 0xFB, 0xF1 }, { 0xFB, 0xF0, 0x44 } },
				{ 0x007277, 2, { 0xE0, 0xFF }, { 0xA7, 0x20 } },
				{ 0x0072AC, 3, { 0xCD, 0x37, 0x73 }, { 0xCD, 0xD0, 0x3F } },
				{ 0x003FD0, 3, { 0xE1, 0x7B, 0xFF }, { 0xFB, 0xCD, 0x37 } },
				{ 0x003FD3, 3, { 0x83, 0x7D, 0xE7 }, { 0x73, 0xF3, 0xC9 } } } },
			// Jantaku Boy refuses 2P VS / MULTI once it detects an SGB (1A:$6593) and greys them
			// (ATTR_BLK 1A:$6503, palette 2); the SGB2 has a link port, so offer and color both.
			{ 0x4209, "GALLERY3", "link modes offered on a Super Game Boy", 2,
			  { { 0x06A596, 3, { 0xA7, 0x28, 0x09 }, { 0xA7, 0x18, 0x09 } },
				{ 0x06A50B, 3, { 0x03, 0x0A, 0x07 }, { 0x03, 0x0F, 0x07 } } } },
		};

		// Link-cable fixes on every model: in the cart while a link session runs.
		// Uno's VBlank handler calls its sound driver with IRQs on; a VBlank held
		// through a link bulk can fire late enough for the next one to re-enter it.
		// Both calls go through a stub: busy flag $DA00 set -> leave by the handler's exit.
		const GbCartPatch kGbLinkCartPatches[] = {
			{ 0xC315, "UNO-SMALL WORLD", "nested VBlank skips the sound driver", 9,
			  { { 0x00015B, 3, { 0xCD, 0x80, 0x08 }, { 0xCD, 0x60, 0x3F } },
				{ 0x00019B, 3, { 0xCD, 0x80, 0x08 }, { 0xCD, 0x60, 0x3F } },
				{ 0x003F60, 3, { 0xFF, 0xFF, 0xFF }, { 0x21, 0x00, 0xDA } },
				{ 0x003F63, 3, { 0xFF, 0xFF, 0xFF }, { 0xCB, 0x46, 0x20 } },
				{ 0x003F66, 3, { 0xFF, 0xFF, 0xFF }, { 0x09, 0x34, 0xCD } },
				{ 0x003F69, 3, { 0xFF, 0xFF, 0xFF }, { 0x80, 0x08, 0x21 } },
				{ 0x003F6C, 3, { 0xFF, 0xFF, 0xFF }, { 0x00, 0xDA, 0x35 } },
				{ 0x003F6F, 3, { 0xFF, 0xFF, 0xFF }, { 0xC9, 0xE1, 0xC3 } },
				{ 0x003F72, 2, { 0xFF, 0xFF }, { 0xC8, 0x01 } } } },
			{ 0xC29F, "UNO2SMALL WORLD", "nested VBlank skips the sound driver", 9,
			  { { 0x000305, 3, { 0xCD, 0xED, 0x09 }, { 0xCD, 0xE0, 0x3F } },
				{ 0x00034E, 3, { 0xCD, 0xED, 0x09 }, { 0xCD, 0xE0, 0x3F } },
				{ 0x003FE0, 3, { 0xEB, 0x03, 0xDF }, { 0x21, 0x00, 0xDA } },
				{ 0x003FE3, 3, { 0xBF, 0x61, 0xE7 }, { 0xCB, 0x46, 0x20 } },
				{ 0x003FE6, 3, { 0x8F, 0x7F, 0x8F }, { 0x09, 0x34, 0xCD } },
				{ 0x003FE9, 3, { 0x3B, 0x43, 0xB7 }, { 0xED, 0x09, 0x21 } },
				{ 0x003FEC, 3, { 0xBB, 0xFF, 0xEF }, { 0x00, 0xDA, 0x35 } },
				{ 0x003FEF, 3, { 0xFB, 0xF5, 0xB7 }, { 0xC9, 0xE1, 0xC3 } },
				{ 0x003FF2, 2, { 0x33, 0xF9 }, { 0x7B, 0x03 } } } },
			// Namco Gallery Vol.3's Jantaku Boy arms its link port before its SGB check and sets
			// the packet length after it; bytes arriving between walk the ISR index into IE.
			{ 0x4209, "GALLERY3", "link packet length set before the port is armed", 1,
			  { { 0x000DBF, 3, { 0x00, 0xE0, 0x02 }, { 0x04, 0xE0, 0xD1 } } } },
			// Mach Go Go Go: an SGB console arms for the race prompt ~0.6 s after a Game Boy partner, whose
			// confirm ($30) can meet an unarmed port; its F8 polls are then ignored: take F8 as the confirm.
			{ 0x8BA1, "MACH GO", "race confirm taken from the partner's poll", 5,
			  { { 0x0078E5, 3, { 0xE6, 0x70, 0xFE }, { 0xCD, 0xC0, 0x3F } },
				{ 0x0078E8, 3, { 0x30, 0x28, 0x10 }, { 0x28, 0x11, 0x00 } },
				{ 0x003FC0, 3, { 0xFF, 0xFF, 0xFF }, { 0xFE, 0xF8, 0xC8 } },   // cp $F8 / ret z
				{ 0x003FC3, 3, { 0xFF, 0xFF, 0xFF }, { 0xE6, 0x70, 0xFE } },   // and $70 / cp $30 / ret
				{ 0x003FC6, 2, { 0xFF, 0xFF }, { 0x30, 0xC9 } } } },
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

	const GbCartPatch *FindSgbLinkCartPatch(const uint8_t *rom, size_t size)
	{
		return FindIn(kSgbLinkCartPatches, sizeof kSgbLinkCartPatches / sizeof kSgbLinkCartPatches[0], rom, size);
	}

	const GbCartPatch *FindGbLinkCartPatch(const uint8_t *rom, size_t size)
	{
		return FindIn(kGbLinkCartPatches, sizeof kGbLinkCartPatches / sizeof kGbLinkCartPatches[0], rom, size);
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

	bool UndoGbCartPatch(const GbCartPatch &p, uint8_t *rom, size_t size)
	{
		if (!rom) return false;
		const int n = p.edit_count < kGbCartPatchMaxEdits ? p.edit_count : kGbCartPatchMaxEdits;
		for (int i = 0; i < n; ++i)
		{
			const GbCartEdit &e = p.edits[i];
			if (e.len > 3 || e.addr + e.len > size) return false;
			if (std::memcmp(rom + e.addr, e.neu, e.len) != 0) return false;
		}
		for (int i = 0; i < n; ++i)
		{
			const GbCartEdit &e = p.edits[i];
			std::memcpy(rom + e.addr, e.old, e.len);
		}
		return true;
	}

} // namespace SGB
