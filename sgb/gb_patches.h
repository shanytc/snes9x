/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Game Boy cart edits made in memory, outside Super Game Boy Color (which keeps
// its own in sgbc_patches): SGB compatibility rows, applied under an SGB BIOS,
// and link-cable rows, in the cart only while a link session runs.

#ifndef _SGB_GB_PATCHES_H_
#define _SGB_GB_PATCHES_H_

#include <cstdint>
#include <cstddef>

namespace SGB {

// One byte run in the cart image, up to three bytes: `old` is verified before
// `neu` is written.
struct GbCartEdit
{
	uint32_t addr;
	uint8_t  len;
	uint8_t  old[3];
	uint8_t  neu[3];
};

// One row per cart, found by header global-sum + title, never a whole-ROM hash.
static const int kGbCartPatchMaxEdits = 12;
struct GbCartPatch
{
	uint16_t    global_sum;   // header $014E-$014F, big-endian as stored
	const char *title;        // header $0134.., up to the first NUL
	const char *name;         // shown on the load banner
	uint8_t     edit_count;
	GbCartEdit  edits[kGbCartPatchMaxEdits];
};

// Super Game Boy compatibility: a cart whose own SGB path stops under a BIOS
// (Joryuu Janshi parks on the SGB2 boot value). Applied under any SGB BIOS,
// Super Game Boy Color included.
const GbCartPatch *FindSgbCartPatch(const uint8_t *rom, size_t size);

// Link-cable fixes, put in and taken out at the cart's power-on: on every model,
// and for code only an SGB BIOS runs (Uno 2's packet waits).
const GbCartPatch *FindGbLinkCartPatch(const uint8_t *rom, size_t size);
const GbCartPatch *FindSgbLinkCartPatch(const uint8_t *rom, size_t size);

// Write a row's edits into the cart image in memory. True when every edit's
// old bytes matched and all were written; false with nothing written otherwise.
bool ApplyGbCartPatch(const GbCartPatch &p, uint8_t *rom, size_t size);
// Put a row's old bytes back; nothing is written unless every edit is in.
bool UndoGbCartPatch(const GbCartPatch &p, uint8_t *rom, size_t size);

} // namespace SGB

#endif
