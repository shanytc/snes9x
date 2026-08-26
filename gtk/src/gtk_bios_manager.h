/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef __GTK_BIOS_MANAGER_H
#define __GTK_BIOS_MANAGER_H

#include <gtkmm/window.h>

// Runs the modal File -> BIOS Manager dialog and saves the config on OK.
void S9xGtkBiosManagerDialog(Gtk::Window *parent);

#endif
