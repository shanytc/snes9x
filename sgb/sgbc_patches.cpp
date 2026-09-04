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

// In-Memory patch applies to SGB2 (SHA-256) only.
const unsigned char kSgbcBiosBaseSha256[32] = {
	0xC1, 0x72, 0x49, 0x8A, 0x23, 0xD1, 0x17, 0x66, 0x72, 0x93, 0x1B, 0xAB, 0x33, 0xB6, 0x29, 0xC7,
	0xD2, 0x8F, 0x91, 0x4A, 0x43, 0xDC, 0xA9, 0xE5, 0x40, 0xB8, 0xAF, 0x1B, 0x37, 0xCC, 0xF2, 0xC6,
};

// In-Memory patch (original code) to enable SGBC support in an empty code cave
const unsigned char kSgbcBiosIps[] = {
	0x50, 0x41, 0x54, 0x43, 0x48, 0x00, 0x04, 0x8F, 0x00, 0x06, 0x22, 0xA7, 0xD9, 0x82, 0x80, 0xE6,
	0x00, 0x39, 0xBE, 0x00, 0x04, 0x5C, 0x15, 0xDA, 0x82, 0x00, 0x44, 0xBF, 0x00, 0x04, 0x5C, 0x4A,
	0xDA, 0x82, 0x00, 0x7F, 0xDC, 0x00, 0x04, 0xC9, 0xC1, 0x36, 0x3E, 0x01, 0x59, 0xA7, 0x01, 0x8B,
	0x08, 0x8B, 0xC2, 0x30, 0x48, 0xDA, 0x5A, 0xE2, 0x30, 0xD8, 0xA9, 0x7E, 0x48, 0xAB, 0xAD, 0x00,
	0xCE, 0xC9, 0x53, 0xD0, 0x07, 0xAD, 0x01, 0xCE, 0xC9, 0x43, 0xF0, 0x13, 0xA9, 0x53, 0x8D, 0x00,
	0xCE, 0xA9, 0x43, 0x8D, 0x01, 0xCE, 0x9C, 0x02, 0xCE, 0x9C, 0x03, 0xCE, 0x9C, 0x04, 0xCE, 0xAD,
	0x00, 0x01, 0xC9, 0x0F, 0xF0, 0x05, 0x9C, 0x02, 0xCE, 0x80, 0x2B, 0xAD, 0x02, 0xCE, 0xC9, 0x01,
	0xF0, 0x21, 0xC9, 0x02, 0xF0, 0x20, 0xAD, 0x01, 0x01, 0xC9, 0x04, 0x90, 0x19, 0xAD, 0x52, 0x06,
	0xC9, 0x43, 0xF0, 0x07, 0xA9, 0x02, 0x8D, 0x02, 0xCE, 0x80, 0x0B, 0xA9, 0x01, 0x8D, 0x02, 0xCE,
	0x9C, 0x04, 0xCE, 0x20, 0x7B, 0xDA, 0xC2, 0x30, 0x7A, 0xFA, 0x68, 0xAB, 0x28, 0x6B, 0x08, 0x8B,
	0xC2, 0x30, 0x48, 0xDA, 0x5A, 0xE2, 0x30, 0xD8, 0xA9, 0x7E, 0x48, 0xAB, 0xAD, 0x94, 0x02, 0xCD,
	0x03, 0xCE, 0xF0, 0x11, 0x8D, 0x03, 0xCE, 0xC9, 0x00, 0xD0, 0x0A, 0xAD, 0x00, 0xCE, 0xC9, 0x53,
	0xD0, 0x03, 0x20, 0x7B, 0xDA, 0xC2, 0x30, 0x7A, 0xFA, 0x68, 0xAB, 0x28, 0xAD, 0x00, 0x60, 0x5C,
	0xC2, 0xB9, 0x80, 0x78, 0x08, 0x8B, 0xC2, 0x30, 0x48, 0xDA, 0x5A, 0xE2, 0x30, 0xD8, 0xA9, 0x7E,
	0x48, 0xAB, 0xAD, 0x00, 0xCE, 0xC9, 0x53, 0xD0, 0x0D, 0xAD, 0x02, 0xCE, 0xC9, 0x01, 0xD0, 0x06,
	0x20, 0xD4, 0xDA, 0x20, 0xAE, 0xDA, 0xC2, 0x30, 0x7A, 0xFA, 0x68, 0xAB, 0x28, 0xAD, 0x10, 0x42,
	0x5C, 0xC3, 0xC4, 0x80, 0xAD, 0x00, 0x01, 0xC9, 0x0F, 0xD0, 0x2B, 0xAD, 0x01, 0x01, 0xC9, 0x04,
	0x90, 0x24, 0xAD, 0x02, 0xCE, 0xC9, 0x01, 0xD0, 0x1D, 0x20, 0xFF, 0xDA, 0xB0, 0x18, 0x20, 0xD4,
	0xDA, 0x20, 0xAE, 0xDA, 0xA9, 0x01, 0x8D, 0x10, 0x02, 0x8D, 0x17, 0x02, 0xAD, 0x04, 0xCE, 0xC9,
	0xFF, 0xF0, 0x03, 0xEE, 0x04, 0xCE, 0x60, 0xA0, 0x02, 0xA2, 0x00, 0xBF, 0x2C, 0xDB, 0x82, 0x99,
	0x00, 0x04, 0xE8, 0xC8, 0xE0, 0x06, 0xD0, 0xF3, 0xC8, 0xC8, 0xC0, 0x22, 0xD0, 0xEB, 0xA2, 0x00,
	0xBF, 0x2C, 0xDB, 0x82, 0x9D, 0x22, 0xC2, 0xE8, 0xE0, 0x06, 0xD0, 0xF4, 0x60, 0xAD, 0x02, 0x04,
	0xC9, 0x1F, 0xD0, 0x07, 0xAD, 0x03, 0x04, 0xC9, 0x7C, 0xF0, 0x1C, 0xA0, 0x02, 0xA2, 0x00, 0xA9,
	0x06, 0x8D, 0x05, 0xCE, 0xB9, 0x00, 0x04, 0x9D, 0x08, 0xCE, 0xE8, 0xC8, 0xCE, 0x05, 0xCE, 0xD0,
	0xF3, 0xC8, 0xC8, 0xE0, 0x18, 0xD0, 0xE8, 0x60, 0xA0, 0x02, 0xA2, 0x00, 0xBF, 0x2C, 0xDB, 0x82,
	0xD9, 0x00, 0x04, 0xD0, 0x1E, 0xE8, 0xC8, 0xE0, 0x06, 0xD0, 0xF1, 0xC8, 0xC8, 0xC0, 0x22, 0xD0,
	0xE9, 0xA2, 0x00, 0xBF, 0x2C, 0xDB, 0x82, 0xDD, 0x22, 0xC2, 0xD0, 0x07, 0xE8, 0xE0, 0x06, 0xD0,
	0xF2, 0x38, 0x60, 0x18, 0x60, 0x1F, 0x7C, 0x1F, 0x7D, 0x1F, 0x7E, 0x45, 0x4F, 0x46,
};

const size_t kSgbcBiosIpsSize = 446;

// In-Memory patch to support SGBc in games that don't check for it
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
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x04D6, "BIKKURIMAN", "Bikkuriman 2000 - Charging Card GB (Japan)", 2,
	  { { 0x001A57, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x001AB2, 1, { 0x3C }, { 0x3D } } } },
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
	// manual: the Color branch skips the SGB block; enter it where they rejoin
	{ 0xB84C, "BUGS LIFE", "Bug's Life, A (Europe)", 1,
	  { { 0x0002AF, 3, { 0x18, 0x65, 0xCD }, { 0xC3, 0xF9, 0x02 } } } },
	// manual: the Color branch skips the SGB block; enter it where they rejoin
	{ 0xB74C, "BUGS LIFE", "Bug's Life, A (USA)", 1,
	  { { 0x0002AF, 3, { 0x18, 0x65, 0xCD }, { 0xC3, 0xF9, 0x02 } } } },
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
	// manual: the boot far-call's jump-table entry rerouted through the dead
	// DMG title-scene stub, so the CGB palette preload still runs ahead of the
	// border transfers (retargeting the call itself dropped the preload).
	{ 0x7EB9, "CONKER CGB", "Conker's Pocket Tales (USA, Europe) (En,Fr,De)", 3,
	  { { 0x0005C8, 2, { 0x46, 0x4B }, { 0x76, 0x49 } },
	    { 0x0005D0, 3, { 0xA2, 0x40, 0xCD }, { 0xD2, 0x41, 0xC9 } },
	    { 0x1FBFE6, 2, { 0x76, 0x49 }, { 0xC7, 0x05 } } } },
	// nop_skip_branch
	{ 0x7961, "C C RACING ARLP\200", "Cross Country Racing (Europe) (En,Fr,De)", 1,
	  { { 0x0001D2, 3, { 0xCA, 0xE0, 0x01 }, { 0x00, 0x00, 0x00 } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0xAA02, "DN SWEETADVBSAJ\200", "Dear Daniel no Sweet Adventure - Kitty-chan o Sagashite (Japan)", 2,
	  { { 0x00135F, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x0013BE, 1, { 0x3C }, { 0x3D } } } },
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
	// manual: an SGB detect suppresses the Color flag; stop it
	{ 0x33C8, "DRAGONYHM", "Dragonyhm (World) (v1.0.0) (Demo) (Aftermarket) (Unl)", 1,
	  { { 0x188367, 2, { 0x20, 0x0D }, { 0x00, 0x00 } } } },
	// nop_skip_branch
	{ 0xA73A, "DT GAMEBOY BBDJ\200", "DT - Lords of Genomes (Japan)", 1,
	  { { 0x0001B4, 2, { 0x28, 0x06 }, { 0x00, 0x00 } } } },
	// nop_early_ret
	{ 0xFC46, "MONOPOLYGB", "DX Monopoly GB (Japan)", 2,
	  { { 0x0004AA, 2, { 0x20, 0x4B }, { 0x00, 0x00 } },
	    { 0x0037C2, 1, { 0xC0 }, { 0x00 } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x0A05, "ATELIERELIEA8EJ\200", "Elie no Atelier GB (Japan)", 2,
	  { { 0x000EB0, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x000F0F, 1, { 0x3C }, { 0x3D } } } },
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
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0x6517, "G&W GALLERY2", "Game & Watch Gallery 2 (USA, Europe)", 3,
	  { { 0x000C11, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000C5E, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000C6E, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x74, 0x0C } } } },
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0x8133, "G&WGALLERY3AGQE\200", "Game & Watch Gallery 3 (USA, Europe)", 3,
	  { { 0x000DC5, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000E07, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000E17, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x1D, 0x0E } } } },
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0x6EB3, "GB GALLERY2", "Game Boy Gallery 2 (Japan) (Possible Proto) (NP)", 3,
	  { { 0x000C11, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000C5E, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000C6E, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x74, 0x0C } } } },
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0xDACE, "GB GALLERY3", "Game Boy Gallery 3 (Australia)", 3,
	  { { 0x000C11, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000C5E, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000C6E, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x74, 0x0C } } } },
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0x3192, "GB GALLERY3AGQJ\200", "Game Boy Gallery 3 (Japan)", 3,
	  { { 0x000DC6, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000E08, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000E18, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x1E, 0x0E } } } },
	// manual: run the Color init, then fall into the border routine past its gate
	{ 0x2458, "GB GALLERY4AGQU\200", "Game Boy Gallery 4 (Australia)", 3,
	  { { 0x000DC5, 3, { 0xEA, 0x00, 0xC7 }, { 0x00, 0x00, 0x00 } },
	    { 0x000E07, 3, { 0x3D, 0x20, 0x0E }, { 0x00, 0x00, 0x00 } },
	    { 0x000E17, 3, { 0xC9, 0xFA, 0x00 }, { 0xC3, 0x1D, 0x0E } } } },
	// manual: the SGB branch maps the console type; give it the Color value
	{ 0x3E6E, "GLOCAL HEXCITE", "Glocal Hexcite (Japan)", 1,
	  { { 0x00071E, 1, { 0x02 }, { 0x04 } } } },
	// manual: the SGB path hangs on LY with the LCD off; take the Color one
	{ 0x2E53, "T-GREAT-B-PAGVJ\200", "Great Battle Pocket, The (Japan)", 1,
	  { { 0x0004D9, 1, { 0x03 }, { 0x00 } } } },
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
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x4DCF, "KTBZFACTORYAHBJ\200", "Hello Kitty no Beads Factory (Japan)", 3,
	  { { 0x0027FE, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x00285D, 1, { 0x3C }, { 0x3D } },
	    { 0x00295A, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0xC4BD, "KT SWEETADVBKTJ\200", "Hello Kitty no Sweet Adventure - Daniel-kun ni Aitai (Japan)", 2,
	  { { 0x00135F, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x0013BE, 1, { 0x3C }, { 0x3D } } } },
	// manual: the SGB branch maps the console type; give it the Color value
	{ 0x0C33, "HEXCITE", "Hexcite (Europe) (En,Fr,De) (Proto)", 1,
	  { { 0x0006A7, 1, { 0x03 }, { 0x04 } } } },
	// manual: the SGB branch maps the console type; give it the Color value
	{ 0xB919, "HEXCITE", "Hexcite - The Shapes of Victory (USA, Europe)", 1,
	  { { 0x00069B, 1, { 0x03 }, { 0x04 } } } },
	// manual: console type 1 (CGB) runs the SGB init too
	{ 0x87FD, "HOLY MAGIC     \200", "Holy Magic Century (Europe) (En,Fr,De)", 1,
	  { { 0x0001B9, 1, { 0x02 }, { 0x01 } } } },
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
	// manual: CGB type runs the SGB init, palette gates take $11
	{ 0x20D7, "TUWAMONO   A55J\200", "Joryuu Janshi ni Chousen GB - Watashi-tachi ni Chousen Shitene! (Japan)", 3,
	  { { 0x0001FC, 2, { 0x20, 0x03 }, { 0x00, 0x00 } },
	    { 0x0014CE, 1, { 0x01 }, { 0x11 } },
	    { 0x001599, 1, { 0x01 }, { 0x11 } } } },
	// manual: the SGB flag gates the Color path; clear it, retune the border gate
	{ 0x32BD, "MONSTER KANAQPJ\200", "Kanzume Monsters Parfait (Japan)", 2,
	  { { 0x0003EC, 1, { 0x03 }, { 0x00 } },
	    { 0x00047F, 1, { 0x03 }, { 0x00 } } } },
	// manual: CGB boot through the SGB init block
	{ 0xC45B, "KARAMUCHO OBOOJ\200", "Karamuchou wa Oosawagi! - Okawari! (Japan) (SGB Enhanced, GB Compatible) (NP)", 3,
	  { { 0x000172, 2, { 0x28, 0x1D }, { 0x28, 0x08 } },
	    { 0x00018F, 2, { 0x18, 0x1E }, { 0x18, 0x00 } },
	    { 0x000191, 2, { 0x3E, 0x04 }, { 0x3E, 0x85 } } } },
	// manual: CGB boot through the SGB init block
	{ 0xF8D0, "KARAMUCHO SAWAG\200", "Karamuchou wa Oosawagi! - Polinkies to Okashina Nakama-tachi (Japan)", 3,
	  { { 0x000172, 2, { 0x28, 0x1D }, { 0x28, 0x08 } },
	    { 0x00018F, 2, { 0x18, 0x1E }, { 0x18, 0x00 } },
	    { 0x000191, 2, { 0x3E, 0x04 }, { 0x3E, 0x85 } } } },
	// manual: an SGB flag bit gates the Color path; clear it, ungate the sender
	{ 0x1789, "BEAST WARS", "Kettou Transformers Beast Wars - Beast Senshi Saikyou Ketteisen (Japan)", 2,
	  { { 0x0001DE, 2, { 0xCB, 0xFE }, { 0xCB, 0xBE } },
	    { 0x00020A, 1, { 0xC8 }, { 0x00 } } } },
	// manual: an SGB flag bit gates the Color path; clear it, ungate the sender
	{ 0x8005, "KINDAICHI", "Kindaichi Shounen no Jikenbo - 10 Nenme no Shoutaijou (Japan)", 2,
	  { { 0x0001DE, 2, { 0xCB, 0xFE }, { 0xCB, 0xBE } },
	    { 0x00020A, 1, { 0xC8 }, { 0x00 } } } },
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
	// manual: Color init falls into the SGB detect
	{ 0x3B5C, "MADDEN 2000", "Madden NFL 2000 (USA, Europe) (Beta)", 1,
	  { { 0x00018F, 3, { 0xC3, 0xC4, 0x01 }, { 0xC3, 0xAD, 0x01 } } } },
	// manual: Color init falls into the SGB detect
	{ 0xF678, "MADDEN 2000AEME\200", "Madden NFL 2000 (USA, Europe)", 1,
	  { { 0x00018F, 3, { 0xC3, 0xC4, 0x01 }, { 0xC3, 0xAD, 0x01 } } } },
	// nop_skip_branch
	{ 0xBE50, "MAHJONGJOOHA56J\200", "Mahjong Joou (Japan)", 2,
	  { { 0x001097, 1, { 0xC8 }, { 0x00 } },
	    { 0x000BB9, 3, { 0xC2, 0xD3, 0x0B }, { 0x00, 0x00, 0x00 } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0xE932, "ATELIERMARIA8MJ\200", "Marie no Atelier GB (Japan)", 2,
	  { { 0x000EB0, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x000F0F, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x9837, "MEDACARDKBTA8CJ\200", "Medarot Cardrobottle - Kabuto Version (Japan)", 3,
	  { { 0x0012E1, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x001340, 1, { 0x3C }, { 0x3D } },
	    { 0x00144D, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x980B, "MEDACARDKWGA9CJ\200", "Medarot Cardrobottle - Kuwagata Version (Japan)", 3,
	  { { 0x0012E1, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x001340, 1, { 0x3C }, { 0x3D } },
	    { 0x00144D, 1, { 0x3C }, { 0x3D } } } },
	// manual: the Color branch skips the SGB block; enter it where they rejoin
	{ 0xD1EF, "MEN IN BLACK", "Men in Black - The Series (USA, Europe)", 1,
	  { { 0x0002B7, 3, { 0x18, 0x67, 0xCD }, { 0xC3, 0x03, 0x03 } } } },
	// force_jr
	{ 0xA0C5, "NHL 2000", "NHL 2000 (USA, Europe)", 1,
	  { { 0x00021F, 2, { 0x20, 0x4C }, { 0x18, 0x4C } } } },
	// force_jr
	{ 0x5C51, "OHASTA Y&R", "Ohasuta Yama-chan & Raymond (Japan)", 1,
	  { { 0x010450, 2, { 0x20, 0x09 }, { 0x18, 0x09 } } } },
	// manual: CGB boot through the SGB init block
	{ 0xD7CB, "PACHISUROU", "Pachipachi Pachisurou - New Pulsar Hen (Japan)", 3,
	  { { 0x000172, 2, { 0x28, 0x1D }, { 0x28, 0x08 } },
	    { 0x00018F, 2, { 0x18, 0x1E }, { 0x18, 0x00 } },
	    { 0x000191, 2, { 0x3E, 0x04 }, { 0x3E, 0x85 } } } },
	// nop_skip_branch
	{ 0x99CA, "PHANTOMZONABKZJ\200", "Phantom Zona (Japan)", 1,
	  { { 0x000180, 2, { 0x28, 0x17 }, { 0x00, 0x00 } } } },
	// manual: the Color branch skips the SGB block; enter it where they rejoin,
	// keep the Color type, and take the two border gates that test the SGB one
	{ 0x41CB, "POKEBOM USA", "Pocket Bomberman (USA, Europe)", 4,
	  { { 0x00026F, 3, { 0x18, 0x61, 0xCD }, { 0xC3, 0x82, 0x02 } },
	    { 0x00029B, 1, { 0x01 }, { 0x02 } },
	    { 0x00317F, 1, { 0x01 }, { 0x02 } },
	    { 0x003661, 1, { 0x01 }, { 0x02 } } } },
	// manual: run the Color init after the SGB block
	{ 0xEFB7, "POCKET DENSYA2", "Pocket Densha 2 (Japan)", 1,
	  { { 0x0002DC, 3, { 0xC3, 0x54, 0x03 }, { 0xC3, 0xF7, 0x01 } } } },
	// manual: detect, console stays CGB
	{ 0xF8B7, "POCKET KINGAV5J\200", "Pocket King (Japan)", 2,
	  { { 0x0001C1, 2, { 0x20, 0x04 }, { 0x18, 0x04 } },
	    { 0x0001D5, 2, { 0x3E, 0x01 }, { 0x3E, 0x02 } } } },
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
	// manual: CGB flag kept, SGB detect taken
	{ 0x1A21, "POKEMONPINBVPHP\200", "Pokemon Pinball (Europe) (En,Fr,De,Es,It) (Rumble Version)", 1,
	  { { 0x00020A, 2, { 0x20, 0x0C }, { 0x00, 0x00 } } } },
	// manual: CGB flag kept, SGB detect taken
	{ 0xD7B0, "POKEPINBALLVPHJ\200", "Pokemon Pinball (Japan) (Rumble Version)", 1,
	  { { 0x00020A, 2, { 0x20, 0x16 }, { 0x00, 0x00 } } } },
	// manual: CGB flag kept, SGB detect taken
	{ 0xDA60, "POKEPINBALLVPHE\200", "Pokemon Pinball (USA, Australia) (Rumble Version)", 1,
	  { { 0x00020A, 2, { 0x20, 0x16 }, { 0x00, 0x00 } } } },
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
	// manual: SGB detect called ahead of the CGB init, type 5 (CGB|SGB1)
	{ 0xB727, "PP SUCSESS", "Power Pro Kun Pocket (Japan) (Rev 1)", 2,
	  { { 0x05AB0E, 1, { 0xC2 }, { 0xCD } },
	    { 0x05AB1A, 1, { 0x04 }, { 0x05 } } } },
	// manual: SGB detect called ahead of the CGB init, type 5 (CGB|SGB1)
	{ 0xC4E9, "PP SUCSESS", "Power Pro Kun Pocket (Japan)", 2,
	  { { 0x05AB0E, 1, { 0xC2 }, { 0xCD } },
	    { 0x05AB1A, 1, { 0x04 }, { 0x05 } } } },
	// manual: SGB detect called ahead of the CGB init, type 5 (CGB|SGB1)
	{ 0xA977, "PAWA POKE2", "Power Pro Kun Pocket 2 (Japan)", 2,
	  { { 0x05A6CE, 1, { 0xC2 }, { 0xCD } },
	    { 0x05A6DA, 1, { 0x04 }, { 0x05 } } } },
	// manual: CGB type runs the SGB init, palette gates take $11
	{ 0x19D6, "TUWAMONO   AQDJ\200", "Pro Mahjong Tsuwamono GB (Japan)", 3,
	  { { 0x0001FF, 2, { 0x20, 0x03 }, { 0x00, 0x00 } },
	    { 0x0011B1, 1, { 0x01 }, { 0x11 } },
	    { 0x001242, 1, { 0x01 }, { 0x11 } } } },
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
	// manual: console type 1 (CGB) runs the SGB init too
	{ 0x1AC7, "QUEST FANTASY  \200", "Quest - Fantasy Challenge (USA)", 1,
	  { { 0x0001B9, 1, { 0x02 }, { 0x01 } } } },
	// manual: SGB detect on the CGB path, $11 passes the border gate, BGP=0 write turned into LCDC=$FF
	{ 0x9C19, "STAR", "Robopon - Star Version (USA) (Proto)", 3,
	  { { 0x063210, 2, { 0x28, 0x2A }, { 0x00, 0x00 } },
	    { 0x0FE26B, 3, { 0xC8, 0xFE, 0x01 }, { 0x00, 0xFE, 0x11 } },
	    { 0x0FE312, 3, { 0xAF, 0xE0, 0x47 }, { 0x3D, 0xE0, 0x40 } } } },
	// manual: SGB detect on the CGB path, $11 passes the border gate, BGP=0 write turned into LCDC=$FF
	{ 0xDA57, "SUN", "Robopon - Sun Version (USA)", 3,
	  { { 0x063210, 2, { 0x28, 0x2A }, { 0x00, 0x00 } },
	    { 0x0FE26D, 3, { 0xC8, 0xFE, 0x01 }, { 0x00, 0xFE, 0x11 } },
	    { 0x0FE314, 3, { 0xAF, 0xE0, 0x47 }, { 0x3D, 0xE0, 0x40 } } } },
	// manual: SGB detect on the CGB path, $11 passes the border gate, BGP=0 write turned into LCDC=$FF
	{ 0x5D30, "SUN", "Robopon - Sun Version (USA) (Beta)", 3,
	  { { 0x063205, 2, { 0x28, 0x2A }, { 0x00, 0x00 } },
	    { 0x0FE27B, 3, { 0xC8, 0xFE, 0x01 }, { 0x00, 0xFE, 0x11 } },
	    { 0x0FE322, 3, { 0xAF, 0xE0, 0x47 }, { 0x3D, 0xE0, 0x40 } } } },
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
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0xE12A, "TIMENETPAST", "Sanrio Timenet - Kako Hen (Japan)", 2,
	  { { 0x010004, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x010063, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0xF0B1, "TIMENETPAST", "Sanrio Timenet - Kako Hen (Japan) (Rev 1)", 2,
	  { { 0x010004, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x010063, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x3135, "TIMENETFUTURE", "Sanrio Timenet - Mirai Hen (Japan)", 2,
	  { { 0x010004, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x010063, 1, { 0x3C }, { 0x3D } } } },
	// manual: SGB detect wins over the Color branch; keep the type
	{ 0x3A4E, "TIMENETFUTURE", "Sanrio Timenet - Mirai Hen (Japan) (Rev 1)", 2,
	  { { 0x010004, 3, { 0xEA, 0xF0, 0xC1 }, { 0x00, 0x00, 0x00 } },
	    { 0x010063, 1, { 0x3C }, { 0x3D } } } },
	// manual: CGB type runs the SGB init, MASK_EN freeze, PAL_SET gate hopped
	{ 0x5089, "SD HIRYUEX", "SD Hiryuu no Ken EX (Japan)", 3,
	  { { 0x0023E9, 2, { 0x20, 0x03 }, { 0x00, 0x00 } },
	    { 0x00194E, 1, { 0x01 }, { 0x11 } },
	    { 0x0019DA, 3, { 0x02, 0xFA, 0x86 }, { 0x01, 0x18, 0x08 } } } },
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
	// manual: SGB2 detect, type CGB|SGB2
	{ 0x5ADB, "TOPNARIKIRIAN6J\200", "Tales of Phantasia - Narikiri Dungeon (Japan)", 2,
	  { { 0x004108, 2, { 0x28, 0x22 }, { 0x28, 0x0E } },
	    { 0x004128, 2, { 0x3E, 0x06 }, { 0x3E, 0x07 } } } },
	// manual: both flag tests off
	{ 0xDDE6, "TNN FISHINGAFCE\200", "TNN Outdoors Fishing Champ (USA)", 2,
	  { { 0x0002DE, 2, { 0x20, 0x07 }, { 0x00, 0x00 } },
	    { 0x0002E2, 2, { 0x20, 0x03 }, { 0x00, 0x00 } } } },
	// manual: an SGB detect suppresses the Color flag; stop it
	{ 0xCEC1, "TOOLSOFNEXAURA", "Tools of Nexaura (World) (v0.4) (Demo) (Aftermarket) (Unl)", 1,
	  { { 0x058367, 2, { 0x20, 0x0D }, { 0x00, 0x00 } } } },
	// manual: Color init falls into the SGB detect
	{ 0xA16C, "TOYSTORY 2", "Toy Story 2 (USA, Europe)", 1,
	  { { 0x000293, 2, { 0x18, 0x59 }, { 0x18, 0x3B } } } },
	// manual: bit-mask console type; add the Color bit to the SGB value
	{ 0xD855, "CARD HERO", "Trade & Battle Card Hero (Japan)", 1,
	  { { 0x0049F1, 1, { 0x82 }, { 0x86 } } } },
	// manual: bit-mask console type; add the Color bit to the SGB value
	{ 0x270B, "CARD HERO", "Trade & Battle Card Hero (Japan) (Rev 1) (3DS Virtual Console)", 1,
	  { { 0x0049F1, 1, { 0x82 }, { 0x86 } } } },
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

// ---- Super Game Boy compatibility edits ----------------------------------
// The cart's own SGB path stops under a BIOS the game never handled.
const SgbcPatch kSgbPatches[] = {
	// Boot A $FF (SGB2) survives the detect, and the first scene then loops
	// forever (01:$4052 `cp $FF / jr nz`): take the jump for every type.
	{ 0x20D7, "TUWAMONO   A55J\200", "SGB2 park skipped", 1,
	  { { 0x004057, 2, { 0x20, 0x05 }, { 0x18, 0x05 } } } },
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

static const SgbcPatch *FindIn(const SgbcPatch *table, size_t count, const uint8_t *rom, size_t size)
{
	if (!rom || size < 0x150) return nullptr;
	const uint16_t gsum = static_cast<uint16_t>((rom[0x14E] << 8) | rom[0x14F]);
	for (size_t i = 0; i < count; ++i)
		if (table[i].global_sum == gsum && TitleMatches(rom, table[i].title)) return &table[i];
	return nullptr;
}

const SgbcPatch *FindSgbcPatch(const uint8_t *rom, size_t size)
{
	return FindIn(kPatches, sizeof kPatches / sizeof kPatches[0], rom, size);
}

const SgbcPatch *FindSgbPatch(const uint8_t *rom, size_t size)
{
	return FindIn(kSgbPatches, sizeof kSgbPatches / sizeof kSgbPatches[0], rom, size);
}

bool ApplySgbcPatch(const SgbcPatch &p, uint8_t *rom, size_t size)
{
	if (!rom) return false;
	const int n = p.edit_count < 4 ? p.edit_count : 4;
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
