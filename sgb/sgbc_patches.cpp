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
// 36 in 1 (Taiwan) (SL36-0032) (Unl).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k000Old0[] = { 0xC0 };
const uint8_t k000New0[] = { 0x00 };
const SgbcEdit k000[] = { { 0x009C59, 1, k000Old0, k000New0 } };
// Animal Breeder 3 (Japan) (SGB Enhanced) (GB Compatible).zip  (manual: flag routine answers SGB2, ref=? fixed=1)
const uint8_t k001Old0[] = { 0xCB, 0x4E };
const uint8_t k001New0[] = { 0xAF, 0x00 };
const SgbcEdit k001[] = { { 0x000254, 2, k001Old0, k001New0 } };
// Balloon Fight GB (Japan) (SGB Enhanced, GB Compatible) (NP).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k002Old0[] = { 0xC8 };
const uint8_t k002New0[] = { 0x00 };
const SgbcEdit k002[] = { { 0x003B41, 1, k002Old0, k002New0 } };
// Barcode Taisen Bardigun (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=0 fixed=0)
const uint8_t k003Old0[] = { 0xC2, 0x2B, 0x02 };
const uint8_t k003New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k003[] = { { 0x000193, 3, k003Old0, k003New0 } };
// Barcode Taisen Bardigun (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=0 fixed=0)
const uint8_t k004Old0[] = { 0xC2, 0x1B, 0x02 };
const uint8_t k004New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k004[] = { { 0x000182, 3, k004Old0, k004New0 } };
// Blaster Master - Enemy Below (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=1 fixed=1)
const uint8_t k005Old0[] = { 0x18, 0x1D };
const uint8_t k005New0[] = { 0x18, 0x00 };
const SgbcEdit k005[] = { { 0x00016E, 2, k005Old0, k005New0 } };
// Bomberman Quest (Europe) (En,Fr,De) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k006Old0[] = { 0xCA, 0x72, 0x02 };
const uint8_t k006New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k006[] = { { 0x0001BE, 3, k006Old0, k006New0 } };
// Bomberman Quest (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k007Old0[] = { 0xCA, 0x77, 0x02 };
const uint8_t k007New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k007[] = { { 0x0001C3, 3, k007Old0, k007New0 } };
// Bomberman Quest (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k008Old0[] = { 0xCA, 0x77, 0x02 };
const uint8_t k008New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k008[] = { { 0x0001C3, 3, k008Old0, k008New0 } };
// Bug's Life, A (Europe) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k009Old0[] = { 0x20, 0x4A };
const uint8_t k009New0[] = { 0x18, 0x4A };
const SgbcEdit k009[] = { { 0x000265, 2, k009Old0, k009New0 } };
// Bug's Life, A (USA) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k010Old0[] = { 0x20, 0x4A };
const uint8_t k010New0[] = { 0x18, 0x4A };
const SgbcEdit k010[] = { { 0x000265, 2, k010Old0, k010New0 } };
// Chase H.Q. - Secret Police (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k011Old0[] = { 0x28, 0x12 };
const uint8_t k011New0[] = { 0x00, 0x00 };
const SgbcEdit k011[] = { { 0x0001C0, 2, k011Old0, k011New0 } };
// Chase H.Q. - Secret Police (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k012Old0[] = { 0x28, 0x12 };
const uint8_t k012New0[] = { 0x00, 0x00 };
const SgbcEdit k012[] = { { 0x0001C0, 2, k012Old0, k012New0 } };
// Choro Q - Hyper Customable GB (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k013Old0[] = { 0x28, 0x2D };
const uint8_t k013New0[] = { 0x00, 0x00 };
const SgbcEdit k013[] = { { 0x003171, 2, k013Old0, k013New0 } };
// Classic Bubble Bobble (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k014Old0[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k014New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k014[] = { { 0x0001A6, 3, k014Old0, k014New0 } };
// Classic Bubble Bobble (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k015Old0[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k015New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k015[] = { { 0x0001A6, 3, k015Old0, k015New0 } };
// Cross Country Racing (Europe) (En,Fr,De) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k016Old0[] = { 0xCA, 0xE0, 0x01 };
const uint8_t k016New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k016[] = { { 0x0001D2, 3, k016Old0, k016New0 } };
// Dino Breeder 4 (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k017Old0[] = { 0xC8 };
const uint8_t k017New0[] = { 0x00 };
const SgbcEdit k017[] = { { 0x002EF5, 1, k017Old0, k017New0 } };
// Dokapon! - Millennium Quest (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=0 fixed=0)
const uint8_t k018Old0[] = { 0xC8 };
const uint8_t k018New0[] = { 0x00 };
const SgbcEdit k018[] = { { 0x1ED106, 1, k018Old0, k018New0 } };
// Dragon Quest I & II (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k019Old0[] = { 0x18, 0x05 };
const uint8_t k019New0[] = { 0x18, 0x00 };
const SgbcEdit k019[] = { { 0x0001BE, 2, k019Old0, k019New0 } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Iru no Bouken (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k020Old0[] = { 0x18, 0x05 };
const uint8_t k020New0[] = { 0x18, 0x00 };
const SgbcEdit k020[] = { { 0x0001D1, 2, k020Old0, k020New0 } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k021Old0[] = { 0x18, 0x05 };
const uint8_t k021New0[] = { 0x18, 0x00 };
const SgbcEdit k021[] = { { 0x0001D1, 2, k021Old0, k021New0 } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k022Old0[] = { 0x18, 0x05 };
const uint8_t k022New0[] = { 0x18, 0x00 };
const SgbcEdit k022[] = { { 0x0001D1, 2, k022Old0, k022New0 } };
// Dragon Warrior I & II (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k023Old0[] = { 0x18, 0x05 };
const uint8_t k023New0[] = { 0x18, 0x00 };
const SgbcEdit k023[] = { { 0x0001BE, 2, k023Old0, k023New0 } };
// Dragon Warrior Monsters 2 - Cobi's Journey (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k024Old0[] = { 0x18, 0x05 };
const uint8_t k024New0[] = { 0x18, 0x00 };
const SgbcEdit k024[] = { { 0x0001D1, 2, k024Old0, k024New0 } };
// Dragon Warrior Monsters 2 - Tara's Adventure (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k025Old0[] = { 0x18, 0x05 };
const uint8_t k025New0[] = { 0x18, 0x00 };
const SgbcEdit k025[] = { { 0x0001D1, 2, k025Old0, k025New0 } };
// DT - Lords of Genomes (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k026Old0[] = { 0x28, 0x06 };
const uint8_t k026New0[] = { 0x00, 0x00 };
const SgbcEdit k026[] = { { 0x0001B4, 2, k026Old0, k026New0 } };
// Hanasaka Tenshi Tenten-kun no Beat Breaker (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k027Old0[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k027New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k027[] = { { 0x0001A9, 3, k027Old0, k027New0 } };
// Harvest Moon 2 GBC (Europe) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=0 fixed=0)
const uint8_t k028Old0[] = { 0x28, 0x39 };
const uint8_t k028New0[] = { 0x18, 0x39 };
const SgbcEdit k028[] = { { 0x0001B1, 2, k028Old0, k028New0 } };
// Harvest Moon 2 GBC (Germany) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=0 fixed=0)
const uint8_t k029Old0[] = { 0x28, 0x39 };
const uint8_t k029New0[] = { 0x18, 0x39 };
const SgbcEdit k029[] = { { 0x0001B1, 2, k029Old0, k029New0 } };
// Harvest Moon 2 GBC (USA) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=0 fixed=0)
const uint8_t k030Old0[] = { 0x28, 0x39 };
const uint8_t k030New0[] = { 0x18, 0x39 };
const SgbcEdit k030[] = { { 0x0001B1, 2, k030Old0, k030New0 } };
// Honkaku Yonin Uchi Mahjong - Mahjong Ou (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k031Old0[] = { 0xC2, 0xD8, 0x03 };
const uint8_t k031New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k031[] = { { 0x0003CD, 3, k031Old0, k031New0 } };
// Hyper Olympic Series - Track & Field GB (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k032Old0[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k032New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k032[] = { { 0x0001C8, 3, k032Old0, k032New0 } };
// International Track & Field (Europe) (En,Fr,De,It) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k033Old0[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k033New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k033[] = { { 0x0001C8, 3, k033Old0, k033New0 } };
// International Track & Field (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k034Old0[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k034New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k034[] = { { 0x0001C8, 3, k034Old0, k034New0 } };
// It's a World Rally (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k035Old0[] = { 0xCA, 0xDD, 0x01 };
const uint8_t k035New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k035[] = { { 0x0001D2, 3, k035Old0, k035New0 } };
// Jinsei Game - Tomodachi Takusan Tsukurou yo! (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=1 fixed=1)
const uint8_t k036Old0[] = { 0x18, 0x4A };
const uint8_t k036New0[] = { 0x18, 0x00 };
const SgbcEdit k036[] = { { 0x002F0E, 2, k036Old0, k036New0 } };
// Kinniku Banzuke GB 2 - Mezase! Muscle Champion (Japan) (SGB Enhanced) (GB Compatible).zip  (manual: flag routine answers SGB2, ref=? fixed=1)
const uint8_t k037Old0[] = { 0xCB, 0x4E };
const uint8_t k037New0[] = { 0xAF, 0x00 };
const SgbcEdit k037[] = { { 0x00025B, 2, k037Old0, k037New0 } };
// Koushien Pocket (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k038Old0[] = { 0xCA, 0x79, 0x02 };
const uint8_t k038New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k038[] = { { 0x00019E, 3, k038Old0, k038New0 } };
// Legend of Zelda, The - Link's Awakening DX (Europe) (Rev 2) (Beta) (1999-09-19) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k039Old0[] = { 0xC0 };
const uint8_t k039New0[] = { 0x00 };
const SgbcEdit k039[] = { { 0x0F2A25, 1, k039Old0, k039New0 } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k040Old0[] = { 0xC0 };
const uint8_t k040New0[] = { 0x00 };
const SgbcEdit k040[] = { { 0x0F2A25, 1, k040Old0, k040New0 } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-22) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k041Old0[] = { 0xC0 };
const uint8_t k041New0[] = { 0x00 };
const SgbcEdit k041[] = { { 0x0F2A25, 1, k041Old0, k041New0 } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k042Old0[] = { 0xC0 };
const uint8_t k042New0[] = { 0x00 };
const SgbcEdit k042[] = { { 0x0F2A25, 1, k042Old0, k042New0 } };
// Legend of Zelda, The - Link's Awakening DX (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k043Old0[] = { 0xC0 };
const uint8_t k043New0[] = { 0x00 };
const SgbcEdit k043[] = { { 0x0F2A25, 1, k043Old0, k043New0 } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Beta) (1998-12-11) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k044Old0[] = { 0xC0 };
const uint8_t k044New0[] = { 0x00 };
const SgbcEdit k044[] = { { 0x0F2A25, 1, k044Old0, k044New0 } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k045Old0[] = { 0xC0 };
const uint8_t k045New0[] = { 0x00 };
const SgbcEdit k045[] = { { 0x0F2A25, 1, k045Old0, k045New0 } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-22) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k046Old0[] = { 0xC0 };
const uint8_t k046New0[] = { 0x00 };
const SgbcEdit k046[] = { { 0x0F2A25, 1, k046Old0, k046New0 } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k047Old0[] = { 0xC0 };
const uint8_t k047New0[] = { 0x00 };
const SgbcEdit k047[] = { { 0x0F2A25, 1, k047Old0, k047New0 } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k048Old0[] = { 0xC0 };
const uint8_t k048New0[] = { 0x00 };
const SgbcEdit k048[] = { { 0x0F2A25, 1, k048Old0, k048New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k049Old0[] = { 0xC0 };
const uint8_t k049New0[] = { 0x00 };
const SgbcEdit k049[] = { { 0x0F2A25, 1, k049Old0, k049New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-08-19) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k050Old0[] = { 0xC0 };
const uint8_t k050New0[] = { 0x00 };
const SgbcEdit k050[] = { { 0x0F2A25, 1, k050Old0, k050New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k051Old0[] = { 0xC0 };
const uint8_t k051New0[] = { 0x00 };
const SgbcEdit k051[] = { { 0x0F2A25, 1, k051Old0, k051New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-07) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k052Old0[] = { 0xC0 };
const uint8_t k052New0[] = { 0x00 };
const SgbcEdit k052[] = { { 0x0F2A25, 1, k052Old0, k052New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-23) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k053Old0[] = { 0xC0 };
const uint8_t k053New0[] = { 0x00 };
const SgbcEdit k053[] = { { 0x0F2A25, 1, k053Old0, k053New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k054Old0[] = { 0xC0 };
const uint8_t k054New0[] = { 0x00 };
const SgbcEdit k054[] = { { 0x0F2A25, 1, k054Old0, k054New0 } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k055Old0[] = { 0xC0 };
const uint8_t k055New0[] = { 0x00 };
const SgbcEdit k055[] = { { 0x0F2A25, 1, k055Old0, k055New0 } };
// Lodoss-tou Senki - Eiyuu Kishiden GB (Japan) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k056Old0[] = { 0x28, 0x39 };
const uint8_t k056New0[] = { 0x18, 0x39 };
const SgbcEdit k056[] = { { 0x00019B, 2, k056Old0, k056New0 } };
// Men in Black - The Series (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k057Old0[] = { 0x20, 0x4C };
const uint8_t k057New0[] = { 0x18, 0x4C };
const SgbcEdit k057[] = { { 0x00026B, 2, k057Old0, k057New0 } };
// NHL 2000 (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k058Old0[] = { 0x20, 0x4C };
const uint8_t k058New0[] = { 0x18, 0x4C };
const SgbcEdit k058[] = { { 0x00021F, 2, k058Old0, k058New0 } };
// Ohasuta Yama-chan & Raymond (Japan) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k059Old0[] = { 0x20, 0x09 };
const uint8_t k059New0[] = { 0x18, 0x09 };
const SgbcEdit k059[] = { { 0x010450, 2, k059Old0, k059New0 } };
// Phantom Zona (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k060Old0[] = { 0x28, 0x17 };
const uint8_t k060New0[] = { 0x00, 0x00 };
const SgbcEdit k060[] = { { 0x000180, 2, k060Old0, k060New0 } };
// Pocket Bomberman (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=0 fixed=0)
const uint8_t k061Old0[] = { 0x20, 0x24 };
const uint8_t k061New0[] = { 0x18, 0x24 };
const SgbcEdit k061[] = { { 0x00024B, 2, k061Old0, k061New0 } };
// Pocket Monsters Gin (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k062Old0[] = { 0xC0 };
const uint8_t k062New0[] = { 0x00 };
const SgbcEdit k062[] = { { 0x009C59, 1, k062Old0, k062New0 } };
// Pocket Monsters Gin (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k063Old0[] = { 0xC0 };
const uint8_t k063New0[] = { 0x00 };
const SgbcEdit k063[] = { { 0x009C59, 1, k063Old0, k063New0 } };
// Pocket Monsters Kin (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k064Old0[] = { 0xC0 };
const uint8_t k064New0[] = { 0x00 };
const SgbcEdit k064[] = { { 0x009C59, 1, k064Old0, k064New0 } };
// Pokemon - Edicion Oro (Spain) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k065Old0[] = { 0xC0 };
const uint8_t k065New0[] = { 0x00 };
const SgbcEdit k065[] = { { 0x009CC3, 1, k065Old0, k065New0 } };
// Pokemon - Edicion Plata (Spain) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k066Old0[] = { 0xC0 };
const uint8_t k066New0[] = { 0x00 };
const SgbcEdit k066[] = { { 0x009CC3, 1, k066Old0, k066New0 } };
// Pokemon - Gold Version (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k067Old0[] = { 0xC0 };
const uint8_t k067New0[] = { 0x00 };
const SgbcEdit k067[] = { { 0x009CC3, 1, k067Old0, k067New0 } };
// Pokemon - Goldene Edition (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k068Old0[] = { 0xC0 };
const uint8_t k068New0[] = { 0x00 };
const SgbcEdit k068[] = { { 0x009CC3, 1, k068Old0, k068New0 } };
// Pokemon - Silberne Edition (Germany) (Beta) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k069Old0[] = { 0xC0 };
const uint8_t k069New0[] = { 0x00 };
const SgbcEdit k069[] = { { 0x009CC3, 1, k069Old0, k069New0 } };
// Pokemon - Silberne Edition (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k070Old0[] = { 0xC0 };
const uint8_t k070New0[] = { 0x00 };
const SgbcEdit k070[] = { { 0x009CC3, 1, k070Old0, k070New0 } };
// Pokemon - Silver Version (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k071Old0[] = { 0xC0 };
const uint8_t k071New0[] = { 0x00 };
const SgbcEdit k071[] = { { 0x009CC3, 1, k071Old0, k071New0 } };
// Pokemon - Version Argent (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k072Old0[] = { 0xC0 };
const uint8_t k072New0[] = { 0x00 };
const SgbcEdit k072[] = { { 0x009CC3, 1, k072Old0, k072New0 } };
// Pokemon - Version Or (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k073Old0[] = { 0xC0 };
const uint8_t k073New0[] = { 0x00 };
const SgbcEdit k073[] = { { 0x009CC3, 1, k073Old0, k073New0 } };
// Pokemon - Versione Argento (Italy) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k074Old0[] = { 0xC0 };
const uint8_t k074New0[] = { 0x00 };
const SgbcEdit k074[] = { { 0x009CC3, 1, k074Old0, k074New0 } };
// Pokemon - Versione Oro (Italy) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k075Old0[] = { 0xC0 };
const uint8_t k075New0[] = { 0x00 };
const SgbcEdit k075[] = { { 0x009CC3, 1, k075Old0, k075New0 } };
// Pokemon Gold (Taiwan) (En) (Unl).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k076Old0[] = { 0xC0 };
const uint8_t k076New0[] = { 0x00 };
const SgbcEdit k076[] = { { 0x009C59, 1, k076Old0, k076New0 } };
// Pokemon Trading Card Game (Europe) (En,Es,It) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (manual: detect, console stays CGB, ref=? fixed=0)
const uint8_t k077Old0[] = { 0x28, 0x0C };
const uint8_t k077New0[] = { 0x00, 0x00 };
const uint8_t k077Old1[] = { 0x06, 0x00 };
const uint8_t k077New1[] = { 0x06, 0x02 };
const uint8_t k077Old2[] = { 0x06, 0x01 };
const uint8_t k077New2[] = { 0x06, 0x02 };
const SgbcEdit k077[] = { { 0x00033E, 2, k077Old0, k077New0 }, { 0x000343, 2, k077Old1, k077New1 }, { 0x00034A, 2, k077Old2, k077New2 } };
// Pokemon Trading Card Game (Europe) (En,Es,It) (SGB Enhanced) (GB Compatible).zip  (manual: detect, console stays CGB, ref=? fixed=0)
const uint8_t k078Old0[] = { 0x28, 0x0C };
const uint8_t k078New0[] = { 0x00, 0x00 };
const uint8_t k078Old1[] = { 0x06, 0x00 };
const uint8_t k078New1[] = { 0x06, 0x02 };
const uint8_t k078Old2[] = { 0x06, 0x01 };
const uint8_t k078New2[] = { 0x06, 0x02 };
const SgbcEdit k078[] = { { 0x00033E, 2, k078Old0, k078New0 }, { 0x000343, 2, k078Old1, k078New1 }, { 0x00034A, 2, k078Old2, k078New2 } };
// Pokemon Trading Card Game (Europe) (En,Fr,De) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (manual: detect, console stays CGB, ref=? fixed=0)
const uint8_t k079Old0[] = { 0x28, 0x0C };
const uint8_t k079New0[] = { 0x00, 0x00 };
const uint8_t k079Old1[] = { 0x06, 0x00 };
const uint8_t k079New1[] = { 0x06, 0x02 };
const uint8_t k079Old2[] = { 0x06, 0x01 };
const uint8_t k079New2[] = { 0x06, 0x02 };
const SgbcEdit k079[] = { { 0x00033E, 2, k079Old0, k079New0 }, { 0x000343, 2, k079Old1, k079New1 }, { 0x00034A, 2, k079Old2, k079New2 } };
// Pokemon Trading Card Game (Europe) (En,Fr,De) (SGB Enhanced) (GB Compatible).zip  (manual: detect, console stays CGB, ref=? fixed=0)
const uint8_t k080Old0[] = { 0x28, 0x0C };
const uint8_t k080New0[] = { 0x00, 0x00 };
const uint8_t k080Old1[] = { 0x06, 0x00 };
const uint8_t k080New1[] = { 0x06, 0x02 };
const uint8_t k080Old2[] = { 0x06, 0x01 };
const uint8_t k080New2[] = { 0x06, 0x02 };
const SgbcEdit k080[] = { { 0x00033E, 2, k080Old0, k080New0 }, { 0x000343, 2, k080Old1, k080New1 }, { 0x00034A, 2, k080Old2, k080New2 } };
// Puchi Carat (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k081Old0[] = { 0xCA, 0x23, 0x41 };
const uint8_t k081New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k081[] = { { 0x00404D, 3, k081Old0, k081New0 } };
// Puchi Carat (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k082Old0[] = { 0xCA, 0x23, 0x41 };
const uint8_t k082New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k082[] = { { 0x00404D, 3, k082Old0, k082New0 } };
// Puchi Carat (USA) (Proto 1) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k083Old0[] = { 0xCA, 0x23, 0x41 };
const uint8_t k083New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k083[] = { { 0x00404D, 3, k083Old0, k083New0 } };
// Puchi Carat (USA) (Proto 2) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k084Old0[] = { 0xCA, 0x23, 0x41 };
const uint8_t k084New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k084[] = { { 0x00404D, 3, k084Old0, k084New0 } };
// Quartet (World) (GB Compatible) (Aftermarket) (Unl).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k085Old0[] = { 0x20, 0x28 };
const uint8_t k085New0[] = { 0x00, 0x00 };
const SgbcEdit k085[] = { { 0x0044A1, 2, k085Old0, k085New0 } };
// Robopon - Star Version (USA) (Proto).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k086Old0[] = { 0x28, 0x2A };
const uint8_t k086New0[] = { 0x00, 0x00 };
const SgbcEdit k086[] = { { 0x063210, 2, k086Old0, k086New0 } };
// Robopon - Sun Version (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k087Old0[] = { 0x28, 0x2A };
const uint8_t k087New0[] = { 0x00, 0x00 };
const SgbcEdit k087[] = { { 0x063210, 2, k087Old0, k087New0 } };
// Robot Poncots - Comic Bom Bom Special Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k088Old0[] = { 0x28, 0x2A };
const uint8_t k088New0[] = { 0x00, 0x00 };
const SgbcEdit k088[] = { { 0x063145, 2, k088Old0, k088New0 } };
// Robot Poncots - Moon Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k089Old0[] = { 0x28, 0x2A };
const uint8_t k089New0[] = { 0x00, 0x00 };
const SgbcEdit k089[] = { { 0x063145, 2, k089Old0, k089New0 } };
// Robot Poncots - Star Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k090Old0[] = { 0x28, 0x2A };
const uint8_t k090New0[] = { 0x00, 0x00 };
const SgbcEdit k090[] = { { 0x063143, 2, k090Old0, k090New0 } };
// Robot Poncots - Sun Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k091Old0[] = { 0x28, 0x2A };
const uint8_t k091New0[] = { 0x00, 0x00 };
const SgbcEdit k091[] = { { 0x063143, 2, k091Old0, k091New0 } };
// Senkai Ibunroku Juntei Taisen - TV Animation Senkaiden Houshin Engi Yori (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k092Old0[] = { 0x28, 0x0C };
const uint8_t k092New0[] = { 0x00, 0x00 };
const SgbcEdit k092[] = { { 0x0001D7, 2, k092Old0, k092New0 } };
// Super Black Bass Pocket 3 (Japan) (SGB Enhanced) (GB Compatible).zip  (manual: both flag tests off, ref=? fixed=1)
const uint8_t k093Old0[] = { 0x20, 0x07 };
const uint8_t k093New0[] = { 0x00, 0x00 };
const uint8_t k093Old1[] = { 0x20, 0x03 };
const uint8_t k093New1[] = { 0x00, 0x00 };
const SgbcEdit k093[] = { { 0x0002DE, 2, k093Old0, k093New0 }, { 0x0002E2, 2, k093Old1, k093New1 } };
// Sylvanian Families - Otogi no Kuni no Pendant (Japan) (SGB Enhanced) (GB Compatible).zip  (manual: flag routine answers SGB2, ref=? fixed=1)
const uint8_t k094Old0[] = { 0xCB, 0x4E };
const uint8_t k094New0[] = { 0xAF, 0x00 };
const SgbcEdit k094[] = { { 0x000256, 2, k094Old0, k094New0 } };
// Taito Memorial - Bubble Bobble (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k095Old0[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k095New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k095[] = { { 0x0001A6, 3, k095Old0, k095New0 } };
// Taito Memorial - Chase H.Q. - Secret Police (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k096Old0[] = { 0x28, 0x12 };
const uint8_t k096New0[] = { 0x00, 0x00 };
const SgbcEdit k096[] = { { 0x0001C0, 2, k096Old0, k096New0 } };
// TNN Outdoors Fishing Champ (USA) (SGB Enhanced) (GB Compatible).zip  (manual: both flag tests off, ref=? fixed=1)
const uint8_t k097Old0[] = { 0x20, 0x07 };
const uint8_t k097New0[] = { 0x00, 0x00 };
const uint8_t k097Old1[] = { 0x20, 0x03 };
const uint8_t k097New1[] = { 0x00, 0x00 };
const SgbcEdit k097[] = { { 0x0002DE, 2, k097Old0, k097New0 }, { 0x0002E2, 2, k097Old1, k097New1 } };
// Tsuri Sensei 2 (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k098Old0[] = { 0x28, 0x03 };
const uint8_t k098New0[] = { 0x00, 0x00 };
const SgbcEdit k098[] = { { 0x000220, 2, k098Old0, k098New0 } };
// Wario Land 2 (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k099Old0[] = { 0xC2, 0x55, 0x03 };
const uint8_t k099New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k099[] = { { 0x00025D, 3, k099Old0, k099New0 } };
// Wario Land II (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k100Old0[] = { 0xC2, 0x55, 0x03 };
const uint8_t k100New0[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k100[] = { { 0x00025D, 3, k100Old0, k100New0 } };
// Wetrix GB (Japan) (SGB Enhanced) (GB Compatible).zip  (force_jr, ref=1 fixed=1)
const uint8_t k101Old0[] = { 0x20, 0x03 };
const uint8_t k101New0[] = { 0x18, 0x03 };
const SgbcEdit k101[] = { { 0x000BA6, 2, k101Old0, k101New0 } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Beta) (1998-11-09) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k102Old0[] = { 0xC0 };
const uint8_t k102New0[] = { 0x00 };
const SgbcEdit k102[] = { { 0x0F2A25, 1, k102Old0, k102New0 } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-08-04) (SGB Enhanced).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k103Old0[] = { 0xC0 };
const uint8_t k103New0[] = { 0x00 };
const SgbcEdit k103[] = { { 0x0F2A25, 1, k103Old0, k103New0 } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-09-04) (SGB Enhanced).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k104Old0[] = { 0xC0 };
const uint8_t k104New0[] = { 0x00 };
const SgbcEdit k104[] = { { 0x0F2A25, 1, k104Old0, k104New0 } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k105Old0[] = { 0xC0 };
const uint8_t k105New0[] = { 0x00 };
const SgbcEdit k105[] = { { 0x0F2A25, 1, k105Old0, k105New0 } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k106Old0[] = { 0xC0 };
const uint8_t k106New0[] = { 0x00 };
const SgbcEdit k106[] = { { 0x0F2A25, 1, k106Old0, k106New0 } };
// Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (manual: no CGB return, reach SGB init, ref=? fixed=1)
const uint8_t k107Old0[] = { 0xC8 };
const uint8_t k107New0[] = { 0x00 };
const uint8_t k107Old1[] = { 0x28, 0x01 };
const uint8_t k107New1[] = { 0x18, 0x01 };
const SgbcEdit k107[] = { { 0x018010, 1, k107Old0, k107New0 }, { 0x018017, 2, k107Old1, k107New1 } };
// Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan) (SGB Enhanced) (GB Compatible).zip  (manual: no CGB return, reach SGB init, ref=? fixed=1)
const uint8_t k108Old0[] = { 0xC8 };
const uint8_t k108New0[] = { 0x00 };
const uint8_t k108Old1[] = { 0x28, 0x01 };
const uint8_t k108New1[] = { 0x18, 0x01 };
const SgbcEdit k108[] = { { 0x018010, 1, k108Old0, k108New0 }, { 0x018017, 2, k108Old1, k108New1 } };

const SgbcPatch kPatches[] = {
	{ 0x8A70, "POKEMON_GLDAAUJ\200", "36 in 1 (Taiwan) (SL36-0032) (Unl)", k000, 1 },
	{ 0x4AF7, "ANI3", "Animal Breeder 3 (Japan)", k001, 1 },
	{ 0xBAC0, "BALLOON GB", "Balloon Fight GB (Japan) (SGB Enhanced, GB Compatible) (NP)", k002, 1 },
	{ 0x9CD2, "BARDIGUN", "Barcode Taisen Bardigun (Japan) (Rev 1)", k003, 1 },
	{ 0x6824, "BARDIGUN", "Barcode Taisen Bardigun (Japan)", k004, 1 },
	{ 0x20D8, "B MASTER", "Blaster Master - Enemy Below (USA, Europe)", k005, 1 },
	{ 0xAC39, "BOMBERQUESTAQVP\200", "Bomberman Quest (Europe) (En,Fr,De)", k006, 1 },
	{ 0xE735, "BOMBERMAN QUEST\200", "Bomberman Quest (Japan)", k007, 1 },
	{ 0xEAA7, "BOMBERQUESTAVQE\200", "Bomberman Quest (USA)", k008, 1 },
	{ 0xB84C, "BUGS LIFE", "Bug's Life, A (Europe)", k009, 1 },
	{ 0xB74C, "BUGS LIFE", "Bug's Life, A (USA)", k010, 1 },
	{ 0x015F, "CHASE HQ", "Chase H.Q. - Secret Police (Europe)", k011, 1 },
	{ 0x06D2, "CHASE HQ", "Chase H.Q. - Secret Police (USA)", k012, 1 },
	{ 0xE7F0, "CHOROQ HCGBACQJ\200", "Choro Q - Hyper Customable GB (Japan)", k013, 1 },
	{ 0xC35C, "BUBBOB", "Classic Bubble Bobble (Europe)", k014, 1 },
	{ 0xCD62, "BUBBOB", "Classic Bubble Bobble (USA)", k015, 1 },
	{ 0x7961, "C C RACING ARLP\200", "Cross Country Racing (Europe) (En,Fr,De)", k016, 1 },
	{ 0x24D4, "DINO4", "Dino Breeder 4 (Japan)", k017, 1 },
	{ 0x3FDF, "DOKAPON", "Dokapon! - Millennium Quest (Japan)", k018, 1 },
	{ 0xC280, "DQ1&2", "Dragon Quest I & II (Japan)", k019, 1 },
	{ 0xF95B, "DQM2-I", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Iru no Bouken (Japan)", k020, 1 },
	{ 0x05BA, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (Rev 1)", k021, 1 },
	{ 0x04BC, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan)", k022, 1 },
	{ 0x2349, "DW1&2", "Dragon Warrior I & II (USA)", k023, 1 },
	{ 0x859E, "DWM2-C", "Dragon Warrior Monsters 2 - Cobi's Journey (USA)", k024, 1 },
	{ 0x853C, "DWM2-T", "Dragon Warrior Monsters 2 - Tara's Adventure (USA)", k025, 1 },
	{ 0xA73A, "DT GAMEBOY BBDJ\200", "DT - Lords of Genomes (Japan)", k026, 1 },
	{ 0x8980, "TENTEN", "Hanasaka Tenshi Tenten-kun no Beat Breaker (Japan)", k027, 1 },
	{ 0xE5F7, "H-MOON2 CGBBM2P\200", "Harvest Moon 2 GBC (Europe)", k028, 1 },
	{ 0xB3DC, "H-MOON2 CGBBM2D\200", "Harvest Moon 2 GBC (Germany)", k029, 1 },
	{ 0x271C, "H-MOON2 CGBBM2E\200", "Harvest Moon 2 GBC (USA)", k030, 1 },
	{ 0x6920, "MAHJONGOH", "Honkaku Yonin Uchi Mahjong - Mahjong Ou (Japan)", k031, 1 },
	{ 0xB3CA, "TRACK&FIELDAHDJ\200", "Hyper Olympic Series - Track & Field GB (Japan)", k032, 1 },
	{ 0x9897, "TRACK&FIELDAHDP\200", "International Track & Field (Europe) (En,Fr,De,It)", k033, 1 },
	{ 0x9A3A, "TRACK&FIELDAHDE\200", "International Track & Field (USA)", k034, 1 },
	{ 0xB9F1, "ITS W_RALLYARLJ\200", "It's a World Rally (Japan)", k035, 1 },
	{ 0x7C1B, "JINSEI TOMOACJJ\200", "Jinsei Game - Tomodachi Takusan Tsukurou yo! (Japan)", k036, 1 },
	{ 0x5650, "MUSCLERANK2B6KJ\200", "Kinniku Banzuke GB 2 - Mezase! Muscle Champion (Japan)", k037, 1 },
	{ 0x2DB1, "KOUSHIEN POCKET\200", "Koushien Pocket (Japan)", k038, 1 },
	{ 0x7789, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Europe) (Rev 2) (Beta) (1999-09-19)", k039, 1 },
	{ 0x5E40, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-04)", k040, 1 },
	{ 0x6046, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-22)", k041, 1 },
	{ 0x9836, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1)", k042, 1 },
	{ 0x1377, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France)", k043, 1 },
	{ 0x2D7E, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Beta) (1998-12-11)", k044, 1 },
	{ 0x89E8, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-04)", k045, 1 },
	{ 0x8BEE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-22)", k046, 1 },
	{ 0x8ACE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1)", k047, 1 },
	{ 0x2D09, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany)", k048, 1 },
	{ 0x2735, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 1)", k049, 1 },
	{ 0x0000, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-08-19)", k050, 1 },
	{ 0x7507, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-04)", k051, 1 },
	{ 0x7689, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-07)", k052, 1 },
	{ 0x788F, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-23)", k053, 1 },
	{ 0x0135, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2)", k054, 1 },
	{ 0xE3FD, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe)", k055, 1 },
	{ 0x259C, "LODOSS WAR GB", "Lodoss-tou Senki - Eiyuu Kishiden GB (Japan)", k056, 1 },
	{ 0xD1EF, "MEN IN BLACK", "Men in Black - The Series (USA, Europe)", k057, 1 },
	{ 0xA0C5, "NHL 2000", "NHL 2000 (USA, Europe)", k058, 1 },
	{ 0x5C51, "OHASTA Y&R", "Ohasuta Yama-chan & Raymond (Japan)", k059, 1 },
	{ 0x99CA, "PHANTOMZONABKZJ\200", "Phantom Zona (Japan)", k060, 1 },
	{ 0x41CB, "POKEBOM USA", "Pocket Bomberman (USA, Europe)", k061, 1 },
	{ 0x1D34, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan) (Rev 1)", k062, 1 },
	{ 0x7691, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan)", k063, 1 },
	{ 0x8460, "POKEMON_GLDAAUJ\200", "Pocket Monsters Kin (Japan) (Rev 1)", k064, 1 },
	{ 0x9353, "POKEMON_GLDAAUS\200", "Pokemon - Edicion Oro (Spain)", k065, 1 },
	{ 0x064B, "POKEMON_SLVAAXS\200", "Pokemon - Edicion Plata (Spain)", k066, 1 },
	{ 0x682D, "POKEMON_GLDAAUE\200", "Pokemon - Gold Version (USA, Europe)", k067, 1 },
	{ 0xDC97, "POKEMON_GLDAAUD\200", "Pokemon - Goldene Edition (Germany)", k068, 1 },
	{ 0x6A0E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany) (Beta)", k069, 1 },
	{ 0xCD6E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany)", k070, 1 },
	{ 0x0DAE, "POKEMON_SLVAAXE\200", "Pokemon - Silver Version (USA, Europe)", k071, 1 },
	{ 0xFB8C, "POKEMON_SLVAAXF\200", "Pokemon - Version Argent (France)", k072, 1 },
	{ 0x6FC6, "POKEMON_GLDAAUF\200", "Pokemon - Version Or (France)", k073, 1 },
	{ 0x7350, "POKEMON_SLVAAXI\200", "Pokemon - Versione Argento (Italy)", k074, 1 },
	{ 0xCE0C, "POKEMON_GLDAAUI\200", "Pokemon - Versione Oro (Italy)", k075, 1 },
	{ 0x365F, "POKEMON_GOLD_US\200", "Pokemon Gold (Taiwan) (En) (Unl)", k076, 1 },
	{ 0xB440, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Es,It) (Rev 1)", k077, 3 },
	{ 0xC869, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Es,It)", k078, 3 },
	{ 0x49CD, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Fr,De) (Rev 1)", k079, 3 },
	{ 0x5E22, "POKECARD", "Pokemon Trading Card Game (Europe) (En,Fr,De)", k080, 3 },
	{ 0x48E6, "PUCHI CARATAIQP\200", "Puchi Carat (Europe)", k081, 1 },
	{ 0xF6BD, "PUCHI CARATACUJ\200", "Puchi Carat (Japan)", k082, 1 },
	{ 0x7926, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 1)", k083, 1 },
	{ 0x1CF8, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 2)", k084, 1 },
	{ 0xDCBF, "QUARTET", "Quartet (World) (Aftermarket) (Unl)", k085, 1 },
	{ 0x9C19, "STAR", "Robopon - Star Version (USA) (Proto)", k086, 1 },
	{ 0xDA57, "SUN", "Robopon - Sun Version (USA)", k087, 1 },
	{ 0x4527, "ROBO BOM", "Robot Poncots - Comic Bom Bom Special Version (Japan)", k088, 1 },
	{ 0x9E0A, "ROBO MOON", "Robot Poncots - Moon Version (Japan)", k089, 1 },
	{ 0x0453, "ROBOPON STAR", "Robot Poncots - Star Version (Japan)", k090, 1 },
	{ 0xFD8C, "ROBOPON SUN", "Robot Poncots - Sun Version (Japan)", k091, 1 },
	{ 0x43D3, "SENKAIIBUNRBHSJ\200", "Senkai Ibunroku Juntei Taisen - TV Animation Senkaiden Houshin Engi Yori (Japan)", k092, 1 },
	{ 0xC16E, "BASS POCKET 3", "Super Black Bass Pocket 3 (Japan)", k093, 2 },
	{ 0xEEFE, "SYLVANIAN", "Sylvanian Families - Otogi no Kuni no Pendant (Japan)", k094, 1 },
	{ 0x1C2F, "BUBBOB", "Taito Memorial - Bubble Bobble (Japan)", k095, 1 },
	{ 0x46E9, "CHASE HQ", "Taito Memorial - Chase H.Q. - Secret Police (Japan)", k096, 1 },
	{ 0xDDE6, "TNN FISHINGAFCE\200", "TNN Outdoors Fishing Champ (USA)", k097, 2 },
	{ 0x9524, "TURISENSEI2AF2J\200", "Tsuri Sensei 2 (Japan)", k098, 1 },
	{ 0x968F, "CGBWARIOLAND2", "Wario Land 2 (Japan)", k099, 1 },
	{ 0x24C7, "CGBWARIOLAND2", "Wario Land II (USA, Europe)", k100, 1 },
	{ 0x439C, "WETRIX GB", "Wetrix GB (Japan)", k101, 1 },
	{ 0x9874, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Beta) (1998-11-09)", k102, 1 },
	{ 0xD553, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-08-04)", k103, 1 },
	{ 0x3115, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-09-04)", k104, 1 },
	{ 0x331B, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2)", k105, 1 },
	{ 0x9872, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan)", k106, 1 },
	{ 0x2874, "ZOIDS GENOBBGZJ\200", "Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan) (Rev 1)", k107, 2 },
	{ 0x3885, "ZOIDS GENOBBGZJ\200", "Zoids - Jashin Fukkatsu! Genobreaker Hen (Japan)", k108, 2 },
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
	for (int i = 0; i < p.edit_count; ++i)
	{
		const SgbcEdit &e = p.edits[i];
		if (e.addr + e.len > size) return false;
		if (std::memcmp(rom + e.addr, e.old, e.len) != 0) return false;
	}
	for (int i = 0; i < p.edit_count; ++i)
	{
		const SgbcEdit &e = p.edits[i];
		std::memcpy(rom + e.addr, e.neu, e.len);
	}
	return true;
}

} // namespace SGB
