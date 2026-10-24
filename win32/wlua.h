/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef WLUA_H
#define WLUA_H

#include <windows.h>

// File > Script > Lua: the script editor. The engine (lua/) runs on this, the
// window thread, so the editor calls it directly.

// Engine callbacks and the timer that keeps scripts running while paused.
void WinLuaInit();
void WinShowLuaWindow();
// -lua <script>: remembered while parsing, started once the game is loaded.
void WinLuaAddCommandLineScript(const char *utf8_path);
void WinLuaRunCommandLineScripts();
// Asks about unsaved scripts; false when the user cancelled quitting.
bool WinLuaConfirmClose();
// On exit: remembers the open scripts, stops them and closes the editor.
void WinLuaShutdown();
// The editor's keyboard shortcuts; true when the message was handled.
bool WinLuaHandleMessage(MSG *msg);

#endif
