/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _BIOSMANAGER_H_
#define _BIOSMANAGER_H_

#include "port.h"
#include <string>
#include <vector>

// User-assigned BIOS file paths, edited through File -> BIOS Manager. A slot's
// path is the only place its loader reads a BIOS from: files can live anywhere
// under any name, and a blank slot means that system runs without one.
enum S9xBiosSlot
{
	S9X_BIOS_GB = 0,        // dmg_boot.bin — Game Boy boot ROM
	S9X_BIOS_GBC,           // cgb_boot.bin — Game Boy Color boot ROM
	S9X_BIOS_SGB1,          // sgb.sfc      — Super Game Boy
	S9X_BIOS_SGB2,          // sgb2.sfc     — Super Game Boy 2
	S9X_BIOS_SGB1_BOOT,     // sgb.boot.rom — Super Game Boy GB-side boot ROM
	S9X_BIOS_SGB2_BOOT,     // sgb2.boot.rom — the SGB2 one
	S9X_BIOS_SFCBOX_KROM,   // KROM1.BIN    — Super Famicom Box supervisor
	S9X_BIOS_SFCBOX_FONT,   // MB90082.BIN  — Super Famicom Box OSD font
	S9X_BIOS_BSX,           // BS-X.bin     — Satellaview
	S9X_BIOS_SUFAMI,        // STBIOS.bin   — Sufami Turbo
	S9X_BIOS_NSS,           // nss-ic14.02.ic14 — Nintendo Super System supervisor
	S9X_BIOS_NSS_FONT,      // m50458_char.bin  — its M50458 OSD charset
	S9X_BIOS_SUPERDISC,     // SDBR_v0.95.sfc   — Super Disc BIOS cartridge
	S9X_NUM_BIOS_SLOTS
};

struct S9xBiosSlotInfo
{
	const char *key;           // config key under [BIOS]
	const char *label;         // dialog row label
	const char *const *names;  // conventional filenames, NULL-ended; [0] is the dialog hint
	uint32      size;          // expected byte count, 0 = any
	const char *note;          // shown while the slot is empty, NULL if required
};

#define S9X_BIOS_PATH_MAX 512

// nocash's NSS-TEST.BIN: a hardware test that runs in place of the NSS BIOS.
#define S9X_NSS_NOCASH_TEST_CRC 0x15616021u

const S9xBiosSlotInfo *S9xGetBiosSlotInfo (int slot);

// Empty string when the slot is unassigned. Set with an empty path to clear.
const char *S9xGetBiosPath (int slot);
void        S9xSetBiosPath (int slot, const char *path);

// Raw buffer for config backends that bind a char array (win32 wconfig).
char *S9xGetBiosPathBuffer (int slot);

// Every slot's path in one string: keep one and compare later to learn
// whether the BIOS Manager changed anything in between.
std::string S9xBiosPathsFingerprint (void);

// Why an assigned path is or isn't loadable, for the dialog's status column.
enum S9xBiosPathStatus
{
	S9X_BIOS_PATH_UNSET = 0,   // no path assigned to this slot
	S9X_BIOS_PATH_MISSING,     // assigned, but nothing readable is there
	S9X_BIOS_PATH_BAD_SIZE,    // readable, but not a size this slot takes
	S9X_BIOS_PATH_BAD_IMAGE,   // right size, but the loader's header check fails
	S9X_BIOS_PATH_OK
};

// Runs the same tests the loader will, so OK means the file will actually be
// used. `detail` (optional) comes back with a short phrase for the dialog:
// on failure why, and on success which revision, where that is worth saying.
S9xBiosPathStatus S9xCheckBiosPath (int slot, std::string *detail = NULL);

// Shorthand for S9xCheckBiosPath(slot) == S9X_BIOS_PATH_OK. Loaders read the
// path themselves and run without a BIOS when it fails, so a bad file is a
// warning, not a block.
bool8 S9xBiosPathUsable (int slot);

// Assigned path when it is readable, otherwise "". Empty means the loader has
// no BIOS for that slot; nothing else is searched.
std::string S9xResolveBiosPath (int slot);

// Vets one candidate image; return true to accept it. `size` is how much was
// read (capped at max_size), `full_size` the candidate's real length — they
// differ when a caller only wants a header prefix, and a filter that cares
// about exact sizes must test `full_size`. `ctx` is passed through.
typedef bool (*S9xBiosAcceptFn) (const uint8 *data, uint32 size, uint32 full_size,
                                 void *ctx);

// Read a BIOS image into `out`, at most `max_size` bytes. `path` may be a plain
// file or a .zip, in which case its members are inflated in memory and the
// largest accepted one wins. A NULL `accept` takes anything non-empty.
bool8 S9xReadBiosImage (const char *path, std::vector<uint8> &out, uint32 max_size,
                        S9xBiosAcceptFn accept = NULL, void *ctx = NULL);

#endif
