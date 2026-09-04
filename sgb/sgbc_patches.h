/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Game Boy Color: the in-memory patches. The SGB2 SNES-side BIOS takes
// the built-in IPS that keys its pane; a dual (SGB + CGB) cart takes the byte
// edits that make it run its SGB init on the Color branch too.

#ifndef _SGB_SGBC_PATCHES_H_
#define _SGB_SGBC_PATCHES_H_

#include <cstdint>
#include <cstddef>
#include <vector>

namespace SGB {

// Apply a plain IPS ("PATCH", 5-byte records, RLE records, "EOF") to `rom` in
// place; a record past the end grows it. False on a malformed patch, with
// `rom` untouched.
bool ApplyIps(std::vector<uint8_t> &rom, const uint8_t *ips, size_t len);

// The built-in patch for the SGB2 BIOS, applied only to the dump it was built
// against (SHA-256 of the pristine 512 KB image). True when `bios` was patched.
bool PatchSgbcBios(std::vector<uint8_t> &bios);

// One byte run in a cart image, up to three bytes (a jump, a store): `old`
// is verified before `neu` is written.
struct SgbcEdit
{
	uint32_t addr;
	uint8_t  len;
	uint8_t  old[3];
	uint8_t  neu[3];
};

// A dual cart picks its branch from A at $0100 and runs no SGB code on the
// Color branch, so the BIOS never gets its border or sound packets. These
// edits make a known game run its SGB init as well: one row per cart, its
// edits inline (four at most so far; unused slots stay zero).
struct SgbcPatch
{
	uint16_t    global_sum;   // header $014E-$014F, big-endian as stored
	const char *title;        // header $0134.., up to the first NUL
	const char *name;         // shown on the load banner
	uint8_t     edit_count;
	SgbcEdit    edits[4];
};

const SgbcPatch *FindSgbcPatch(const uint8_t *rom, size_t size);

// Super Game Boy compatibility: a cart whose own SGB path stops under a BIOS
// (Joryuu Janshi parks on the SGB2 boot value). Same row shape; applied in
// memory under any SGB BIOS, Super Game Boy Color included.
const SgbcPatch *FindSgbPatch(const uint8_t *rom, size_t size);

// True when every edit's old bytes matched and all were written; false with
// nothing written otherwise.
bool ApplySgbcPatch(const SgbcPatch &p, uint8_t *rom, size_t size);

} // namespace SGB

#endif
