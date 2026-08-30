/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// File -> BIOS Manager: one row per supported BIOS, plus the default boot mode
// for Game Boy content. Built in code rather than the .ui since it is a plain
// generated table.

#include "gtk_bios_manager.h"
#include "gtk_s9x.h"
#include "gtk_config.h"

#include <gtkmm.h>
#include <string>
#include <vector>

#include "snes9x.h"
#include "memmap.h"
#include "biosmanager.h"

namespace {

struct Row
{
    Gtk::Entry *entry;
    Gtk::Label *status;
};

// Green OK / amber size warning / red missing, matching the Qt dialog.
void refresh_row(int slot, Row &row)
{
    const std::string text = row.entry->get_text();
    if (text.empty())
    {
        row.status->set_markup("");
        return;
    }

    FILE *f = fopen(text.c_str(), "rb");
    if (!f)
    {
        row.status->set_markup("<span foreground='#c0392b'>not found</span>");
        return;
    }
    fclose(f);

    char saved[S9X_BIOS_PATH_MAX];
    snprintf(saved, sizeof saved, "%s", S9xGetBiosPath(slot));
    S9xSetBiosPath(slot, text.c_str());
    std::string why;
    const S9xBiosPathStatus st = S9xCheckBiosPath(slot, &why);
    S9xSetBiosPath(slot, saved);

    // The reason is the useful half: which size the slot wanted, or what the
    // file turned out to be. Falls back to the status when there is none.
    if (st == S9X_BIOS_PATH_OK)
        row.status->set_markup("<span foreground='#27ae60'>OK</span>");
    else if (st == S9X_BIOS_PATH_MISSING)
        row.status->set_markup("<span foreground='#c0392b'>not found</span>");
    else
        row.status->set_markup("<span foreground='#c0392b'>" +
                               Glib::Markup::escape_text(why.empty() ? "unusable" : why) +
                               "</span>");
}

} // namespace

void S9xGtkBiosManagerDialog(Gtk::Window *parent)
{
    Gtk::Dialog dialog(_("BIOS Manager"), true);
    if (parent)
        dialog.set_transient_for(*parent);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_OK"), Gtk::RESPONSE_OK);

    auto *content = dialog.get_content_area();
    content->set_spacing(8);
    content->set_border_width(10);

    auto *intro = Gtk::manage(new Gtk::Label());
    intro->set_text(_("Point each entry at its BIOS file. Anything left blank falls back "
                      "to the usual search by filename in the BIOS folder."));
    intro->set_line_wrap(true);
    intro->set_max_width_chars(70);
    intro->set_xalign(0.0f);
    content->pack_start(*intro, Gtk::PACK_SHRINK);

    auto *grid = Gtk::manage(new Gtk::Grid());
    grid->set_row_spacing(4);
    grid->set_column_spacing(6);
    content->pack_start(*grid, Gtk::PACK_SHRINK);

    std::vector<Row> rows(S9X_NUM_BIOS_SLOTS);

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        const auto *info = S9xGetBiosSlotInfo(slot);

        auto *label = Gtk::manage(new Gtk::Label(info->label));
        label->set_xalign(0.0f);

        auto *entry = Gtk::manage(new Gtk::Entry());
        entry->set_text(S9xGetBiosPath(slot));
        entry->set_placeholder_text(info->filename);
        entry->set_width_chars(44);
        entry->set_hexpand(true);

        auto *select = Gtk::manage(new Gtk::Button(_("Select…")));
        auto *clear = Gtk::manage(new Gtk::Button("✕"));
        clear->set_tooltip_text(_("Clear"));

        auto *status = Gtk::manage(new Gtk::Label());
        status->set_xalign(0.0f);

        grid->attach(*label,  0, slot, 1, 1);
        grid->attach(*entry,  1, slot, 1, 1);
        grid->attach(*select, 2, slot, 1, 1);
        grid->attach(*clear,  3, slot, 1, 1);
        grid->attach(*status, 4, slot, 1, 1);

        rows[slot] = { entry, status };

        select->signal_clicked().connect([&dialog, &rows, slot, info] {
            Gtk::FileChooserDialog chooser(dialog, _("Select BIOS File"),
                                           Gtk::FILE_CHOOSER_ACTION_OPEN);
            chooser.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
            chooser.add_button(_("_Open"), Gtk::RESPONSE_ACCEPT);

            const std::string current = rows[slot].entry->get_text();
            if (!current.empty())
                chooser.set_filename(current);
            else
                chooser.set_current_folder(S9xGetDirectory(BIOS_DIR));

            auto filter = Gtk::FileFilter::create();
            filter->set_name(_("BIOS files"));
            for (const char *pat : { "*.zip", "*.bin", "*.BIN", "*.rom", "*.sfc", "*.gb", "*.gbc" })
                filter->add_pattern(pat);
            chooser.add_filter(filter);
            auto all = Gtk::FileFilter::create();
            all->set_name(_("All files"));
            all->add_pattern("*");
            chooser.add_filter(all);

            if (chooser.run() == Gtk::RESPONSE_ACCEPT)
                rows[slot].entry->set_text(chooser.get_filename());
        });

        clear->signal_clicked().connect([&rows, slot] { rows[slot].entry->set_text(""); });
        entry->signal_changed().connect([&rows, slot] { refresh_row(slot, rows[slot]); });
    }

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
        refresh_row(slot, rows[slot]);

    dialog.show_all();
    if (dialog.run() != Gtk::RESPONSE_OK)
        return;

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
        S9xSetBiosPath(slot, rows[slot].entry->get_text().c_str());

    gui_config->save_config_file();

    // The running cart keeps the BIOS it was loaded against - these paths are
    // only read at load time. What changes is which Game Boy Model entries are
    // selectable, and this menu is only rebuilt here and on a load, so it has
    // to be asked for explicitly.
    top_level->configure_widgets();
}
