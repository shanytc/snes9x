/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* The Linux counterpart of win32's "write the chosen logo into the .exe".
 *
 * An ELF binary carries no icon; what launchers, docks and task bars show for
 * the program is the Icon= key of its .desktop entry, resolved through the
 * icon theme (modern GNOME shows nothing else, not even the window's own
 * icon). The user's icon directory is searched before the system one, so
 * dropping the chosen artwork into
 *
 *   $XDG_DATA_HOME/icons/hicolor/<size>x<size>/apps/<icon_name>.png
 *   $XDG_DATA_HOME/icons/hicolor/scalable/apps/<icon_name>.svg
 *
 * overrides the installed icon without touching anything outside $HOME.
 *
 * A program run straight from its build directory, or from an AppImage that
 * no integration tool has registered, has no desktop entry at all and gets a
 * generic icon whatever the theme says. Install() therefore also writes a
 * user-level entry for the running executable when none exists anywhere,
 * marked so Restore() removes only what it created.
 *
 * When the program runs from an AppImage ($APPIMAGE set) that a tool such as
 * AppImageLauncher or Gear Lever did register, the entries in
 * $XDG_DATA_HOME/applications whose Exec= points at the AppImage are found
 * and their icon (own name or absolute path) is replaced too.
 *
 * The executable file itself (or the AppImage) shows the generic MIME icon
 * in file managers, since an ELF binary carries no icon. Nautilus, Nemo and
 * Caja do keep a per-file icon in GIO metadata, so Install() tags the running
 * program with the icon name through the `gio` tool when it is available,
 * which is the nearest thing to the icon inside a Windows .exe.
 *
 * Every file replaced is kept next to the new one as <file>.s9xbak so
 * Restore() can put things back exactly; files that did not exist before are
 * simply removed again. Nothing here needs a restart: the icon directories
 * are touched so the toolkits' icon caches notice the change. */
namespace XdgAppIcon
{

struct Image
{
    int size;                  // width == height in pixels
    std::vector<uint8_t> png;  // encoded PNG bytes
};

/* What the port's shipped .desktop file says, for the entry written when the
 * program is not installed. `id` is the file name without ".desktop" and must
 * equal the window class / Wayland app-id so the desktop matches the running
 * window to it. */
struct DesktopEntry
{
    std::string id;          // "super-snes9x-qt"
    std::string name;        // "Super Snes9x"
    std::string comment;     // "A Super Nintendo emulator"
    std::string categories;  // "Game;Emulator;"
    std::string mime_types;  // "application/vnd.nintendo.snes.rom;..."
};

/* Install `images` as the launcher icon named `icon_name` (the Icon= value of
 * the program's own .desktop entry), one image per size; the largest one is
 * also wrapped into the scalable SVG. Writes a desktop entry for the running
 * executable if none is installed. Returns false and fills `error` when a
 * file could not be written. */
bool Install(const std::string &icon_name, const std::vector<Image> &images,
             const DesktopEntry &entry, std::string &error);

/* Undo Install(): put back every replaced file, delete the ones that were
 * created, and drop the desktop entry if Install() wrote it. Safe to call when
 * nothing was installed. */
bool Restore(const std::string &icon_name, const DesktopEntry &entry, std::string &error);

} // namespace XdgAppIcon
