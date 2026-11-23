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
			// Super Black Bass Pocket 2's packet sender clears IF, dropping a partner byte's pending serial
			// IRQ, and its 1 s link watchdog resets both: clear all but the serial bit (stub over listing text).
			{ 0x8FF7, "BASS POCKET 2", "serial IRQ kept through SGB packets", 5,
			  { { 0x000D34, 3, { 0xAF, 0xE0, 0x0F }, { 0xCD, 0x10, 0x00 } },   // xor a / ldh (IF),a -> call $0010
				{ 0x000D61, 3, { 0xAF, 0xE0, 0x0F }, { 0xCD, 0x10, 0x00 } },
				{ 0x000010, 3, { 0x20, 0x20, 0x20 }, { 0xF0, 0x0F, 0xE6 } },   // ldh a,(IF) / and $08
				{ 0x000013, 3, { 0x20, 0x20, 0x20 }, { 0x08, 0xE0, 0x0F } },   // ldh (IF),a
				{ 0x000016, 2, { 0x20, 0x20 }, { 0xAF, 0xC9 } } } },             // xor a / ret
			// Tetris Blast's 2P setup (01:$7081) and 2P game screen (01:$4560) pick no SGB palette, so an SGB stays on
			// the restart's all-white PAL_SET: both draw 1P mode 0's map (04:$77B2), so pick its row ($0D) as it does.
			{ 0xC47E, "TETRIS BLAST", "2P palettes picked on a Super Game Boy", 8,
			  { { 0x006FFB, 3, { 0xCA, 0x81, 0x70 }, { 0xCA, 0x80, 0x3F } },   // 2P setup: jp z,$3F80
				{ 0x0044AA, 2, { 0x60, 0x45 }, { 0x88, 0x3F } },               // mode 3 (2P) screen: $3F88
				{ 0x003F80, 3, { 0xFF, 0xFF, 0xFF }, { 0x3E, 0x0D, 0xEA } },   // ld a,$0D / ld ($C769),a
				{ 0x003F83, 3, { 0xFF, 0xFF, 0xFF }, { 0x69, 0xC7, 0xC3 } },   // jp $7081
				{ 0x003F86, 3, { 0xFF, 0xFF, 0xFF }, { 0x81, 0x70, 0x3E } },   // ld a,$0D / ld ($C769),a
				{ 0x003F89, 3, { 0xFF, 0xFF, 0xFF }, { 0x0D, 0xEA, 0x69 } },
				{ 0x003F8C, 3, { 0xFF, 0xFF, 0xFF }, { 0xC7, 0xC3, 0x60 } },   // jp $4560
				{ 0x003F8F, 1, { 0xFF }, { 0x45 } } } },
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
			// Bakuchou Retrieve Master: on the trading file screen ($C61A=1) the VBlank pad read and the link
			// send's read land in one frame, so player 1's A edge is gone: read once, as the trade itself does.
			{ 0xD954, "RETRIEVE", "file-screen pad read once a frame", 1,
			  { { 0x000363, 3, { 0xFE, 0x02, 0x28 }, { 0xB7, 0x00, 0x20 } } } },   // or a / nop / jr nz
			// Jinsei Game Densetsu: the DMG-07 setup's $C4 / $C8 / $C6 handshake bytes are written for one
			// main-loop pass; a pass that runs past the next packet's start never sends them: hold each one.
			{ 0x56FD, "JINSEI", "setup handshake bytes held", 3,
			  { { 0x001B04, 2, { 0xE0, 0xC4 }, { 0x00, 0x00 } },                  // P1 keeps $C4 until it calls a seat
				{ 0x001ECF, 3, { 0xCD, 0xBC, 0x0E }, { 0x00, 0x00, 0x00 } },     // a seat keeps $C8 until it sees $C6
				{ 0x001B83, 2, { 0xE0, 0xC4 }, { 0x00, 0x00 } } } },             // P1 keeps $C6 until the game sends
			// Tumble Pop: the VS joiner enters the settings screen with its port armed only if the initiator's
			// F0 crossed its last F1; else both reset ($FF x16 / 128-frame watchdog): arm it at state 2.
			{ 0x006D, "TUMBLE POP", "VS joiner armed on the settings screen", 11,
			  { { 0x000865, 3, { 0x3E, 0x02, 0xEA }, { 0xCD, 0xE0, 0x3F } },   // call $3FE0
				{ 0x000868, 2, { 0xF7, 0xD1 }, { 0x00, 0x00 } },
				{ 0x003FE0, 3, { 0xFF, 0xFF, 0xFF }, { 0x3E, 0x02, 0xEA } },   // ld a,2 / ld ($D1F7),a
				{ 0x003FE3, 3, { 0xFF, 0xFF, 0xFF }, { 0xF7, 0xD1, 0xFA } },   // ld a,($D1F8)
				{ 0x003FE6, 3, { 0xFF, 0xFF, 0xFF }, { 0xF8, 0xD1, 0xFE } },   // cp $F1 / ret nz
				{ 0x003FE9, 3, { 0xFF, 0xFF, 0xFF }, { 0xF1, 0xC0, 0xF0 } },   // ldh a,(SC)
				{ 0x003FEC, 3, { 0xFF, 0xFF, 0xFF }, { 0x02, 0x87, 0xD8 } },   // add a,a / ret c
				{ 0x003FEF, 3, { 0xFF, 0xFF, 0xFF }, { 0xFA, 0xFA, 0xD1 } },   // ld a,($D1FA)
				{ 0x003FF2, 3, { 0xFF, 0xFF, 0xFF }, { 0xE0, 0x01, 0x3E } },   // ldh (SB),a / ld a,$80
				{ 0x003FF5, 3, { 0xFF, 0xFF, 0xFF }, { 0x80, 0xE0, 0x02 } },   // ldh (SC),a
				{ 0x003FF8, 1, { 0xFF }, { 0xC9 } } } },                         // ret
			{ 0xF763, "TUMBLE POP", "VS joiner armed on the settings screen", 11,
			  { { 0x000859, 3, { 0x3E, 0x02, 0xEA }, { 0xCD, 0xE0, 0x3F } },
				{ 0x00085C, 2, { 0xF7, 0xD1 }, { 0x00, 0x00 } },
				{ 0x003FE0, 3, { 0xFF, 0xFF, 0xFF }, { 0x3E, 0x02, 0xEA } },
				{ 0x003FE3, 3, { 0xFF, 0xFF, 0xFF }, { 0xF7, 0xD1, 0xFA } },
				{ 0x003FE6, 3, { 0xFF, 0xFF, 0xFF }, { 0xF8, 0xD1, 0xFE } },
				{ 0x003FE9, 3, { 0xFF, 0xFF, 0xFF }, { 0xF1, 0xC0, 0xF0 } },
				{ 0x003FEC, 3, { 0xFF, 0xFF, 0xFF }, { 0x02, 0x87, 0xD8 } },
				{ 0x003FEF, 3, { 0xFF, 0xFF, 0xFF }, { 0xFA, 0xFA, 0xD1 } },
				{ 0x003FF2, 3, { 0xFF, 0xFF, 0xFF }, { 0xE0, 0x01, 0x3E } },
				{ 0x003FF5, 3, { 0xFF, 0xFF, 0xFF }, { 0x80, 0xE0, 0x02 } },
				{ 0x003FF8, 1, { 0xFF }, { 0xC9 } } } },
			// WWF King of the Ring probes the cable once, 4 s after power-on ($3C59): a partner still in its boot ROM (a GB
			// next to an SGB2) misses it and the prober later hangs on the LJN logo: probe every frame until linked (~20 s).
			// A miss lets a colliding probe's byte land (~2 ms), then re-arms the $F1 answer ($1F4F, as at power-on); a
			// console a partner clocked ($FFEA) stops probing and joins through $1FA0 instead of probing a host.
			{ 0xCF18, "WWFKOR", "cable probed until the partner answers", 18,
			  { { 0x003C59, 3, { 0xCD, 0x00, 0x20 }, { 0xCD, 0x61, 0x00 } },   // call $0061
				{ 0x000061, 3, { 0xFF, 0xFF, 0xFF }, { 0x01, 0x58, 0x02 } },     // ld bc,600
				{ 0x000064, 3, { 0xFF, 0xFF, 0xFF }, { 0xC5, 0xFA, 0xA0 } },     // push bc / ld a,($C3A0) ...
				{ 0x000067, 3, { 0xFF, 0xFF, 0xFF }, { 0xC3, 0xB7, 0x20 } },     // ... / or a / jr nz,...
				{ 0x00006A, 3, { 0xFF, 0xFF, 0xFF }, { 0x25, 0xF0, 0xEA } },     // ... $0090 (linked) / ldh a,($FFEA)
				{ 0x00006D, 3, { 0xFF, 0xFF, 0xFF }, { 0xB7, 0x20, 0x14 } },     // or a / jr nz,$0084 (clocked: join)
				{ 0x000070, 3, { 0xFF, 0xFF, 0xFF }, { 0xCD, 0x00, 0x20 } },     // call $2000 (probe)
				{ 0x000073, 3, { 0xFF, 0xFF, 0xFF }, { 0xFA, 0xA0, 0xC3 } },     // ld a,($C3A0)
				{ 0x000076, 3, { 0xFF, 0xFF, 0xFF }, { 0xB7, 0x20, 0x17 } },     // or a / jr nz,$0090 (linked)
				{ 0x000079, 3, { 0xFF, 0xFF, 0xFF }, { 0xAF, 0x00, 0x00 } },     // xor a / nop / nop ...
				{ 0x00007C, 3, { 0xFF, 0xFF, 0xFF }, { 0x00, 0x00, 0x3D } },     // ... nop / nop / dec a
				{ 0x00007F, 3, { 0xFF, 0xFF, 0xFF }, { 0x20, 0xF9, 0xCD } },     // jr nz,$007A (~2 ms) / call ...
				{ 0x000082, 3, { 0xFF, 0xFF, 0xFF }, { 0x4F, 0x1F, 0xC1 } },     // ... $1F4F (answer $F1 again) / pop bc
				{ 0x000085, 3, { 0xFF, 0xFF, 0xFF }, { 0x0B, 0x78, 0xB1 } },     // dec bc / ld a,b / or c
				{ 0x000088, 3, { 0xFF, 0xFF, 0xFF }, { 0xC8, 0xC5, 0xCD } },     // ret z / push bc / call ...
				{ 0x00008B, 3, { 0xFF, 0xFF, 0xFF }, { 0xE4, 0x1E, 0xC1 } },     // ... $1EE4 (wait a frame) / pop bc
				{ 0x00008E, 3, { 0xFF, 0xFF, 0xFF }, { 0x18, 0xD4, 0xC1 } },     // jr $0064 / pop bc
				{ 0x000091, 1, { 0xFF }, { 0xC9 } } } },                       // ret
			{ 0x8A2D, "WWFKOR", "cable probed until the partner answers", 18,
			  { { 0x003C59, 3, { 0xCD, 0x00, 0x20 }, { 0xCD, 0x61, 0x00 } },
				{ 0x000061, 3, { 0xFF, 0xFF, 0xFF }, { 0x01, 0x58, 0x02 } },
				{ 0x000064, 3, { 0xFF, 0xFF, 0xFF }, { 0xC5, 0xFA, 0xA0 } },
				{ 0x000067, 3, { 0xFF, 0xFF, 0xFF }, { 0xC3, 0xB7, 0x20 } },
				{ 0x00006A, 3, { 0xFF, 0xFF, 0xFF }, { 0x25, 0xF0, 0xEA } },
				{ 0x00006D, 3, { 0xFF, 0xFF, 0xFF }, { 0xB7, 0x20, 0x14 } },
				{ 0x000070, 3, { 0xFF, 0xFF, 0xFF }, { 0xCD, 0x00, 0x20 } },
				{ 0x000073, 3, { 0xFF, 0xFF, 0xFF }, { 0xFA, 0xA0, 0xC3 } },
				{ 0x000076, 3, { 0xFF, 0xFF, 0xFF }, { 0xB7, 0x20, 0x17 } },
				{ 0x000079, 3, { 0xFF, 0xFF, 0xFF }, { 0xAF, 0x00, 0x00 } },
				{ 0x00007C, 3, { 0xFF, 0xFF, 0xFF }, { 0x00, 0x00, 0x3D } },
				{ 0x00007F, 3, { 0xFF, 0xFF, 0xFF }, { 0x20, 0xF9, 0xCD } },
				{ 0x000082, 3, { 0xFF, 0xFF, 0xFF }, { 0x4F, 0x1F, 0xC1 } },
				{ 0x000085, 3, { 0xFF, 0xFF, 0xFF }, { 0x0B, 0x78, 0xB1 } },
				{ 0x000088, 3, { 0xFF, 0xFF, 0xFF }, { 0xC8, 0xC5, 0xCD } },
				{ 0x00008B, 3, { 0xFF, 0xFF, 0xFF }, { 0xE4, 0x1E, 0xC1 } },
				{ 0x00008E, 3, { 0xFF, 0xFF, 0xFF }, { 0x18, 0xD4, 0xC1 } },
				{ 0x000091, 1, { 0xFF }, { 0xC9 } } } },
			// Tetris Blast 2P: an SGB console sends its screen-change PAL_SETs with IRQs off (~20 frames of
			// $13D0 waits); a GB partner's 16-frame $FF watchdog ($02B6) resets both first: allow 64 frames.
			{ 0xC47E, "TETRIS BLAST", "link-lost watchdog outlasts an SGB screen change", 1,
			  { { 0x0002BD, 2, { 0xFE, 0x10 }, { 0xFE, 0x40 } } } },   // cp $40
			// Army Men 2's menus count six HALTs ($1099) to start the HiColor palettes in line 5's HBlank; a link byte's
			// serial IRQ wakes one, so the picture starts a line early: HALT until HBlank of line 5 or later instead.
			{ 0xC2F5, "ARMY MEN 2", "menu picture timed by LY, not by wakeups", 7,
			  { { 0x001099, 3, { 0x76, 0x00, 0x76 }, { 0xC3, 0x00, 0x3F } },   // jp $3F00
				{ 0x003F00, 3, { 0xFF, 0xFF, 0xFF }, { 0x76, 0x00, 0xF0 } },   // halt / nop / ldh a,(STAT)
				{ 0x003F03, 3, { 0xFF, 0xFF, 0xFF }, { 0x41, 0xE6, 0x03 } },   // and 3
				{ 0x003F06, 3, { 0xFF, 0xFF, 0xFF }, { 0x20, 0xF8, 0xF0 } },   // jr nz,$3F00 / ldh a,(LY)
				{ 0x003F09, 3, { 0xFF, 0xFF, 0xFF }, { 0x44, 0xFE, 0x05 } },   // cp 5
				{ 0x003F0C, 3, { 0xFF, 0xFF, 0xFF }, { 0x38, 0xF2, 0xC3 } },   // jr c,$3F00 / jp $103A
				{ 0x003F0F, 2, { 0xFF, 0xFF }, { 0x3A, 0x10 } } } },
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
