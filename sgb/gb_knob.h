/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_KNOB_H_
#define _SGB_GB_KNOB_H_

#include <cstdlib>

namespace SGB {

// Reads an ACID_* timing override once, for the accuracy knobs the GB core
// is tuned with. Always call it through a function-local
//
//     static const int x = AcidKnob("ACID_X", default);
//
// so C++ guarantees a single, thread-safe initialisation. The older form
//
//     static int x = -1;
//     if (x < 0) { x = getenv(...); y = getenv(...); }
//
// was wrong on two counts once several cores ran tests at the same time: a
// second core could see the guard already written while the rest of the
// block was still filling in, and it read back a sentinel; and a knob whose
// default was itself negative never stopped satisfying its own guard, so it
// called getenv on every single call.
inline int AcidKnob(const char *name, int def)
{
	const char *e = getenv(name);
	return e ? atoi(e) : def;
}

} // namespace SGB

#endif
