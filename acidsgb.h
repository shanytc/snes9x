/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _ACIDSGB_H_
#define _ACIDSGB_H_

// Acid Tests SGB child: one Super Game Boy test on the real SGB BIOS, run in a
// process of its own because the SNES side is process-wide. Loads the ROM
// through Memory.LoadROM exactly as File -> Load Game does and writes every
// frame's GB screen to stdout for sgb/acid.cpp to judge.
//
//   <exe> -acidsgbchild <1|2> <rom> <sgb bios> <gb boot rom|-> <nrx 0|1> <frames>

#include "port.h"

// Set while this process is a child; the front end's display, pacing, sound
// and message hooks go quiet.
extern bool8 S9xAcidSgbChild;

// argv[1] must be AcidTests::kSgbChildFlag. Returns the process exit code.
int S9xAcidSgbChildMain(int argc, char **argv);

#endif
