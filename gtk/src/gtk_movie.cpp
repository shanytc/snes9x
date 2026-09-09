/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_movie.h"
#include "gtk_compat.h"
#include "gtk_s9x.h"
#include "gtk_s9xwindow.h"
#include "gtk_config.h"

#include "fmt/format.h"
#include <array>
#include <filesystem>

#include "snes9x.h"
#include "memmap.h"
#include "movie.h"
#include "snapshot.h"
#include "fscompat.h"

namespace
{
constexpr int kMovieJoypads = 8;

/* <ROM name>.smv in the export folder, where both dialogs start. */
std::string default_movie_path()
{
    if (Memory.ROMFilename.empty())
        return {};
    return S9xGetFilename(".smv", SCREENSHOT_DIR);
}

std::string rom_description(uint32_t crc32, const char *name)
{
    std::string rom_name(name);
    while (!rom_name.empty() && rom_name.back() == ' ')
        rom_name.pop_back();
    return fmt::format("crc32={:08X}, name={}", crc32, rom_name);
}

Glib::ustring metadata_to_ustring(const wchar_t *metadata)
{
    Glib::ustring text;
    for (; *metadata; metadata++)
        text.push_back((gunichar)*metadata);
    return text;
}

std::wstring ustring_to_metadata(const Glib::ustring &text)
{
    std::wstring metadata;
    for (gunichar c : text)
        metadata.push_back((wchar_t)c);
    return metadata;
}

Gtk::Frame *make_joypad_frame(std::array<Gtk::CheckButton *, kMovieJoypads> &boxes, bool sensitive)
{
    auto frame = Gtk::manage(new Gtk::Frame(_("Record Controllers")));
    auto grid = Gtk::manage(new Gtk::Grid);
    grid->set_border_width(5);
    grid->set_column_spacing(10);
    grid->set_row_spacing(2);
    for (int i = 0; i < kMovieJoypads; i++)
    {
        boxes[i] = Gtk::manage(new Gtk::CheckButton(fmt::format(fmt::runtime(_("Joypad {}")), i + 1)));
        boxes[i]->set_sensitive(sensitive);
        grid->attach(*boxes[i], i % 4, i / 4, 1, 1);
    }
    frame->add(*grid);
    return frame;
}

std::string browse_for_movie(Gtk::Window &parent, const std::string &current, bool save)
{
    Gtk::FileChooserDialog dialog(parent, save ? _("Record Movie") : _("Open Movie"),
                                  save ? Gtk::FILE_CHOOSER_ACTION_SAVE : Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(save ? _("_Save") : _("_Open"), Gtk::RESPONSE_ACCEPT);

    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("SuperSnes9x Movie Files"));
    filter->add_pattern("*.smv");
    filter->add_pattern("*.SMV");
    dialog.add_filter(filter);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("All Files"));
    all->add_pattern("*");
    dialog.add_filter(all);

    std::filesystem::path path(current);
    if (!current.empty() && std::filesystem::is_directory(path.parent_path()))
        dialog.set_current_folder(path.parent_path().string());
    else
        dialog.set_current_folder(S9xGetDirectory(SCREENSHOT_DIR));
    if (save)
    {
        dialog.set_do_overwrite_confirmation(true);
        if (!path.filename().empty())
            dialog.set_current_name(path.filename().string());
    }

    if (dialog.run() != Gtk::RESPONSE_ACCEPT)
        return {};
    return dialog.get_filename();
}
} // namespace

std::string S9xMovieErrorString(int result, bool brief)
{
    switch (result)
    {
    case FILE_NOT_FOUND:
        return brief ? _("File not found.")
                     : _("The movie file was not found or could not be opened.");
    case WRONG_FORMAT:
        return brief ? _("Unrecognized format.")
                     : _("The movie file is corrupt or in the wrong format.");
    case WRONG_VERSION:
        return brief ? _("Unsupported movie version.")
                     : _("Unsupported movie version. You need a different version of SuperSnes9x to play this movie.");
    default:
        return _("Could not open movie file.");
    }
}

bool S9xPlayMovieDialog(MoviePlayChoice &choice)
{
    Gtk::Dialog dialog(_("Play Movie"), *top_level->window.get(), true);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    auto ok_button = dialog.add_button(_("_OK"), Gtk::RESPONSE_ACCEPT);
    dialog.set_default_response(Gtk::RESPONSE_ACCEPT);

    auto vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    vbox->set_border_width(8);

    auto path_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    path_box->pack_start(*Gtk::manage(new Gtk::Label(_("Movie File:"))), false, false);
    Gtk::Entry path_entry;
    path_entry.set_width_chars(45);
    path_entry.set_activates_default(true);
    path_box->pack_start(path_entry, true, true);
    Gtk::Button browse_button(_("_Browse…"), true);
    path_box->pack_start(browse_button, false, false);
    vbox->pack_start(*path_box, false, false);

    Gtk::CheckButton read_only_box(_("Open Read-Only"));
    read_only_box.set_active(gui_config->movie_default_read_only);
    vbox->pack_start(read_only_box, false, false);

    Gtk::Label movie_rom_label, current_rom_label;
    movie_rom_label.set_xalign(0.0);
    current_rom_label.set_xalign(0.0);
    vbox->pack_start(movie_rom_label, false, false);
    vbox->pack_start(current_rom_label, false, false);

    auto middle = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));

    auto info_grid = Gtk::manage(new Gtk::Grid);
    info_grid->set_column_spacing(6);
    info_grid->set_row_spacing(2);
    Gtk::Label date_label, length_label, frames_label, rerecord_label;
    const std::pair<const char *, Gtk::Label *> rows[] = {
        { _("Recording Date:"), &date_label },
        { _("Length:"), &length_label },
        { _("Frames:"), &frames_label },
        { _("Re-record Count:"), &rerecord_label },
    };
    int row = 0;
    for (auto &[name, label] : rows)
    {
        auto title = Gtk::manage(new Gtk::Label(name));
        title->set_xalign(1.0);
        label->set_xalign(0.0);
        label->set_width_chars(24);
        info_grid->attach(*title, 0, row, 1, 1);
        info_grid->attach(*label, 1, row, 1, 1);
        row++;
    }
    middle->pack_start(*info_grid, true, true);

    // How the movie was recorded: shown for information, as on win32.
    auto options_frame = Gtk::manage(new Gtk::Frame(_("Record Options")));
    auto options_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    options_box->set_border_width(5);
    Gtk::RadioButtonGroup start_group;
    Gtk::RadioButton from_reset_radio(start_group, _("Record from reset"));
    Gtk::RadioButton from_now_radio(start_group, _("Record from now"));
    from_reset_radio.set_sensitive(false);
    from_now_radio.set_sensitive(false);
    options_box->pack_start(from_reset_radio, false, false);
    options_box->pack_start(from_now_radio, false, false);
    options_frame->add(*options_box);
    middle->pack_start(*options_frame, false, false);
    vbox->pack_start(*middle, false, false);

    std::array<Gtk::CheckButton *, kMovieJoypads> joypad_boxes{};
    vbox->pack_start(*make_joypad_frame(joypad_boxes, false), false, false);

    auto info_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label info_title_label(_("Author Info:"));
    info_title_label.set_xalign(1.0);
    info_title_label.set_yalign(0.0);
    Gtk::Label info_label;
    info_label.set_xalign(0.0);
    info_label.set_yalign(0.0);
    info_label.set_line_wrap(true);
    info_label.set_selectable(true);
    Gtk::Frame info_frame;
    info_frame.add(info_label);
    info_frame.set_size_request(-1, 48);
    info_box->pack_start(info_title_label, false, false);
    info_box->pack_start(info_frame, true, true);
    vbox->pack_start(*info_box, false, false);

    Gtk::Label warning_label;
    warning_label.set_xalign(0.0);
    vbox->pack_start(warning_label, false, false);

    dialog.get_content_area()->pack_start(*vbox, true, true);

    auto refresh = [&] {
        MovieInfo info{};
        int result = FILE_NOT_FOUND;
        std::string path = path_entry.get_text();
        if (!path.empty())
            result = S9xMovieGetInfo(path.c_str(), &info);

        if (result != FILE_NOT_FOUND)
        {
            auto created = Glib::DateTime::create_now_local((gint64)info.TimeCreated);
            date_label.set_text(created ? created.format("%c") : Glib::ustring());

            uint32_t fps = Memory.ROMFramesPerSecond ? Memory.ROMFramesPerSecond : 60;
            uint32_t seconds = (info.LengthFrames + fps / 2) / fps;
            length_label.set_text(fmt::format("{:02d}:{:02d}:{:02d}", seconds / 3600, (seconds / 60) % 60, seconds % 60));
            frames_label.set_text(std::to_string(info.LengthFrames));
            rerecord_label.set_text(std::to_string(info.RerecordCount));
        }
        else
        {
            date_label.set_text("");
            length_label.set_text("");
            frames_label.set_text("");
            rerecord_label.set_text("");
        }

        current_rom_label.set_text(fmt::format(fmt::runtime(_("Current ROM: {}")),
                                               rom_description(Memory.ROMCRC32, Memory.ROMName)));

        if (result == SUCCESS)
        {
            info_title_label.set_text(_("Author Info:"));
            info_label.set_text(metadata_to_ustring(info.Metadata));

            // A movie that was saved read-only cannot be recorded into.
            read_only_box.set_sensitive(!info.ReadOnly);
            if (info.ReadOnly)
                read_only_box.set_active(true);

            for (int i = 0; i < kMovieJoypads; i++)
                joypad_boxes[i]->set_active(info.ControllersMask & (1 << i));
            if (info.Opts & MOVIE_OPT_FROM_RESET)
                from_reset_radio.set_active(true);
            else
                from_now_radio.set_active(true);

            const bool has_rom_info = info.SyncFlags & MOVIE_SYNC_HASROMINFO;
            if (has_rom_info)
                movie_rom_label.set_text(fmt::format(fmt::runtime(_("Movie's ROM: {}")),
                                                     rom_description(info.ROMCRC32, info.ROMName)));
            else
                movie_rom_label.set_text(_("Movie's ROM: (not stored in movie file)"));

            const bool mismatch = has_rom_info && info.ROMCRC32 != Memory.ROMCRC32;
            if (mismatch)
            {
                current_rom_label.set_text(current_rom_label.get_text() + _(" <-- MISMATCH !!!"));
                warning_label.set_text(_("WARNING: You don't have the right ROM loaded!"));
            }
            else
                warning_label.set_text(_("Press OK to start playing the movie."));

            ok_button->set_sensitive(true);
        }
        else
        {
            info_title_label.set_text(_("Error Info:"));
            info_label.set_text(path.empty() ? "" : S9xMovieErrorString(result, false));
            warning_label.set_text(path.empty() ? "" : S9xMovieErrorString(result, true));

            read_only_box.set_sensitive(false);
            for (auto box : joypad_boxes)
                box->set_active(false);

            // Show where the movie was looked for, as win32 does.
            std::error_code ec;
            auto folder = std::filesystem::absolute(path, ec).parent_path().string();
            movie_rom_label.set_text(path.empty() ? std::string()
                                                  : fmt::format(fmt::runtime(_("Path: {}")), folder));

            ok_button->set_sensitive(false);
        }
    };

    browse_button.signal_clicked().connect([&] {
        auto filename = browse_for_movie(dialog, path_entry.get_text(), false);
        if (!filename.empty())
            path_entry.set_text(filename);
    });
    path_entry.signal_changed().connect(refresh);

    path_entry.set_text(default_movie_path());
    refresh();

    dialog.show_all();
    if (dialog.run() != Gtk::RESPONSE_ACCEPT)
        return false;

    choice.path = path_entry.get_text();
    choice.read_only = read_only_box.get_active();
    gui_config->movie_default_read_only = choice.read_only;
    return !choice.path.empty();
}

bool S9xRecordMovieDialog(bool sram_exists, MovieRecordChoice &choice)
{
    Gtk::Dialog dialog(_("Record Movie"), *top_level->window.get(), true);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_OK"), Gtk::RESPONSE_ACCEPT);
    dialog.set_default_response(Gtk::RESPONSE_ACCEPT);

    auto vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    vbox->set_border_width(8);

    auto grid = Gtk::manage(new Gtk::Grid);
    grid->set_column_spacing(6);
    grid->set_row_spacing(4);
    auto path_label = Gtk::manage(new Gtk::Label(_("Movie File:")));
    path_label->set_xalign(1.0);
    Gtk::Entry path_entry;
    path_entry.set_width_chars(45);
    path_entry.set_hexpand(true);
    path_entry.set_activates_default(true);
    Gtk::Button browse_button(_("_Browse…"), true);
    grid->attach(*path_label, 0, 0, 1, 1);
    grid->attach(path_entry, 1, 0, 1, 1);
    grid->attach(browse_button, 2, 0, 1, 1);
    auto metadata_label = Gtk::manage(new Gtk::Label(_("Author Info:")));
    metadata_label->set_xalign(1.0);
    Gtk::Entry metadata_entry;
    metadata_entry.set_max_length(MOVIE_MAX_METADATA - 1);
    metadata_entry.set_activates_default(true);
    grid->attach(*metadata_label, 0, 1, 1, 1);
    grid->attach(metadata_entry, 1, 1, 2, 1);
    vbox->pack_start(*grid, false, false);

    auto middle = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));

    auto options_frame = Gtk::manage(new Gtk::Frame(_("Record Options")));
    auto options_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    options_box->set_border_width(5);
    Gtk::RadioButtonGroup start_group;
    Gtk::RadioButton from_reset_radio(start_group, _("Record from reset"));
    Gtk::RadioButton from_now_radio(start_group, _("Record from now"));
    Gtk::CheckButton clear_sram_box(_("Clear SRAM"));
    options_box->pack_start(from_reset_radio, false, false);
    options_box->pack_start(from_now_radio, false, false);
    options_box->pack_start(clear_sram_box, false, false);
    options_frame->add(*options_box);
    middle->pack_start(*options_frame, false, false);

    std::array<Gtk::CheckButton *, kMovieJoypads> joypad_boxes{};
    middle->pack_start(*make_joypad_frame(joypad_boxes, true), true, true);
    vbox->pack_start(*middle, false, false);

    dialog.get_content_area()->pack_start(*vbox, true, true);

    auto update_clear_sram = [&] {
        clear_sram_box.set_sensitive(sram_exists && from_reset_radio.get_active());
    };

    browse_button.signal_clicked().connect([&] {
        auto filename = browse_for_movie(dialog, path_entry.get_text(), true);
        if (!filename.empty())
            path_entry.set_text(filename);
    });
    from_reset_radio.signal_toggled().connect(update_clear_sram);

    path_entry.set_text(default_movie_path());
    joypad_boxes[0]->set_active(true);
    if (gui_config->movie_default_from_reset)
        from_reset_radio.set_active(true);
    else
        from_now_radio.set_active(true);
    // Without a battery save there is nothing to clear, so the box is shown
    // checked and disabled, as on win32.
    clear_sram_box.set_active(sram_exists ? gui_config->movie_default_clear_sram : true);
    update_clear_sram();

    dialog.show_all();

    while (true)
    {
        if (dialog.run() != Gtk::RESPONSE_ACCEPT)
            return false;

        uint8_t mask = 0;
        for (int i = 0; i < kMovieJoypads; i++)
            if (joypad_boxes[i]->get_active())
                mask |= 1 << i;

        std::string path = path_entry.get_text();
        const char *problem = nullptr;
        if (path.empty())
            problem = _("Choose a file name for the movie.");
        else if (mask == 0)
            problem = _("Select at least one controller to record.");
        if (problem)
        {
            Gtk::MessageDialog msg(dialog, problem, false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
            msg.run();
            continue;
        }

        choice.path = path;
        choice.metadata = ustring_to_metadata(metadata_entry.get_text());
        choice.controllers_mask = mask;
        choice.from_reset = from_reset_radio.get_active();
        choice.clear_sram = sram_exists && choice.from_reset && clear_sram_box.get_active();

        // Remember the choices for next time, like win32's MovieDefault* settings.
        gui_config->movie_default_from_reset = choice.from_reset;
        if (sram_exists && choice.from_reset)
            gui_config->movie_default_clear_sram = clear_sram_box.get_active();
        return true;
    }
}
