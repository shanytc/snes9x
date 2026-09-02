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
const uint8_t k000Old[] = { 0xC0 };
const uint8_t k000New[] = { 0x00 };
const SgbcEdit k000[] = { { 0x009C59, 1, k000Old, k000New } };
// Balloon Fight GB (Japan) (SGB Enhanced, GB Compatible) (NP).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k001Old[] = { 0xC8 };
const uint8_t k001New[] = { 0x00 };
const SgbcEdit k001[] = { { 0x003B41, 1, k001Old, k001New } };
// Barcode Taisen Bardigun (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=0 fixed=0)
const uint8_t k002Old[] = { 0xC2, 0x2B, 0x02 };
const uint8_t k002New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k002[] = { { 0x000193, 3, k002Old, k002New } };
// Barcode Taisen Bardigun (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=0 fixed=0)
const uint8_t k003Old[] = { 0xC2, 0x1B, 0x02 };
const uint8_t k003New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k003[] = { { 0x000182, 3, k003Old, k003New } };
// Blaster Master - Enemy Below (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=1 fixed=1)
const uint8_t k004Old[] = { 0x18, 0x1D };
const uint8_t k004New[] = { 0x18, 0x00 };
const SgbcEdit k004[] = { { 0x00016E, 2, k004Old, k004New } };
// Chase H.Q. - Secret Police (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k005Old[] = { 0x28, 0x12 };
const uint8_t k005New[] = { 0x00, 0x00 };
const SgbcEdit k005[] = { { 0x0001C0, 2, k005Old, k005New } };
// Chase H.Q. - Secret Police (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k006Old[] = { 0x28, 0x12 };
const uint8_t k006New[] = { 0x00, 0x00 };
const SgbcEdit k006[] = { { 0x0001C0, 2, k006Old, k006New } };
// Choro Q - Hyper Customable GB (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k007Old[] = { 0x28, 0x2D };
const uint8_t k007New[] = { 0x00, 0x00 };
const SgbcEdit k007[] = { { 0x003171, 2, k007Old, k007New } };
// Classic Bubble Bobble (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k008Old[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k008New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k008[] = { { 0x0001A6, 3, k008Old, k008New } };
// Classic Bubble Bobble (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k009Old[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k009New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k009[] = { { 0x0001A6, 3, k009Old, k009New } };
// Cross Country Racing (Europe) (En,Fr,De) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k010Old[] = { 0xCA, 0xE0, 0x01 };
const uint8_t k010New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k010[] = { { 0x0001D2, 3, k010Old, k010New } };
// Dokapon! - Millennium Quest (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=0 fixed=0)
const uint8_t k011Old[] = { 0xC8 };
const uint8_t k011New[] = { 0x00 };
const SgbcEdit k011[] = { { 0x1ED106, 1, k011Old, k011New } };
// Dragon Quest I & II (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k012Old[] = { 0x18, 0x05 };
const uint8_t k012New[] = { 0x18, 0x00 };
const SgbcEdit k012[] = { { 0x0001BE, 2, k012Old, k012New } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Iru no Bouken (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k013Old[] = { 0x18, 0x05 };
const uint8_t k013New[] = { 0x18, 0x00 };
const SgbcEdit k013[] = { { 0x0001D1, 2, k013Old, k013New } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k014Old[] = { 0x18, 0x05 };
const uint8_t k014New[] = { 0x18, 0x00 };
const SgbcEdit k014[] = { { 0x0001D1, 2, k014Old, k014New } };
// Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k015Old[] = { 0x18, 0x05 };
const uint8_t k015New[] = { 0x18, 0x00 };
const SgbcEdit k015[] = { { 0x0001D1, 2, k015Old, k015New } };
// Dragon Warrior I & II (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k016Old[] = { 0x18, 0x05 };
const uint8_t k016New[] = { 0x18, 0x00 };
const SgbcEdit k016[] = { { 0x0001BE, 2, k016Old, k016New } };
// Dragon Warrior Monsters 2 - Cobi's Journey (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k017Old[] = { 0x18, 0x05 };
const uint8_t k017New[] = { 0x18, 0x00 };
const SgbcEdit k017[] = { { 0x0001D1, 2, k017Old, k017New } };
// Dragon Warrior Monsters 2 - Tara's Adventure (USA) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k018Old[] = { 0x18, 0x05 };
const uint8_t k018New[] = { 0x18, 0x00 };
const SgbcEdit k018[] = { { 0x0001D1, 2, k018Old, k018New } };
// DT - Lords of Genomes (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k019Old[] = { 0x28, 0x06 };
const uint8_t k019New[] = { 0x00, 0x00 };
const SgbcEdit k019[] = { { 0x0001B4, 2, k019Old, k019New } };
// Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k020Old[] = { 0xC8 };
const uint8_t k020New[] = { 0x00 };
const SgbcEdit k020[] = { { 0x0380FD, 1, k020Old, k020New } };
// Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k021Old[] = { 0xC8 };
const uint8_t k021New[] = { 0x00 };
const SgbcEdit k021[] = { { 0x0380FD, 1, k021Old, k021New } };
// Hanasaka Tenshi Tenten-kun no Beat Breaker (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k022Old[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k022New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k022[] = { { 0x0001A9, 3, k022Old, k022New } };
// Hyper Olympic Series - Track & Field GB (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k023Old[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k023New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k023[] = { { 0x0001C8, 3, k023Old, k023New } };
// International Rally (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k024Old[] = { 0xCA, 0xE3, 0x01 };
const uint8_t k024New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k024[] = { { 0x0001D5, 3, k024Old, k024New } };
// International Track & Field (Europe) (En,Fr,De,It) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k025Old[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k025New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k025[] = { { 0x0001C8, 3, k025Old, k025New } };
// International Track & Field (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k026Old[] = { 0xCA, 0xD1, 0x01 };
const uint8_t k026New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k026[] = { { 0x0001C8, 3, k026Old, k026New } };
// It's a World Rally (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k027Old[] = { 0xCA, 0xDD, 0x01 };
const uint8_t k027New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k027[] = { { 0x0001D2, 3, k027Old, k027New } };
// Jinsei Game - Tomodachi Takusan Tsukurou yo! (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=1 fixed=1)
const uint8_t k028Old[] = { 0x18, 0x4A };
const uint8_t k028New[] = { 0x18, 0x00 };
const SgbcEdit k028[] = { { 0x002F0E, 2, k028Old, k028New } };
// Legend of Zelda, The - Link's Awakening DX (Europe) (Rev 2) (Beta) (1999-09-19) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k029Old[] = { 0xC0 };
const uint8_t k029New[] = { 0x00 };
const SgbcEdit k029[] = { { 0x0F2A25, 1, k029Old, k029New } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k030Old[] = { 0xC0 };
const uint8_t k030New[] = { 0x00 };
const SgbcEdit k030[] = { { 0x0F2A25, 1, k030Old, k030New } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-22) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k031Old[] = { 0xC0 };
const uint8_t k031New[] = { 0x00 };
const SgbcEdit k031[] = { { 0x0F2A25, 1, k031Old, k031New } };
// Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k032Old[] = { 0xC0 };
const uint8_t k032New[] = { 0x00 };
const SgbcEdit k032[] = { { 0x0F2A25, 1, k032Old, k032New } };
// Legend of Zelda, The - Link's Awakening DX (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k033Old[] = { 0xC0 };
const uint8_t k033New[] = { 0x00 };
const SgbcEdit k033[] = { { 0x0F2A25, 1, k033Old, k033New } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Beta) (1998-12-11) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k034Old[] = { 0xC0 };
const uint8_t k034New[] = { 0x00 };
const SgbcEdit k034[] = { { 0x0F2A25, 1, k034Old, k034New } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k035Old[] = { 0xC0 };
const uint8_t k035New[] = { 0x00 };
const SgbcEdit k035[] = { { 0x0F2A25, 1, k035Old, k035New } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-22) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k036Old[] = { 0xC0 };
const uint8_t k036New[] = { 0x00 };
const SgbcEdit k036[] = { { 0x0F2A25, 1, k036Old, k036New } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k037Old[] = { 0xC0 };
const uint8_t k037New[] = { 0x00 };
const SgbcEdit k037[] = { { 0x0F2A25, 1, k037Old, k037New } };
// Legend of Zelda, The - Link's Awakening DX (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k038Old[] = { 0xC0 };
const uint8_t k038New[] = { 0x00 };
const SgbcEdit k038[] = { { 0x0F2A25, 1, k038Old, k038New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k039Old[] = { 0xC0 };
const uint8_t k039New[] = { 0x00 };
const SgbcEdit k039[] = { { 0x0F2A25, 1, k039Old, k039New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-08-19) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k040Old[] = { 0xC0 };
const uint8_t k040New[] = { 0x00 };
const SgbcEdit k040[] = { { 0x0F2A25, 1, k040Old, k040New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-04) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k041Old[] = { 0xC0 };
const uint8_t k041New[] = { 0x00 };
const SgbcEdit k041[] = { { 0x0F2A25, 1, k041Old, k041New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-07) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k042Old[] = { 0xC0 };
const uint8_t k042New[] = { 0x00 };
const SgbcEdit k042[] = { { 0x0F2A25, 1, k042Old, k042New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-23) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k043Old[] = { 0xC0 };
const uint8_t k043New[] = { 0x00 };
const SgbcEdit k043[] = { { 0x0F2A25, 1, k043Old, k043New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k044Old[] = { 0xC0 };
const uint8_t k044New[] = { 0x00 };
const SgbcEdit k044[] = { { 0x0F2A25, 1, k044Old, k044New } };
// Legend of Zelda, The - Link's Awakening DX (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k045Old[] = { 0xC0 };
const uint8_t k045New[] = { 0x00 };
const SgbcEdit k045[] = { { 0x0F2A25, 1, k045Old, k045New } };
// Mahjong Joou (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k046Old[] = { 0xC2, 0xD3, 0x0B };
const uint8_t k046New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k046[] = { { 0x000BB9, 3, k046Old, k046New } };
// Pocket King (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jr_to_sgb_entry, ref=0 fixed=0)
const uint8_t k047Old[] = { 0x18, 0x10 };
const uint8_t k047New[] = { 0x18, 0x00 };
const SgbcEdit k047[] = { { 0x0001C5, 2, k047Old, k047New } };
// Pocket Monsters Gin (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k048Old[] = { 0xC0 };
const uint8_t k048New[] = { 0x00 };
const SgbcEdit k048[] = { { 0x009C59, 1, k048Old, k048New } };
// Pocket Monsters Gin (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k049Old[] = { 0xC0 };
const uint8_t k049New[] = { 0x00 };
const SgbcEdit k049[] = { { 0x009C59, 1, k049Old, k049New } };
// Pocket Monsters Kin (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k050Old[] = { 0xC0 };
const uint8_t k050New[] = { 0x00 };
const SgbcEdit k050[] = { { 0x009C59, 1, k050Old, k050New } };
// Pokemon - Edicion Oro (Spain) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k051Old[] = { 0xC0 };
const uint8_t k051New[] = { 0x00 };
const SgbcEdit k051[] = { { 0x009CC3, 1, k051Old, k051New } };
// Pokemon - Edicion Plata (Spain) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k052Old[] = { 0xC0 };
const uint8_t k052New[] = { 0x00 };
const SgbcEdit k052[] = { { 0x009CC3, 1, k052Old, k052New } };
// Pokemon - Gold Version (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k053Old[] = { 0xC0 };
const uint8_t k053New[] = { 0x00 };
const SgbcEdit k053[] = { { 0x009CC3, 1, k053Old, k053New } };
// Pokemon - Goldene Edition (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k054Old[] = { 0xC0 };
const uint8_t k054New[] = { 0x00 };
const SgbcEdit k054[] = { { 0x009CC3, 1, k054Old, k054New } };
// Pokemon - Silberne Edition (Germany) (Beta) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k055Old[] = { 0xC0 };
const uint8_t k055New[] = { 0x00 };
const SgbcEdit k055[] = { { 0x009CC3, 1, k055Old, k055New } };
// Pokemon - Silberne Edition (Germany) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k056Old[] = { 0xC0 };
const uint8_t k056New[] = { 0x00 };
const SgbcEdit k056[] = { { 0x009CC3, 1, k056Old, k056New } };
// Pokemon - Silver Version (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k057Old[] = { 0xC0 };
const uint8_t k057New[] = { 0x00 };
const SgbcEdit k057[] = { { 0x009CC3, 1, k057Old, k057New } };
// Pokemon - Version Argent (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k058Old[] = { 0xC0 };
const uint8_t k058New[] = { 0x00 };
const SgbcEdit k058[] = { { 0x009CC3, 1, k058Old, k058New } };
// Pokemon - Version Or (France) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k059Old[] = { 0xC0 };
const uint8_t k059New[] = { 0x00 };
const SgbcEdit k059[] = { { 0x009CC3, 1, k059Old, k059New } };
// Pokemon - Versione Argento (Italy) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k060Old[] = { 0xC0 };
const uint8_t k060New[] = { 0x00 };
const SgbcEdit k060[] = { { 0x009CC3, 1, k060Old, k060New } };
// Pokemon - Versione Oro (Italy) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k061Old[] = { 0xC0 };
const uint8_t k061New[] = { 0x00 };
const SgbcEdit k061[] = { { 0x009CC3, 1, k061Old, k061New } };
// Pokemon Card GB (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k062Old[] = { 0x28, 0x0C };
const uint8_t k062New[] = { 0x00, 0x00 };
const SgbcEdit k062[] = { { 0x000337, 2, k062Old, k062New } };
// Pokemon Gold (Taiwan) (En) (Unl).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k063Old[] = { 0xC0 };
const uint8_t k063New[] = { 0x00 };
const SgbcEdit k063[] = { { 0x009C59, 1, k063Old, k063New } };
// Pokemon Trading Card Game (USA, Australia) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k064Old[] = { 0x28, 0x0C };
const uint8_t k064New[] = { 0x00, 0x00 };
const SgbcEdit k064[] = { { 0x00034D, 2, k064Old, k064New } };
// Power Pro Kun Pocket (Japan) (Rev 1) (SGB Enhanced) (GB Compatible).zip  (retarget_jp_to_sgb_entry, ref=1 fixed=1)
const uint8_t k065Old[] = { 0xC3, 0xC6, 0x11 };
const uint8_t k065New[] = { 0xC3, 0x64, 0x6B };
const SgbcEdit k065[] = { { 0x05AB4F, 3, k065Old, k065New } };
// Power Pro Kun Pocket (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jp_to_sgb_entry, ref=1 fixed=1)
const uint8_t k066Old[] = { 0xC3, 0xC6, 0x11 };
const uint8_t k066New[] = { 0xC3, 0x64, 0x6B };
const SgbcEdit k066[] = { { 0x05AB4F, 3, k066Old, k066New } };
// Power Pro Kun Pocket 2 (Japan) (SGB Enhanced) (GB Compatible).zip  (retarget_jp_to_sgb_entry, ref=1 fixed=1)
const uint8_t k067Old[] = { 0xC3, 0x61, 0x10 };
const uint8_t k067New[] = { 0xC3, 0x24, 0x67 };
const SgbcEdit k067[] = { { 0x05A70F, 3, k067Old, k067New } };
// Puchi Carat (Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k068Old[] = { 0xCA, 0x23, 0x41 };
const uint8_t k068New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k068[] = { { 0x00404D, 3, k068Old, k068New } };
// Puchi Carat (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k069Old[] = { 0xCA, 0x23, 0x41 };
const uint8_t k069New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k069[] = { { 0x00404D, 3, k069Old, k069New } };
// Puchi Carat (USA) (Proto 1) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k070Old[] = { 0xCA, 0x23, 0x41 };
const uint8_t k070New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k070[] = { { 0x00404D, 3, k070Old, k070New } };
// Puchi Carat (USA) (Proto 2) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k071Old[] = { 0xCA, 0x23, 0x41 };
const uint8_t k071New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k071[] = { { 0x00404D, 3, k071Old, k071New } };
// Quartet (World) (GB Compatible) (Aftermarket) (Unl).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k072Old[] = { 0x20, 0x28 };
const uint8_t k072New[] = { 0x00, 0x00 };
const SgbcEdit k072[] = { { 0x0044A1, 2, k072Old, k072New } };
// Robopon - Star Version (USA) (Proto).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k073Old[] = { 0x28, 0x2A };
const uint8_t k073New[] = { 0x00, 0x00 };
const SgbcEdit k073[] = { { 0x063210, 2, k073Old, k073New } };
// Robopon - Sun Version (USA) (Beta) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k074Old[] = { 0x28, 0x2A };
const uint8_t k074New[] = { 0x00, 0x00 };
const SgbcEdit k074[] = { { 0x063205, 2, k074Old, k074New } };
// Robopon - Sun Version (USA) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k075Old[] = { 0x28, 0x2A };
const uint8_t k075New[] = { 0x00, 0x00 };
const SgbcEdit k075[] = { { 0x063210, 2, k075Old, k075New } };
// Robot Poncots - Comic Bom Bom Special Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k076Old[] = { 0x28, 0x2A };
const uint8_t k076New[] = { 0x00, 0x00 };
const SgbcEdit k076[] = { { 0x063145, 2, k076Old, k076New } };
// Robot Poncots - Moon Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k077Old[] = { 0x28, 0x2A };
const uint8_t k077New[] = { 0x00, 0x00 };
const SgbcEdit k077[] = { { 0x063145, 2, k077Old, k077New } };
// Robot Poncots - Star Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k078Old[] = { 0x28, 0x2A };
const uint8_t k078New[] = { 0x00, 0x00 };
const SgbcEdit k078[] = { { 0x063143, 2, k078Old, k078New } };
// Robot Poncots - Sun Version (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=0)
const uint8_t k079Old[] = { 0x28, 0x2A };
const uint8_t k079New[] = { 0x00, 0x00 };
const SgbcEdit k079[] = { { 0x063143, 2, k079Old, k079New } };
// Senkai Ibunroku Juntei Taisen - TV Animation Senkaiden Houshin Engi Yori (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k080Old[] = { 0x28, 0x0C };
const uint8_t k080New[] = { 0x00, 0x00 };
const SgbcEdit k080[] = { { 0x0001D7, 2, k080Old, k080New } };
// Taito Memorial - Bubble Bobble (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k081Old[] = { 0xCA, 0x8D, 0x02 };
const uint8_t k081New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k081[] = { { 0x0001A6, 3, k081Old, k081New } };
// Taito Memorial - Chase H.Q. - Secret Police (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k082Old[] = { 0x28, 0x12 };
const uint8_t k082New[] = { 0x00, 0x00 };
const SgbcEdit k082[] = { { 0x0001C0, 2, k082Old, k082New } };
// Tales of Phantasia - Narikiri Dungeon (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k083Old[] = { 0x28, 0x22 };
const uint8_t k083New[] = { 0x00, 0x00 };
const SgbcEdit k083[] = { { 0x004108, 2, k083Old, k083New } };
// Tsuri Sensei 2 (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k084Old[] = { 0x28, 0x03 };
const uint8_t k084New[] = { 0x00, 0x00 };
const SgbcEdit k084[] = { { 0x000220, 2, k084Old, k084New } };
// Wario Land 2 (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k085Old[] = { 0xC2, 0x55, 0x03 };
const uint8_t k085New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k085[] = { { 0x00025D, 3, k085Old, k085New } };
// Wario Land II (USA, Europe) (SGB Enhanced) (GB Compatible).zip  (nop_skip_branch, ref=1 fixed=1)
const uint8_t k086Old[] = { 0xC2, 0x55, 0x03 };
const uint8_t k086New[] = { 0x00, 0x00, 0x00 };
const SgbcEdit k086[] = { { 0x00025D, 3, k086Old, k086New } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Beta) (1998-11-09) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k087Old[] = { 0xC0 };
const uint8_t k087New[] = { 0x00 };
const SgbcEdit k087[] = { { 0x0F2A25, 1, k087Old, k087New } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-08-04) (SGB Enhanced).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k088Old[] = { 0xC0 };
const uint8_t k088New[] = { 0x00 };
const SgbcEdit k088[] = { { 0x0F2A25, 1, k088Old, k088New } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-09-04) (SGB Enhanced).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k089Old[] = { 0xC0 };
const uint8_t k089New[] = { 0x00 };
const SgbcEdit k089[] = { { 0x0F2A25, 1, k089Old, k089New } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k090Old[] = { 0xC0 };
const uint8_t k090New[] = { 0x00 };
const SgbcEdit k090[] = { { 0x0F2A25, 1, k090Old, k090New } };
// Zelda no Densetsu - Yume o Miru Shima DX (Japan) (SGB Enhanced) (GB Compatible).zip  (nop_early_ret, ref=1 fixed=1)
const uint8_t k091Old[] = { 0xC0 };
const uint8_t k091New[] = { 0x00 };
const SgbcEdit k091[] = { { 0x0F2A25, 1, k091Old, k091New } };

const SgbcPatch kPatches[] = {
	{ 0x8A70, "POKEMON_GLDAAUJ\200", "36 in 1 (Taiwan) (SL36-0032) (Unl)", k000, 1 },
	{ 0xBAC0, "BALLOON GB", "Balloon Fight GB (Japan) (SGB Enhanced, GB Compatible) (NP)", k001, 1 },
	{ 0x9CD2, "BARDIGUN", "Barcode Taisen Bardigun (Japan) (Rev 1)", k002, 1 },
	{ 0x6824, "BARDIGUN", "Barcode Taisen Bardigun (Japan)", k003, 1 },
	{ 0x20D8, "B MASTER", "Blaster Master - Enemy Below (USA, Europe)", k004, 1 },
	{ 0x015F, "CHASE HQ", "Chase H.Q. - Secret Police (Europe)", k005, 1 },
	{ 0x06D2, "CHASE HQ", "Chase H.Q. - Secret Police (USA)", k006, 1 },
	{ 0xE7F0, "CHOROQ HCGBACQJ\200", "Choro Q - Hyper Customable GB (Japan)", k007, 1 },
	{ 0xC35C, "BUBBOB", "Classic Bubble Bobble (Europe)", k008, 1 },
	{ 0xCD62, "BUBBOB", "Classic Bubble Bobble (USA)", k009, 1 },
	{ 0x7961, "C C RACING ARLP\200", "Cross Country Racing (Europe) (En,Fr,De)", k010, 1 },
	{ 0x3FDF, "DOKAPON", "Dokapon! - Millennium Quest (Japan)", k011, 1 },
	{ 0xC280, "DQ1&2", "Dragon Quest I & II (Japan)", k012, 1 },
	{ 0xF95B, "DQM2-I", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Iru no Bouken (Japan)", k013, 1 },
	{ 0x05BA, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan) (Rev 1)", k014, 1 },
	{ 0x04BC, "DQM2-R", "Dragon Quest Monsters 2 - Maruta no Fushigi na Kagi - Ruka no Tabidachi (Japan)", k015, 1 },
	{ 0x2349, "DW1&2", "Dragon Warrior I & II (USA)", k016, 1 },
	{ 0x859E, "DWM2-C", "Dragon Warrior Monsters 2 - Cobi's Journey (USA)", k017, 1 },
	{ 0x853C, "DWM2-T", "Dragon Warrior Monsters 2 - Tara's Adventure (USA)", k018, 1 },
	{ 0xA73A, "DT GAMEBOY BBDJ\200", "DT - Lords of Genomes (Japan)", k019, 1 },
	{ 0x8DB6, "FAIRY   KITTY", "Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan) (Rev 1)", k020, 1 },
	{ 0xA958, "FAIRY   KITTY", "Fairy Kitty no Kaiun Jiten - Yousei no Kuni no Uranai Shugyou (Japan)", k021, 1 },
	{ 0x8980, "TENTEN", "Hanasaka Tenshi Tenten-kun no Beat Breaker (Japan)", k022, 1 },
	{ 0xB3CA, "TRACK&FIELDAHDJ\200", "Hyper Olympic Series - Track & Field GB (Japan)", k023, 1 },
	{ 0xF4B2, "INTER RALLYARLE\200", "International Rally (USA)", k024, 1 },
	{ 0x9897, "TRACK&FIELDAHDP\200", "International Track & Field (Europe) (En,Fr,De,It)", k025, 1 },
	{ 0x9A3A, "TRACK&FIELDAHDE\200", "International Track & Field (USA)", k026, 1 },
	{ 0xB9F1, "ITS W_RALLYARLJ\200", "It's a World Rally (Japan)", k027, 1 },
	{ 0x7C1B, "JINSEI TOMOACJJ\200", "Jinsei Game - Tomodachi Takusan Tsukurou yo! (Japan)", k028, 1 },
	{ 0x7789, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Europe) (Rev 2) (Beta) (1999-09-19)", k029, 1 },
	{ 0x5E40, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-04)", k030, 1 },
	{ 0x6046, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1) (Beta) (1999-09-22)", k031, 1 },
	{ 0x9836, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France) (Rev 1)", k032, 1 },
	{ 0x1377, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (France)", k033, 1 },
	{ 0x2D7E, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Beta) (1998-12-11)", k034, 1 },
	{ 0x89E8, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-04)", k035, 1 },
	{ 0x8BEE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1) (Beta) (1999-09-22)", k036, 1 },
	{ 0x8ACE, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany) (Rev 1)", k037, 1 },
	{ 0x2D09, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (Germany)", k038, 1 },
	{ 0x2735, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 1)", k039, 1 },
	{ 0x0000, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-08-19)", k040, 1 },
	{ 0x7507, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-04)", k041, 1 },
	{ 0x7689, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-07)", k042, 1 },
	{ 0x788F, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (Beta) (1999-09-23)", k043, 1 },
	{ 0x0135, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2)", k044, 1 },
	{ 0xE3FD, "ZELDA", "Legend of Zelda, The - Link's Awakening DX (USA, Europe)", k045, 1 },
	{ 0xBE50, "MAHJONGJOOHA56J\200", "Mahjong Joou (Japan)", k046, 1 },
	{ 0xF8B7, "POCKET KINGAV5J\200", "Pocket King (Japan)", k047, 1 },
	{ 0x1D34, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan) (Rev 1)", k048, 1 },
	{ 0x7691, "POKEMON_SLVAAXJ\200", "Pocket Monsters Gin (Japan)", k049, 1 },
	{ 0x8460, "POKEMON_GLDAAUJ\200", "Pocket Monsters Kin (Japan) (Rev 1)", k050, 1 },
	{ 0x9353, "POKEMON_GLDAAUS\200", "Pokemon - Edicion Oro (Spain)", k051, 1 },
	{ 0x064B, "POKEMON_SLVAAXS\200", "Pokemon - Edicion Plata (Spain)", k052, 1 },
	{ 0x682D, "POKEMON_GLDAAUE\200", "Pokemon - Gold Version (USA, Europe)", k053, 1 },
	{ 0xDC97, "POKEMON_GLDAAUD\200", "Pokemon - Goldene Edition (Germany)", k054, 1 },
	{ 0x6A0E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany) (Beta)", k055, 1 },
	{ 0xCD6E, "POKEMON_SLVAAXD\200", "Pokemon - Silberne Edition (Germany)", k056, 1 },
	{ 0x0DAE, "POKEMON_SLVAAXE\200", "Pokemon - Silver Version (USA, Europe)", k057, 1 },
	{ 0xFB8C, "POKEMON_SLVAAXF\200", "Pokemon - Version Argent (France)", k058, 1 },
	{ 0x6FC6, "POKEMON_GLDAAUF\200", "Pokemon - Version Or (France)", k059, 1 },
	{ 0x7350, "POKEMON_SLVAAXI\200", "Pokemon - Versione Argento (Italy)", k060, 1 },
	{ 0xCE0C, "POKEMON_GLDAAUI\200", "Pokemon - Versione Oro (Italy)", k061, 1 },
	{ 0xD526, "POKEMON CARD GB\200", "Pokemon Card GB (Japan)", k062, 1 },
	{ 0x365F, "POKEMON_GOLD_US\200", "Pokemon Gold (Taiwan) (En) (Unl)", k063, 1 },
	{ 0x26A6, "POKECARD", "Pokemon Trading Card Game (USA, Australia)", k064, 1 },
	{ 0xB727, "PP SUCSESS", "Power Pro Kun Pocket (Japan) (Rev 1)", k065, 1 },
	{ 0xC4E9, "PP SUCSESS", "Power Pro Kun Pocket (Japan)", k066, 1 },
	{ 0xA977, "PAWA POKE2", "Power Pro Kun Pocket 2 (Japan)", k067, 1 },
	{ 0x48E6, "PUCHI CARATAIQP\200", "Puchi Carat (Europe)", k068, 1 },
	{ 0xF6BD, "PUCHI CARATACUJ\200", "Puchi Carat (Japan)", k069, 1 },
	{ 0x7926, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 1)", k070, 1 },
	{ 0x1CF8, "PUCHI CARATAIQE\200", "Puchi Carat (USA) (Proto 2)", k071, 1 },
	{ 0xDCBF, "QUARTET", "Quartet (World) (Aftermarket) (Unl)", k072, 1 },
	{ 0x9C19, "STAR", "Robopon - Star Version (USA) (Proto)", k073, 1 },
	{ 0x5D30, "SUN", "Robopon - Sun Version (USA) (Beta)", k074, 1 },
	{ 0xDA57, "SUN", "Robopon - Sun Version (USA)", k075, 1 },
	{ 0x4527, "ROBO BOM", "Robot Poncots - Comic Bom Bom Special Version (Japan)", k076, 1 },
	{ 0x9E0A, "ROBO MOON", "Robot Poncots - Moon Version (Japan)", k077, 1 },
	{ 0x0453, "ROBOPON STAR", "Robot Poncots - Star Version (Japan)", k078, 1 },
	{ 0xFD8C, "ROBOPON SUN", "Robot Poncots - Sun Version (Japan)", k079, 1 },
	{ 0x43D3, "SENKAIIBUNRBHSJ\200", "Senkai Ibunroku Juntei Taisen - TV Animation Senkaiden Houshin Engi Yori (Japan)", k080, 1 },
	{ 0x1C2F, "BUBBOB", "Taito Memorial - Bubble Bobble (Japan)", k081, 1 },
	{ 0x46E9, "CHASE HQ", "Taito Memorial - Chase H.Q. - Secret Police (Japan)", k082, 1 },
	{ 0x5ADB, "TOPNARIKIRIAN6J\200", "Tales of Phantasia - Narikiri Dungeon (Japan)", k083, 1 },
	{ 0x9524, "TURISENSEI2AF2J\200", "Tsuri Sensei 2 (Japan)", k084, 1 },
	{ 0x968F, "CGBWARIOLAND2", "Wario Land 2 (Japan)", k085, 1 },
	{ 0x24C7, "CGBWARIOLAND2", "Wario Land II (USA, Europe)", k086, 1 },
	{ 0x9874, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Beta) (1998-11-09)", k087, 1 },
	{ 0xD553, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-08-04)", k088, 1 },
	{ 0x3115, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2) (Beta) (1999-09-04)", k089, 1 },
	{ 0x331B, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan) (Rev 2)", k090, 1 },
	{ 0x9872, "ZELDA", "Zelda no Densetsu - Yume o Miru Shima DX (Japan)", k091, 1 },
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
