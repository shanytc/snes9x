/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <string>

/* File > Script > Lua: the script editor. The engine (lua/) runs on this, the
 * GTK main thread, so the window calls it directly. */

// Hands the engine its pause, reset and debugger callbacks.
void S9xLuaInstallFrontend();
void S9xShowLuaWindow();
// Runs a script file from the command line, with or without the window.
void S9xRunLuaScriptFile(const std::string &path);
// Asks about unsaved scripts; false when the user cancelled quitting.
bool S9xLuaWindowConfirmClose();
// On exit: remembers the open scripts, stops them and closes the window.
void S9xLuaWindowShutdown();
// While open, leaving the main window for the editor doesn't pause the game.
bool S9xLuaWindowOpen();
