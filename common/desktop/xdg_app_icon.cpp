/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "xdg_app_icon.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace fs = std::filesystem;

namespace
{

const char *generated_marker = "X-SuperSnes9x-Generated=true";

std::string data_home()
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/')
        return xdg;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return "";
    return std::string(home) + "/.local/share";
}

std::string hicolor_dir()
{
    std::string dh = data_home();
    return dh.empty() ? "" : dh + "/icons/hicolor";
}

/* System-wide data directories, searched after the user's own. */
std::vector<std::string> data_dirs()
{
    const char *xdg = getenv("XDG_DATA_DIRS");
    std::string list = (xdg && xdg[0]) ? xdg : "/usr/local/share:/usr/share";
    std::vector<std::string> dirs;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find(':', start);
        if (end == std::string::npos)
            end = list.size();
        if (end > start)
            dirs.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    return dirs;
}

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

bool ends_with_nocase(const std::string &s, const std::string &suffix)
{
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); i++)
        if (tolower((unsigned char)s[s.size() - suffix.size() + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

/* Icon names and absolute icon paths used by the desktop entries that launch
 * the running AppImage. Empty when not running from one, or when nothing has
 * registered it. */
struct AppImageIcons
{
    std::vector<std::string> names;
    std::vector<std::string> paths;
    bool any_entry = false;
};

AppImageIcons appimage_icons()
{
    AppImageIcons out;
    const char *appimage = getenv("APPIMAGE");
    if (!appimage || !appimage[0])
        return out;
    std::string dh = data_home();
    if (dh.empty())
        return out;

    try
    {
        for (auto &entry : fs::directory_iterator(fs::path(dh) / "applications"))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".desktop")
                continue;

            std::ifstream in(entry.path());
            std::string line, icon;
            bool in_main = false, launches = false, generated = false;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line[0] == '[')
                {
                    in_main = (line == "[Desktop Entry]");
                    continue;
                }
                if (!in_main)
                    continue;
                if (line == generated_marker)
                    generated = true;
                size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string key = trim(line.substr(0, eq));
                std::string val = trim(line.substr(eq + 1));
                if (key == "Exec" || key == "TryExec")
                    launches = launches || val.find(appimage) != std::string::npos;
                else if (key == "Icon")
                    icon = val;
            }
            // Our own generated entry uses the program's icon name already.
            if (!launches || icon.empty() || generated)
                continue;
            out.any_entry = true;
            if (icon[0] == '/')
                out.paths.push_back(icon);
            else
                out.names.push_back(icon);
        }
    }
    catch (const fs::filesystem_error &)
    {
        // No applications dir, or unreadable: nothing to override.
    }
    return out;
}

std::string base64(const std::vector<uint8_t> &in)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == in.size())
    {
        uint32_t v = in[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == in.size())
    {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

/* The scalable/ directory is consulted for sizes the fixed ones do not cover,
 * so it has to carry the chosen logo as well: an SVG that just embeds the
 * largest PNG. */
std::string svg_wrapper(const XdgAppIcon::Image &img)
{
    std::string s = std::to_string(img.size);
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
           "width=\"" + s + "\" height=\"" + s + "\" viewBox=\"0 0 " + s + " " + s + "\">\n"
           "  <image width=\"" + s + "\" height=\"" + s + "\" xlink:href=\"data:image/png;base64," +
           base64(img.png) + "\"/>\n"
           "</svg>\n";
}

fs::path backup_path(const fs::path &path)
{
    fs::path bak = path;
    bak += ".s9xbak";
    return bak;
}

/* Write `path` atomically. With `keep_backup`, whatever was there is kept as
 * <path>.s9xbak the first time round so restore_file() can bring it back. */
bool write_file(const fs::path &path, const void *data, size_t len, bool keep_backup, std::string &error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "couldn't create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }

    fs::path bak = backup_path(path);
    if (keep_backup && fs::exists(path, ec) && !fs::exists(bak, ec))
    {
        fs::rename(path, bak, ec);
        if (ec)
        {
            error = "couldn't back up " + path.string() + ": " + ec.message();
            return false;
        }
    }

    fs::path tmp = path;
    tmp += ".s9xtmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
    {
        error = "couldn't write " + tmp.string() + ": " + strerror(errno);
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
    {
        error = "couldn't write " + tmp.string() + ": " + strerror(errno);
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    if (ec)
    {
        error = "couldn't replace " + path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/* Put back the backup if there is one, otherwise the file is ours: drop it.
 * With `only_backup` a file without a backup is left alone. */
bool restore_file(const fs::path &path, bool only_backup, std::string &error)
{
    std::error_code ec;
    fs::path bak = backup_path(path);
    if (fs::exists(bak, ec))
    {
        fs::rename(bak, path, ec);
        if (ec)
        {
            error = "couldn't restore " + path.string() + ": " + ec.message();
            return false;
        }
        return true;
    }
    if (!only_backup && fs::exists(path, ec))
    {
        fs::remove(path, ec);
        if (ec)
        {
            error = "couldn't remove " + path.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

/* GTK's icon cache is only rescanned when the theme directories' mtimes move,
 * and a rewritten file inside an existing directory does not move them. */
void touch_dirs(const std::vector<fs::path> &dirs)
{
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (const auto &d : dirs)
    {
        fs::last_write_time(d, now, ec);
        fs::last_write_time(d.parent_path(), now, ec);
    }
    std::string hicolor = hicolor_dir();
    if (!hicolor.empty())
    {
        fs::last_write_time(hicolor, now, ec);
        fs::last_write_time(fs::path(hicolor).parent_path(), now, ec);
    }
}

std::vector<std::string> icon_names(const std::string &icon_name, const AppImageIcons &appimage)
{
    std::vector<std::string> names{ icon_name };
    for (const auto &n : appimage.names)
        if (n != icon_name)
            names.push_back(n);
    return names;
}

/* ---- the desktop entry for an uninstalled program ---------------------- */

fs::path user_entry_path(const XdgAppIcon::DesktopEntry &entry)
{
    std::string dh = data_home();
    return dh.empty() ? fs::path() : fs::path(dh) / "applications" / (entry.id + ".desktop");
}

bool is_generated_entry(const fs::path &path)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
        if (trim(line) == generated_marker)
            return true;
    return false;
}

/* True when a desktop entry for the program already exists that is not the
 * one Install() writes: the packaged one in a system data dir, or one the
 * user put in place themselves. */
bool foreign_entry_installed(const XdgAppIcon::DesktopEntry &entry)
{
    std::error_code ec;
    fs::path mine = user_entry_path(entry);
    if (!mine.empty() && fs::exists(mine, ec) && !is_generated_entry(mine))
        return true;
    for (const auto &d : data_dirs())
        if (fs::exists(fs::path(d) / "applications" / (entry.id + ".desktop"), ec))
            return true;
    return false;
}

/* The AppImage file when running from one (the mounted binary inside it is
 * gone once the program exits), otherwise the executable itself. */
std::string running_executable()
{
    const char *appimage = getenv("APPIMAGE");
    if (appimage && appimage[0] == '/')
        return appimage;
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return "";
    return exe.string();
}

/* One Exec= argument per the Desktop Entry spec: double-quoted, with " ` $ \
 * escaped by a backslash, and every backslash doubled again because the value
 * goes through the general string unescape first. */
std::string exec_quote(const std::string &arg)
{
    std::string out = "\"";
    for (char c : arg)
    {
        if (c == '\\')
            out += "\\\\\\\\";
        else if (c == '"' || c == '`' || c == '$')
            out += std::string("\\\\") + c;
        else
            out += c;
    }
    return out + "\"";
}

bool write_desktop_entry(const XdgAppIcon::DesktopEntry &entry, const std::string &icon_name, std::string &error)
{
    fs::path path = user_entry_path(entry);
    std::string exe = running_executable();
    if (path.empty() || exe.empty())
        return true; // nowhere to write, or nothing to point at: the icons still went in

    std::string text =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=" + entry.name + "\n" +
        (entry.comment.empty() ? "" : "Comment=" + entry.comment + "\n") +
        "Exec=" + exec_quote(exe) + " %F\n"
        "Icon=" + icon_name + "\n" +
        (entry.categories.empty() ? "" : "Categories=" + entry.categories + "\n") +
        (entry.mime_types.empty() ? "" : "MimeType=" + entry.mime_types + "\n") +
        "StartupWMClass=" + entry.id + "\n" +
        generated_marker + "\n";
    return write_file(path, text.data(), text.size(), false, error);
}

bool remove_generated_entry(const XdgAppIcon::DesktopEntry &entry, std::string &error)
{
    std::error_code ec;
    fs::path path = user_entry_path(entry);
    if (path.empty() || !fs::exists(path, ec) || !is_generated_entry(path))
        return true;
    fs::remove(path, ec);
    if (ec)
    {
        error = "couldn't remove " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

/* ---- the executable's own icon in the file manager --------------------- */

/* Run a program with no shell and no output; true when it exits 0. */
bool run_quiet(std::vector<std::string> args)
{
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0)
        return false;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* GIO-based file managers (Nautilus, Nemo, Caja) draw a file with the icon
 * named in its metadata::custom-icon-name attribute. Best effort: it needs
 * the gio tool and the gvfs metadata daemon, and the tag lives in the user's
 * profile rather than in the file. */
void tag_executable_icon(const std::string &icon_name)
{
    std::string exe = running_executable();
    if (!exe.empty())
        run_quiet({ "gio", "set", exe, "metadata::custom-icon-name", icon_name });
}

void untag_executable_icon()
{
    std::string exe = running_executable();
    if (!exe.empty())
        run_quiet({ "gio", "set", "-t", "unset", exe, "metadata::custom-icon-name" });
}

} // namespace

namespace XdgAppIcon
{

bool Install(const std::string &icon_name, const std::vector<Image> &images,
             const DesktopEntry &entry, std::string &error)
{
    error.clear();
    if (images.empty())
    {
        error = "no icon images to install";
        return false;
    }
    std::string hicolor = hicolor_dir();
    if (hicolor.empty())
    {
        error = "neither XDG_DATA_HOME nor HOME is set";
        return false;
    }

    const Image *largest = &images[0];
    for (const auto &img : images)
        if (img.size > largest->size)
            largest = &img;
    std::string svg = svg_wrapper(*largest);

    AppImageIcons appimage = appimage_icons();
    std::vector<fs::path> touched;

    for (const auto &name : icon_names(icon_name, appimage))
    {
        for (const auto &img : images)
        {
            std::string sz = std::to_string(img.size);
            fs::path dir = fs::path(hicolor) / (sz + "x" + sz) / "apps";
            if (!write_file(dir / (name + ".png"), img.png.data(), img.png.size(), true, error))
                return false;
            touched.push_back(dir);
        }
        fs::path dir = fs::path(hicolor) / "scalable" / "apps";
        if (!write_file(dir / (name + ".svg"), svg.data(), svg.size(), true, error))
            return false;
        touched.push_back(dir);
    }

    // Icon files the AppImage integration tool keeps outside the theme.
    for (const auto &path : appimage.paths)
    {
        bool ok = ends_with_nocase(path, ".svg")
                      ? write_file(path, svg.data(), svg.size(), true, error)
                      : write_file(path, largest->png.data(), largest->png.size(), true, error);
        if (!ok)
            return false;
        touched.push_back(fs::path(path).parent_path());
    }

    touch_dirs(touched);
    tag_executable_icon(icon_name);

    // Without any entry the desktop has nothing to attach the icon to.
    if (!appimage.any_entry && !foreign_entry_installed(entry))
        return write_desktop_entry(entry, icon_name, error);
    return true;
}

bool Restore(const std::string &icon_name, const DesktopEntry &entry, std::string &error)
{
    error.clear();
    std::string hicolor = hicolor_dir();
    if (hicolor.empty())
        return true;

    AppImageIcons appimage = appimage_icons();
    std::vector<fs::path> touched;
    bool ok = true;

    try
    {
        std::error_code ec;
        if (fs::is_directory(hicolor, ec))
        {
            for (auto &dir : fs::directory_iterator(hicolor))
            {
                if (!dir.is_directory())
                    continue;
                fs::path apps = dir.path() / "apps";
                for (const auto &name : icon_names(icon_name, appimage))
                {
                    for (const char *ext : { ".png", ".svg" })
                    {
                        fs::path p = apps / (name + ext);
                        if (!fs::exists(p, ec) && !fs::exists(backup_path(p), ec))
                            continue;
                        if (!restore_file(p, false, error))
                            ok = false;
                        touched.push_back(apps);
                    }
                }
            }
        }
    }
    catch (const fs::filesystem_error &e)
    {
        error = e.what();
        ok = false;
    }

    for (const auto &path : appimage.paths)
    {
        if (!restore_file(path, true, error))
            ok = false;
        touched.push_back(fs::path(path).parent_path());
    }

    touch_dirs(touched);
    untag_executable_icon();

    if (!remove_generated_entry(entry, error))
        ok = false;
    return ok;
}

} // namespace XdgAppIcon
