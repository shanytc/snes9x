/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "sgbc_patches.h"

#include "../sha256.h"

#include <cstring>

namespace SGB {

namespace {

// kSgbcBiosBaseSha256[32], kSgbcBiosIps[], kSgbcBiosIpsSize. Built by
// sgb-gbc-bios/tools/build.py --inc from src/sgbc_patch.asm.
#include "sgbc_bios_ips.inc"

// ---- Per-game edits ------------------------------------------------------
// Found by s9x-harness/sgbcfind.exe (boot with the SGB2 signature and with the
// CGB signature, diff the instruction streams, take the branch the Color path
// skips the SGB init on) and verified by sgbc_sweep.py under the SGB2 BIOS:
// the edit must bring back the cart's SGB traffic with the GB still alive.
// Regenerate with sgbc_gentable.py; do not edit the block by hand.
//
// Patterns: nop_skip_branch / nop_early_ret = the Color path jumps over (or
// returns before) the SGB code the SGB path falls into, so the branch is
// NOPed; retarget_jr/jp_to_sgb_entry = the Color init block ends in a jump
// over the SGB block (Dragon Warrior's shape), so that jump lands on the SGB
// block instead and both run.
// GENERATED-BEGIN
const SgbcPatch kPatches[] = {
	// nop_early_ret
	{ 0x8A70, "POKEMON_GLDAAUJ\200", "36 in 1 (Taiwan) (SL36-0032) (Unl)", 1,
	  { { 0x009C59, 1, { 0xC0 }, { 0x00 } } } },
	// manual: flag routine answers SGB2
	{ 0x4AF7, "ANI3", "Animal Breeder 3 (Japan)", 1,
	  { { 0x000254, 2, { 0xCB, 0x4E }, { 0xAF, 0x00 } } } },
	// nop_early_ret
	{ 0xBAC0, "BALLOON GB", "Balloon Fight GB (Japan) (SGB Enhanced, GB Compatible) (NP)", 1,
	  { { 0x003B41, 1, { 0xC8 }, { 0x00 } } } },
	// nop_skip_branch
	{ 0x9CD2, "BARDIGUN", "Barcode Taisen Bardigun (Japan) (Rev 1)", 1,
	  { { 0x000193, 3, { 0xC2, 0x2B, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x6824, "BARDIGUN", "Barcode Taisen Bardigun (Japan)", 1,
	  { { 0x000182, 3, { 0xC2, 0x1B, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x20D8, "B MASTER", "Blaster Master - Enemy Below (USA, Europe)", 1,
	  { { 0x00016E, 2, { 0x18, 0x1D }, { 0x18, 0x00 } } } },
	// nop_skip_branch
	{ 0xAC39, "BOMBERQUESTAQVP\200", "Bomberman Quest (Europe) (En,Fr,De)", 1,
	  { { 0x0001BE, 3, { 0xCA, 0x72, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xE735, "BOMBERMAN QUEST\200", "Bomberman Quest (Japan)", 1,
	  { { 0x0001C3, 3, { 0xCA, 0x77, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xEAA7, "BOMBERQUESTAVQE\200", "Bomberman Quest (USA)", 1,
	  { { 0x0001C3, 3, { 0xCA, 0x77, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// force_jr
	{ 0xB84C, "BUGS LIFE", "Bug's Life, A (Europe)", 1,
	  { { 0x000265, 2, { 0x20, 0x4A }, { 0x18, 0x4A } } } },
	// force_jr
	{ 0xB74C, "BUGS LIFE", "Bug's Life, A (USA)", 1,
	  { { 0x000265, 2, { 0x20, 0x4A }, { 0x18, 0x4A } } } },
	// nop_skip_branch
	{ 0x015F, "CHASE HQ", "Chase H.Q. - Secret Police (Europe)", 1,
	  { { 0x0001C0, 2, { 0x28, 0x12 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x06D2, "CHASE HQ", "Chase H.Q. - Secret Police (USA)", 1,
	  { { 0x0001C0, 2, { 0x28, 0x12 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xE7F0, "CHOROQ HCGBACQJ\200", "Choro Q - Hyper Customable GB (Japan)", 1,
	  { { 0x003171, 2, { 0x28, 0x2D }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xC35C, "BUBBOB", "Classic Bubble Bobble (Europe)", 1,
	  { { 0x0001A6, 3, { 0xCA, 0x8D, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xCD62, "BUBBOB", "Classic Bubble Bobble (USA)", 1,
	  { { 0x0001A6, 3, { 0xCA, 0x8D, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x7961, "C C RACING ARLP\200", "Cross Country Racing (Europe) (En,Fr,De)", 1,
	  { { 0x0001D2, 3, { 0xCA, 0xE0, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// nop_early_ret
	{ 0x24D4, "DINO4", "Dino Breeder 4 (Japan)", 1,
	  { { 0x002EF5, 1, { 0xC8 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x3FDF, "DOKAPON", "Dokapon! - Millennium Quest (Japan)", 1,
	  { { 0x1ED106, 1, { 0xC8 }, { 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0xC280, "DQ1&2", "Dragon Quest I & II (Japan)", 1,
	  { { 0x0001BE, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0xF95B, "DQM2-I", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Iru no Bouken (Japan)", 1,
	  { { 0x0001D1, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x05BA, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (Rev 1)", 1,
	  { { 0x0001D1, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x04BC, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan)", 1,
	  { { 0x0001D1, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x2349, "DW1&2", "Dragon Warrior I & II (USA)", 1,
	  { { 0x0001BE, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x859E, "DWM2-C", "Dragon Warrior Monsters 2 - Cobi's Journey (USA)", 1,
	  { { 0x0001D1, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x853C, "DWM2-T", "Dragon Warrior Monsters 2 - Tara's Adventure (USA)", 1,
	  { { 0x0001D1, 2, { 0x18, 0x05 }, { 0x18, 0x00 } } } },
	// nop_skip_branch
	{ 0xA73A, "DT GAMEBOY BBDJ\200", "DT - Lords of Genomes (Japan)", 1,
	  { { 0x0001B4, 2, { 0x28, 0x06 }, { 0x00, 0x00 } } } },
	// nop_early_ret
	{ 0xFC46, "MONOPOLYGB", "DX Monopoly GB (Japan)", 2,
	  { { 0x0004AA, 2, { 0x20, 0x4B }, { 0x00, 0x00 } },
	    { 0x0037C2, 1, { 0xC0 }, { 0x00 } } } },
	// nop_skip_branch
	{ 0x8B00, "PRO FISHINGAFCP\200", "EuroSport Pro Champ Fishing (Europe) (Proto)", 2,
	  { { 0x0002DE, 2, { 0x20, 0x07 }, { 0x00, 0x00 } },
	    { 0x0002E2, 2, { 0x20, 0x03 }, { 0x00, 0x00 } } } },
	// nop_early_ret+imm_pre_fork
	{ 0x8DB6, "FAIRY   KITTY", "Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan) (Rev 1)", 2,
	  { { 0x0380FD, 1, { 0xC8 }, { 0x00 } },
	    { 0x038103, 2, { 0x3E, 0xFF }, { 0x3E, 0x0A } } } },
	// nop_early_ret+imm_pre_fork
	{ 0xA958, "FAIRY   KITTY", "Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan)", 2,
	  { { 0x0380FD, 1, { 0xC8 }, { 0x00 } },
	    { 0x038103, 2, { 0x3E, 0xFF }, { 0x3E, 0x0A } } } },
	// force_jr+nop_store
	{ 0x1689, "FIFA 2000", "FIFA 2000 (USA, Europe)", 2,
	  { { 0x000349, 2, { 0x20, 0x27 }, { 0x18, 0x27 } },
	    { 0x000384, 2, { 0xE0, 0x9E }, { 0x00, 0x00 } } } },
	// nop_skip_branch+nop_store
	{ 0x7652, "HAMSTAR PRADISE\200", "Hamster Paradise (Japan)", 3,
	  { { 0x0015CD, 2, { 0x28, 0x13 }, { 0x00, 0x00 } },
	    { 0x0015D3, 2, { 0x28, 0x0D }, { 0x00, 0x00 } },
	    { 0x0015DF, 2, { 0xE0, 0x80 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x8980, "TENTEN", "Hanasaka Tenshi Tenten-kun no Beat Breaker (Japan)", 1,
	  { { 0x0001A9, 3, { 0xCA, 0x8D, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// force_jr
	{ 0xE5F7, "H-MOON2 CGBBM2P\200", "Harvest Moon 2 GBC (Europe)", 1,
	  { { 0x0001B1, 2, { 0x28, 0x39 }, { 0x18, 0x39 } } } },
	// force_jr
	{ 0xB3DC, "H-MOON2 CGBBM2D\200", "Harvest Moon 2 GBC (Germany)", 1,
	  { { 0x0001B1, 2, { 0x28, 0x39 }, { 0x18, 0x39 } } } },
	// force_jr
	{ 0x271C, "H-MOON2 CGBBM2E\200", "Harvest Moon 2 GBC (USA)", 1,
	  { { 0x0001B1, 2, { 0x28, 0x39 }, { 0x18, 0x39 } } } },
	// nop_skip_branch
	{ 0x6920, "MAHJONGOH", "Honkaku Yonin Uchi Mahjong - Mahjong Ou (Japan)", 1,
	  { { 0x0003CD, 3, { 0xC2, 0xD8, 0x03 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xB3CA, "TRACK&FIELDAHDJ\200", "Hyper Olympic Series - Track & Field GB (Japan)", 1,
	  { { 0x0001C8, 3, { 0xCA, 0xD1, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch+imm_pre_fork
	{ 0xF4B2, "INTER RALLYARLE\200", "International Rally (USA)", 2,
	  { { 0x0001D5, 3, { 0xCA, 0xE3, 0x01 }, { 0x00, 0x00, 0x00 } },
	    { 0x0012EC, 2, { 0x3E, 0x01 }, { 0x3E, 0x03 } } } },
	// nop_skip_branch
	{ 0x9897, "TRACK&FIELDAHDP\200", "International Track & Field (Europe) (En,Fr,De,It)", 1,
	  { { 0x0001C8, 3, { 0xCA, 0xD1, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x9A3A, "TRACK&FIELDAHDE\200", "International Track & Field (USA)", 1,
	  { { 0x0001C8, 3, { 0xCA, 0xD1, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xB9F1, "ITS W_RALLYARLJ\200", "It's a World Rally (Japan)", 1,
	  { { 0x0001D2, 3, { 0xCA, 0xDD, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// retarget_jr_to_sgb_entry
	{ 0x7C1B, "JINSEI TOMOACJJ\200", "Jinsei Game - Tomodachi Takusan Tsukurou yo! (Japan)", 1,
	  { { 0x002F0E, 2, { 0x18, 0x4A }, { 0x18, 0x00 } } } },
	// manual: flag routine answers SGB2
	{ 0x5650, "MUSCLERANK2B6KJ\200", "Kinniku Banzuke GB 2 - Mezase! Muscle Champion (Japan)", 1,
	  { { 0x00025B, 2, { 0xCB, 0x4E }, { 0xAF, 0x00 } } } },
	// nop_skip_branch
	{ 0x2DB1, "KOUSHIEN POCKET\200", "Koushien Pocket (Japan)", 1,
	  { { 0x00019E, 3, { 0xCA, 0x79, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_early_ret
	{ 0x7789, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Europe) (Rev 2) (Beta) (1999-09-19)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x5E40, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-04)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x6046, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-22)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x9836, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x1377, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x2D7E, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Beta) (1998-12-11)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x89E8, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-04)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x8BEE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-22)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x8ACE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x2D09, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x2735, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 1)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x0000, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-08-19)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x7507, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-04)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x7689, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-07)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x788F, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-23)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x0135, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xE3FD, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// force_jr
	{ 0x259C, "LODOSS WAR GB", "Lodoss-tou Senki - Eiyuu Kishiden GB (Japan)", 1,
	  { { 0x00019B, 2, { 0x28, 0x39 }, { 0x18, 0x39 } } } },
	// nop_skip_branch+nop_store
	{ 0xD6BB, "NUMBERPLACEARCJ\200", "Luca no Puzzle de Daibouken! (Japan)", 3,
	  { { 0x000F6D, 2, { 0x28, 0x13 }, { 0x00, 0x00 } },
	    { 0x000F73, 2, { 0x28, 0x0D }, { 0x00, 0x00 } },
	    { 0x000F7F, 2, { 0xE0, 0x80 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xBE50, "MAHJONGJOOHA56J\200", "Mahjong Joou (Japan)", 2,
	  { { 0x001097, 1, { 0xC8 }, { 0x00 } },
	    { 0x000BB9, 3, { 0xC2, 0xD3, 0x0B }, { 0x00, 0x00, 0x00 } } } },
	// force_jr
	{ 0xD1EF, "MEN IN BLACK", "Men in Black - The Series (USA, Europe)", 1,
	  { { 0x00026B, 2, { 0x20, 0x4C }, { 0x18, 0x4C } } } },
	// force_jr
	{ 0xA0C5, "NHL 2000", "NHL 2000 (USA, Europe)", 1,
	  { { 0x00021F, 2, { 0x20, 0x4C }, { 0x18, 0x4C } } } },
	// force_jr
	{ 0x5C51, "OHASTA Y&R", "Ohasuta Yama-chan & Raymond (Japan)", 1,
	  { { 0x010450, 2, { 0x20, 0x09 }, { 0x18, 0x09 } } } },
	// nop_skip_branch
	{ 0x99CA, "PHANTOMZONABKZJ\200", "Phantom Zona (Japan)", 1,
	  { { 0x000180, 2, { 0x28, 0x17 }, { 0x00, 0x00 } } } },
	// force_jr
	{ 0x41CB, "POKEBOM USA", "Pocket Bomberman (USA, Europe)", 1,
	  { { 0x00024B, 2, { 0x20, 0x24 }, { 0x18, 0x24 } } } },
	// nop_early_ret
	{ 0x1D34, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan) (Rev 1)", 1,
	  { { 0x009C59, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x7691, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan)", 1,
	  { { 0x009C59, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x8460, "POKEMON_GLDAAUJ\200", "Pocket Monsters Kin (Japan) (Rev 1)", 1,
	  { { 0x009C59, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x9353, "POKEMON_GLDAAUS\200", "Pokemon - Edicion Oro (Spain)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x064B, "POKEMON_SLVAAXS\200", "Pokemon - Edicion Plata (Spain)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x682D, "POKEMON_GLDAAUE\200", "Pokemon - Gold Version (USA, Europe)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xDC97, "POKEMON_GLDAAUD\200", "Pokemon - Goldene Edition (Germany)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x6A0E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany) (Beta)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xCD6E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x0DAE, "POKEMON_SLVAAXE\200", "Pokemon - Silver Version (USA, Europe)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xFB8C, "POKEMON_SLVAAXF\200", "Pokemon - Version Argent (France)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x6FC6, "POKEMON_GLDAAUF\200", "Pokemon - Version Or (France)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x7350, "POKEMON_SLVAAXI\200", "Pokemon - Versione Argento (Italy)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xCE0C, "POKEMON_GLDAAUI\200", "Pokemon - Versione Oro (Italy)", 1,
	  { { 0x009CC3, 1, { 0xC0 }, { 0x00 } } } },
	// nop_skip_branch+imm_pre_fork
	{ 0xD526, "POKEMON CARD GB\200", "Pokemon Card GB (Japan)", 2,
	  { { 0x000337, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000343, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// nop_early_ret
	{ 0x365F, "POKEMON_GOLD_US\200", "Pokemon Gold (Taiwan) (En) (Unl)", 1,
	  { { 0x009C59, 1, { 0xC0 }, { 0x00 } } } },
	// manual: detect, console stays CGB
	{ 0xB440, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Es,It) (Rev 1)", 3,
	  { { 0x00033E, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000343, 2, { 0x06, 0x00 }, { 0x06, 0x02 } },
	    { 0x00034A, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// manual: detect, console stays CGB
	{ 0xC869, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Es,It)", 3,
	  { { 0x00033E, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000343, 2, { 0x06, 0x00 }, { 0x06, 0x02 } },
	    { 0x00034A, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// manual: detect, console stays CGB
	{ 0x49CD, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Fr,De) (Rev 1)", 3,
	  { { 0x00033E, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000343, 2, { 0x06, 0x00 }, { 0x06, 0x02 } },
	    { 0x00034A, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// manual: detect, console stays CGB
	{ 0x5E22, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Fr,De)", 3,
	  { { 0x00033E, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000343, 2, { 0x06, 0x00 }, { 0x06, 0x02 } },
	    { 0x00034A, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// nop_skip_branch+imm_pre_fork
	{ 0x26A6, "POKECARD", "Pokemon Trading Card Game (USA, Australia)", 2,
	  { { 0x00034D, 2, { 0x28, 0x0C }, { 0x00, 0x00 } },
	    { 0x000359, 2, { 0x06, 0x01 }, { 0x06, 0x02 } } } },
	// nop_skip_branch
	{ 0x48E6, "PUCHI CARATAIQP\200", "Puchi Carat (Europe)", 1,
	  { { 0x00404D, 3, { 0xCA, 0x23, 0x41 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xF6BD, "PUCHI CARATACUJ\200", "Puchi Carat (Japan)", 1,
	  { { 0x00404D, 3, { 0xCA, 0x23, 0x41 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x7926, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 1)", 1,
	  { { 0x00404D, 3, { 0xCA, 0x23, 0x41 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x1CF8, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 2)", 1,
	  { { 0x00404D, 3, { 0xCA, 0x23, 0x41 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xDCBF, "QUARTET", "Quartet (World) (Aftermarket) (Unl)", 1,
	  { { 0x0044A1, 2, { 0x20, 0x28 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x9C19, "STAR", "Robopon - Star Version (USA) (Proto)", 1,
	  { { 0x063210, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xDA57, "SUN", "Robopon - Sun Version (USA)", 1,
	  { { 0x063210, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x4527, "ROBO BOM", "Robot Poncots - Comic Bom Bom Special Version (Japan)", 1,
	  { { 0x063145, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x9E0A, "ROBO MOON", "Robot Poncots - Moon Version (Japan)", 1,
	  { { 0x063145, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x0453, "ROBOPON STAR", "Robot Poncots - Star Version (Japan)", 1,
	  { { 0x063143, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xFD8C, "ROBOPON SUN", "Robot Poncots - Sun Version (Japan)", 1,
	  { { 0x063143, 2, { 0x28, 0x2A }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x43D3, "SENKAIIBUNRBHSJ\200", "Senkai Ibunroku Juntei Taisen - TV Animation Senkaiden Houshin Engi Yori (Japan)", 1,
	  { { 0x0001D7, 2, { 0x28, 0x0C }, { 0x00, 0x00 } } } },
	// manual: both flag tests off
	{ 0xC16E, "BASS POCKET 3", "Super Black Bass Pocket 3 (Japan)", 2,
	  { { 0x0002DE, 2, { 0x20, 0x07 }, { 0x00, 0x00 } },
	    { 0x0002E2, 2, { 0x20, 0x03 }, { 0x00, 0x00 } } } },
	// manual: flag routine answers SGB2
	{ 0xEEFE, "SYLVANIAN", "Sylvanian Families - Otogi no Kuni no Pendant (Japan)", 1,
	  { { 0x000256, 2, { 0xCB, 0x4E }, { 0xAF, 0x00 } } } },
	// nop_skip_branch
	{ 0x1C2F, "BUBBOB", "Taito Memorial - Bubble Bobble (Japan)", 1,
	  { { 0x0001A6, 3, { 0xCA, 0x8D, 0x02 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x46E9, "CHASE HQ", "Taito Memorial - Chase H.Q. - Secret Police (Japan)", 1,
	  { { 0x0001C0, 2, { 0x28, 0x12 }, { 0x00, 0x00 } } } },
	// manual: both flag tests off
	{ 0xDDE6, "TNN FISHINGAFCE\200", "TNN Outdoors Fishing Champ (USA)", 2,
	  { { 0x0002DE, 2, { 0x20, 0x07 }, { 0x00, 0x00 } },
	    { 0x0002E2, 2, { 0x20, 0x03 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x9524, "TURISENSEI2AF2J\200", "Tsuri Sensei 2 (Japan)", 1,
	  { { 0x000220, 2, { 0x28, 0x03 }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x968F, "CGBWARIOLAND2", "Wario Land 2 (Japan)", 1,
	  { { 0x00025D, 3, { 0xC2, 0x55, 0x03 }, { 0x00, 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0x24C7, "CGBWARIOLAND2", "Wario Land II (USA, Europe)", 1,
	  { { 0x00025D, 3, { 0xC2, 0x55, 0x03 }, { 0x00, 0x00, 0x00 } } } },
	// force_jr
	{ 0x439C, "WETRIX GB", "Wetrix GB (Japan)", 1,
	  { { 0x000BA6, 2, { 0x20, 0x03 }, { 0x18, 0x03 } } } },
	// nop_early_ret
	{ 0x9874, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Beta) (1998-11-09)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0xD553, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-08-04)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x3115, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-09-04)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x331B, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// nop_early_ret
	{ 0x9872, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan)", 1,
	  { { 0x0F2A25, 1, { 0xC0 }, { 0x00 } } } },
	// manual: no CGB return, reach SGB init
	{ 0x2874, "ZOIDS GENOBBGZJ\200", "Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan) (Rev 1)", 2,
	  { { 0x018010, 1, { 0xC8 }, { 0x00 } },
	    { 0x018017, 2, { 0x28, 0x01 }, { 0x18, 0x01 } } } },
	// manual: no CGB return, reach SGB init
	{ 0x3885, "ZOIDS GENOBBGZJ\200", "Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan)", 2,
	  { { 0x018010, 1, { 0xC8 }, { 0x00 } },
	    { 0x018017, 2, { 0x28, 0x01 }, { 0x18, 0x01 } } } },
};
// GENERATED-END

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

} // anonymous

bool ApplyIps(std::vector<uint8_t> &rom, const uint8_t *ips, size_t len)
{
	if (!ips || len < 8 || std::memcmp(ips, "PATCH", 5) != 0) return false;

	std::vector<uint8_t> out(rom);
	size_t i = 5;
	for (;;)
	{
		if (i + 3 == len && std::memcmp(ips + i, "EOF", 3) == 0) break;
		if (i + 5 > len) return false;
		const size_t off  = (static_cast<size_t>(ips[i]) << 16) |
		                    (static_cast<size_t>(ips[i + 1]) << 8) | ips[i + 2];
		const size_t size = (static_cast<size_t>(ips[i + 3]) << 8) | ips[i + 4];
		i += 5;
		if (size == 0)
		{
			// RLE record: 2-byte run length, then the byte.
			if (i + 3 > len) return false;
			const size_t run = (static_cast<size_t>(ips[i]) << 8) | ips[i + 1];
			if (out.size() < off + run) out.resize(off + run, 0);
			std::memset(out.data() + off, ips[i + 2], run);
			i += 3;
		}
		else
		{
			if (i + size > len) return false;
			if (out.size() < off + size) out.resize(off + size, 0);
			std::memcpy(out.data() + off, ips + i, size);
			i += size;
		}
	}
	rom.swap(out);
	return true;
}

bool PatchSgbcBios(std::vector<uint8_t> &bios)
{
	if (kSgbcBiosIpsSize == 0 || bios.size() != 0x80000) return false;
	unsigned char digest[32];
	sha256sum(bios.data(), static_cast<unsigned int>(bios.size()), digest);
	if (std::memcmp(digest, kSgbcBiosBaseSha256, sizeof digest) != 0) return false;
	return ApplyIps(bios, kSgbcBiosIps, kSgbcBiosIpsSize);
}

const SgbcPatch *FindSgbcPatch(const uint8_t *rom, size_t size)
{
	if (!rom || size < 0x150) return nullptr;
	const uint16_t gsum = static_cast<uint16_t>((rom[0x14E] << 8) | rom[0x14F]);
	for (const SgbcPatch &p : kPatches)
		if (p.global_sum == gsum && TitleMatches(rom, p.title)) return &p;
	return nullptr;
}

bool ApplySgbcPatch(const SgbcPatch &p, uint8_t *rom, size_t size)
{
	if (!rom) return false;
	const int n = p.edit_count < 3 ? p.edit_count : 3;
	for (int i = 0; i < n; ++i)
	{
		const SgbcEdit &e = p.edits[i];
		if (e.len > 3 || e.addr + e.len > size) return false;
		if (std::memcmp(rom + e.addr, e.old, e.len) != 0) return false;
	}
	for (int i = 0; i < n; ++i)
	{
		const SgbcEdit &e = p.edits[i];
		std::memcpy(rom + e.addr, e.neu, e.len);
	}
	return true;
}

} // namespace SGB
