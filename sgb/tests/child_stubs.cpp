/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// acid_sgb_child: the Acid Tests SGB child built headless for acid_test's
// --sgb1-bios/--sgb2-bios runs. The whole SNES core plus this port layer;
// every front-end hook is a no-op because the child only streams GB frames.

#include "snes9x.h"
#include "memmap.h"
#include "display.h"
#include "controls.h"
#include "conffile.h"
#include "fscompat.h"
#include "stream.h"
#include "acidsgb.h"

#include <cstdlib>
#include <string>

void S9xMessage(int, int, const char *msg) { S9xSetInfoString(msg); }
void S9xExit(void) { exit(0); }
bool8 S9xInitUpdate(void) { return TRUE; }
bool8 S9xDeinitUpdate(int, int) { return TRUE; }
bool8 S9xContinueUpdate(int, int) { return TRUE; }
void S9xSyncSpeed(void) {}
bool8 S9xOpenSoundDevice(void) { return TRUE; }
void S9xToggleSoundChannel(int) {}
void S9xAutoSaveSRAM(void) {}
void S9xExtraUsage(void) {}
void S9xParseArg(char **, int &, int) {}
void S9xParsePortConfig(ConfigFile &, int) {}
bool S9xPollButton(uint32, bool *) { return false; }
bool S9xPollAxis(uint32, int16 *) { return false; }
bool S9xPollPointer(uint32, int16 *, int16 *) { return false; }
void S9xHandlePortCommand(s9xcommand_t, int16, int16) {}
const char *S9xStringInput(const char *) { return NULL; }
std::string S9xGetDirectory(enum s9x_getdirtype) { return "."; }
std::string S9xGetFilenameInc(std::string ex, enum s9x_getdirtype) { return "acid_sgb_child" + ex; }
bool8 S9xOpenSnapshotFile(const char *, bool8, STREAM *) { return FALSE; }
void S9xCloseSnapshotFile(STREAM) {}

int main(int argc, char **argv)
{
	return S9xAcidSgbChildMain(argc, argv);
}
