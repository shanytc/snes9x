/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// File -> BIOS Manager, laid out as win32's: a sidebar of families, and a card per BIOS for
// the sidebar's pick. A chip's card picks how it runs (Legacy, Native or Firmware) and opens
// its speed chart. Built in code rather than the .ui since it all comes from the core's tables.

#include "gtk_bios_manager.h"
#include "gtk_s9x.h"
#include "gtk_config.h"

#include <gtkmm.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "snes9x.h"
#include "memmap.h"
#include "biosmanager.h"

namespace {

// Sidebar item data: a slot, kHeading + family for a heading, or kGroup + group for a group.
constexpr int kHeading = S9X_NUM_BIOS_SLOTS;
constexpr int kGroup   = kHeading + S9X_BIOS_NUM_FAMILIES;

// A chip's modes, in its box's order and the speed chart's, each with its colour there.
const struct { const char *name; int mode; double r, g, b; } kModes[] = {
    { N_("Legacy (HLE)"), S9X_CHIP_HLE,      0x8E / 255.0, 0x9A / 255.0, 0xA6 / 255.0 },
    { N_("Native (LLE)"), S9X_CHIP_NATIVE,   0x2E / 255.0, 0x9E / 255.0, 0x4F / 255.0 },
    { N_("Firmware"),     S9X_CHIP_FIRMWARE, 0x2F / 255.0, 0x7E / 255.0, 0xD8 / 255.0 },
};

// Each family's theme icon, after the first that's missing.
const char *const kIcons[S9X_BIOS_NUM_FAMILIES][2] = {
    { "input-gaming", "applications-games" }, { "application-x-addon", "preferences-plugins" },
    { "cpu", "computer" }, { "phone", "input-gaming" } };

struct Card
{
    Gtk::Widget       *frame  = nullptr;
    Gtk::Label        *label  = nullptr;
    Gtk::Entry        *entry  = nullptr;
    Gtk::Widget       *mark   = nullptr;   // the red cross before trouble
    Gtk::Label        *status = nullptr;
    Gtk::Button       *browse = nullptr;
    Gtk::Button       *clear  = nullptr;
    Gtk::ComboBoxText *mode   = nullptr;   // a chip's only
};

struct SidebarColumns : public Gtk::TreeModel::ColumnRecord
{
    Gtk::TreeModelColumn<Glib::ustring> name, icon;
    Gtk::TreeModelColumn<int>           data, weight;
    SidebarColumns() { add(name); add(icon); add(data); add(weight); }
};

// A slot's parent in the sidebar, as item data; -1 for straight under its family.
int parent_of(int slot)
{
    const S9xBiosNesting n = S9xGetBiosNesting(slot);
    if (n.parent < 0)
        return -1;
    return (n.parent >= S9X_BIOS_UNDER_GROUP) ? kGroup + n.parent - S9X_BIOS_UNDER_GROUP : n.parent;
}

int chip_mode(const Card &c)
{
    if (!c.mode)
        return -1;
    const Glib::ustring id = c.mode->get_active_id();
    return id.empty() ? (int)S9X_CHIP_NATIVE : std::stoi(id.raw());
}

// A line of the dialog's text, in pixels.
int text_height(Gtk::Widget &w)
{
    int tw, th;
    w.create_pango_layout("X")->get_pixel_size(tw, th);
    return th;
}

// The speed chart: a group of bars per measured scene, one per mode on a scale from 0,
// with gridlines under them and each bar's value after it; `chip` shows the time in the
// chip rather than the frame time.
void bench_geometry(int th, int &bar, int &gap, int &group, int &pad)
{
    bar   = th;
    gap   = std::max(1, th / 6);
    group = th;
    pad   = th / 2 + 2;
}

int bench_height(int th, int scenes)
{
    int bar, gap, group, pad;
    bench_geometry(th, bar, gap, group, pad);
    return pad + th + pad + scenes * (3 * bar + 2 * gap) + (scenes - 1) * group + 2 * gap + th + pad;
}

// A bar's label: frame time with its change against the HLE, or the time in the chip.
std::string bench_label(const S9xBiosBenchScene &b, int mode, bool chip)
{
    char buf[64];
    if (chip)
        snprintf(buf, sizeof buf, b.ticks[mode] ? "%dM" : "<1M", b.ticks[mode]);
    else if (mode == 0)
        snprintf(buf, sizeof buf, "%.3f ms", b.ms[0]);
    else
        snprintf(buf, sizeof buf, "%.3f ms   %+.1f%%", b.ms[mode], (b.ms[mode] - b.ms[0]) * 100.0 / b.ms[0]);
    return buf;
}

void draw_bench(Gtk::DrawingArea &area, const Cairo::RefPtr<Cairo::Context> &cr, int slot, bool chip)
{
    auto      layout = area.create_pango_layout("");
    const int th     = text_height(area);
    int       bar, gap, group, pad;
    bench_geometry(th, bar, gap, group, pad);
    const int w = area.get_allocated_width(), h = area.get_allocated_height();
    auto text_w = [&](const std::string &s) {
        int tw, tth;
        layout->set_text(s);
        layout->get_pixel_size(tw, tth);
        return tw;
    };
    auto text = [&](double x, double y, const std::string &s) {
        layout->set_text(s);
        cr->move_to(x, y);
        layout->show_in_cairo_context(cr);
    };

    Gdk::RGBA paper, ink = area.get_style_context()->get_color();
    if (!area.get_style_context()->lookup_color("theme_base_color", paper))
        paper.set_rgba(1, 1, 1);
    auto set = [&](const Gdk::RGBA &c) { cr->set_source_rgb(c.get_red(), c.get_green(), c.get_blue()); };
    auto mix = [&](double a) {
        cr->set_source_rgb(paper.get_red() * (1 - a) + ink.get_red() * a, paper.get_green() * (1 - a) + ink.get_green() * a,
                           paper.get_blue() * (1 - a) + ink.get_blue() * a);
    };
    set(paper);
    cr->paint();
    mix(0.35);
    cr->set_line_width(1);
    cr->rectangle(0.5, 0.5, w - 1, h - 1);
    cr->stroke();

    // The legend: each mode's swatch and name.
    double x = pad;
    for (const auto &m : kModes)
    {
        const int sw = th * 2 / 3;
        cr->set_source_rgb(m.r, m.g, m.b);
        cr->rectangle(x, pad + (th - sw) / 2, sw, sw);
        cr->fill();
        x += sw + th / 3;
        set(ink);
        text(x, pad, _(m.name));
        x += text_w(_(m.name)) + th;
    }

    // Columns: the scenes' names, the bars, then room for the widest label.
    int                      count = 0, scenes = 0, namew = 0, labelw = 0;
    double                   most  = 0;
    const S9xBiosBenchScene *bench = S9xGetBiosBench(&count);
    for (int k = 0; k < count; k++)
    {
        if (bench[k].slot != slot)
            continue;
        scenes++;
        namew = std::max(namew, text_w(bench[k].scene));
        for (int m = 0; m < 3; m++)
        {
            labelw = std::max(labelw, text_w(bench_label(bench[k], m, chip)));
            most   = std::max(most, chip ? (double)bench[k].ticks[m] : (double)bench[k].ms[m]);
        }
    }
    static const double kSteps[] = { 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };
    double step = kSteps[sizeof kSteps / sizeof kSteps[0] - 1];
    for (double s : kSteps)
        if (most / s <= 5)
        {
            step = s;
            break;
        }
    int lines = (int)(most / step);
    if (lines * step < most || lines < 1)
        lines++;
    const double top = lines * step;

    const int x0   = pad + namew + th;
    const int x1   = w - pad - th / 3 - labelw;
    const int y0   = pad + th + pad;
    const int y1   = y0 + scenes * (3 * bar + 2 * gap) + (scenes - 1) * group;
    auto      x_of = [&](double v) { return x0 + (int)((x1 - x0) * v / top + 0.5); };

    // Gridlines and their numbers, then the zero line over them.
    for (int t = 0; t <= lines; t++)
    {
        const int gx = x_of(t * step);
        mix(1.0 / 6);
        cr->move_to(gx + 0.5, y0 - gap);
        cr->line_to(gx + 0.5, y1 + gap);
        cr->stroke();
        char num[32];
        snprintf(num, sizeof num, (chip && t) ? "%gM" : "%g", t * step);
        set(ink);
        text(gx - text_w(num) / 2, y1 + 2 * gap, num);
    }
    set(ink);
    cr->move_to(x0 + 0.5, y0 - gap);
    cr->line_to(x0 + 0.5, y1 + gap);
    cr->stroke();

    // Each scene: its name level with its three bars, each bar's value after it.
    int y = y0;
    for (int k = 0; k < count; k++)
    {
        const S9xBiosBenchScene &b = bench[k];
        if (b.slot != slot)
            continue;
        set(ink);
        text(pad, y + (3 * bar + 2 * gap - th) / 2, b.scene);
        for (int m = 0; m < 3; m++)
        {
            const int right = std::max(x_of(chip ? (double)b.ticks[m] : (double)b.ms[m]), x0 + 2);
            cr->set_source_rgb(kModes[m].r, kModes[m].g, kModes[m].b);
            cr->rectangle(x0 + 1, y, right - x0 - 1, bar);
            cr->fill();
            set(ink);
            text(right + th / 3, y + (bar - th) / 2, bench_label(b, m, chip));
            y += bar + gap;
        }
        y += group - gap;
    }
}

// The speed chart's popup for one chip slot.
void show_bench_chart(Gtk::Window &parent, int slot)
{
    Gtk::Dialog dialog(std::string(S9xGetBiosSlotInfo(slot)->label) + " " + _("Speed"), parent, true);
    dialog.add_button(_("_Close"), Gtk::RESPONSE_CLOSE);
    auto *content = dialog.get_content_area();
    content->set_spacing(6);
    content->set_border_width(10);

    Gtk::RadioButton::Group views;
    auto *frame = Gtk::manage(new Gtk::RadioButton(views, _("Frame time")));
    auto *chip  = Gtk::manage(new Gtk::RadioButton(views, _("Time in the chip")));
    auto *row   = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
    row->pack_start(*frame, Gtk::PACK_SHRINK);
    row->pack_start(*chip, Gtk::PACK_SHRINK);
    content->pack_start(*row, Gtk::PACK_SHRINK);

    auto *note = Gtk::manage(new Gtk::Label());
    note->set_line_wrap(true);
    note->set_max_width_chars(60);
    note->set_xalign(0.0f);
    content->pack_start(*note, Gtk::PACK_SHRINK);

    auto     *area = Gtk::manage(new Gtk::DrawingArea());
    bool      in_chip = false;
    const int th = text_height(dialog);
    area->set_size_request(th * 34, bench_height(th, S9xBiosBenchScenes(slot)));
    area->signal_draw().connect([area, slot, &in_chip](const Cairo::RefPtr<Cairo::Context> &cr) {
        draw_bench(*area, cr, slot, in_chip);
        return true;
    });
    content->pack_start(*area, Gtk::PACK_SHRINK);

    auto show = [&] {
        in_chip = chip->get_active();
        note->set_text(in_chip ? _("Time spent inside the chip over the same scenes, in millions of CPU clock ticks. Lower is faster. Measured on one PC.")
                               : _("Milliseconds the emulator takes per frame while each game plays a scripted scene. Lower is faster. Measured on one PC."));
        area->queue_draw();
    };
    chip->signal_toggled().connect(show);
    show();

    dialog.show_all();
    dialog.run();
}

// The speed chart's button face: a small bar chart in the modes' colours.
Gtk::Widget *bench_face(int size)
{
    auto *area = Gtk::manage(new Gtk::DrawingArea());
    area->set_size_request(size, size);
    area->signal_draw().connect([area](const Cairo::RefPtr<Cairo::Context> &cr) {
        static const int kTall[3] = { 45, 65, 100 };   // percent of the icon, short to tall
        const int        s = std::min(area->get_allocated_width(), area->get_allocated_height());
        const int        bw = std::max(2, s / 4), gap = std::max(1, (s - 3 * bw) / 4);
        for (int i = 0; i < 3; i++)
        {
            const int h = (s - 2) * kTall[i] / 100;
            cr->set_source_rgb(kModes[i].r, kModes[i].g, kModes[i].b);
            cr->rectangle(gap + i * (bw + gap), s - 1 - h, bw, h);
            cr->fill();
        }
        return true;
    });
    return area;
}

// The white-on-red cross before a status that's trouble.
Gtk::Widget *cross_badge(int size)
{
    auto *area = Gtk::manage(new Gtk::DrawingArea());
    area->set_size_request(size, size);
    area->set_valign(Gtk::ALIGN_CENTER);
    area->signal_draw().connect([area](const Cairo::RefPtr<Cairo::Context> &cr) {
        const double s = std::min(area->get_allocated_width(), area->get_allocated_height());
        cr->set_source_rgb(0xC0 / 255.0, 0x39 / 255.0, 0x2B / 255.0);
        cr->arc(s / 2, s / 2, s / 2, 0, 2 * G_PI);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
        cr->set_line_width(std::max(1.0, s / 7));
        cr->set_line_cap(Cairo::LINE_CAP_ROUND);
        cr->move_to(s * 0.32, s * 0.32);
        cr->line_to(s * 0.68, s * 0.68);
        cr->move_to(s * 0.68, s * 0.32);
        cr->line_to(s * 0.32, s * 0.68);
        cr->stroke();
        return true;
    });
    return area;
}

void refresh_card(int slot, Card &c)
{
    // Validate against the live text, not the stored path, so typing shows up.
    const std::string text    = c.entry->get_text();
    // A chip reads its file only in Firmware mode; otherwise the path waits greyed out.
    const int         mode    = chip_mode(c);
    const bool        file_on = mode < 0 || mode == S9X_CHIP_FIRMWARE;
    c.entry->set_sensitive(file_on);
    c.browse->set_sensitive(file_on);
    c.clear->set_sensitive(file_on && !text.empty());   // nothing to clear on a blank card

    enum { Plain, Note, Good, Bad } look = Plain;
    std::string says;
    if (mode >= 0 && (!file_on || text.empty()))
    {
        says = (mode == S9X_CHIP_HLE)    ? _("Fastest (less accurate)")
             : (mode == S9X_CHIP_NATIVE) ? _("Fast (chip accurate)")
                                         : _("Nothing selected: Slow (chip accurate)");
        look = file_on ? Bad : Plain;
    }
    else if (text.empty())
    {
        // Empty is fine for some slots and not others, so say which.
        const char *note = S9xGetBiosSlotInfo(slot)->note;
        says = note ? _(note) : "";
        look = Note;
    }
    else if (FILE *f = fopen(text.c_str(), "rb"))
    {
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
        {
            says = (mode >= 0) ? std::string(_("Slow (chip accurate)")) : why.empty() ? std::string(_("OK")) : std::string(_("OK")) + " - " + why;
            look = Good;
        }
        else
        {
            says = (st == S9X_BIOS_PATH_MISSING)   ? std::string(_("not found"))
                 : !why.empty()                    ? why
                 : (st == S9X_BIOS_PATH_BAD_IMAGE) ? std::string(_("wrong image"))
                                                   : std::string(_("unexpected size"));
            look = Bad;
        }
    }
    else
    {
        says = _("not found");
        look = Bad;
    }

    const Glib::ustring escaped = Glib::Markup::escape_text(says);
    c.status->set_markup(look == Good ? "<span foreground='#1e8b3a'>" + escaped + "</span>"
                       : look == Bad  ? "<span foreground='#c0392b'>" + escaped + "</span>"
                       : look == Note ? "<span foreground='#7f8c8d'><i>" + escaped + "</i></span>"
                                      : escaped);
    // The full line on hover, for when a long path gets cut short.
    c.status->set_tooltip_text(says);
    c.mark->set_visible(look == Bad);
}

} // namespace

void S9xGtkBiosManagerDialog(Gtk::Window *parent)
{
    Gtk::Dialog dialog(_("BIOS Manager"), true);
    if (parent)
        dialog.set_transient_for(*parent);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    Gtk::Button *save = dialog.add_button(_("_Save Changes"), Gtk::RESPONSE_OK);
    save->get_style_context()->add_class("suggested-action");
    dialog.set_default_response(Gtk::RESPONSE_OK);

    // The cards: rounded panels in the base colour on the dialog's background.
    auto css = Gtk::CssProvider::create();
    try
    {
        css->load_from_data(".bios-card { background-color: @theme_base_color; border: 1px solid @borders;"
                            " border-radius: 6px; padding: 6px 10px; }");
        Gtk::StyleContext::add_provider_for_screen(dialog.get_screen(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    catch (const Glib::Error &)
    {
    }

    const int th      = text_height(dialog);
    auto     *content = dialog.get_content_area();
    content->set_spacing(8);
    content->set_border_width(10);

    auto *intro = Gtk::manage(new Gtk::Label());
    intro->set_text(_("Configure BIOS files for system emulation. Nothing is searched for: "
                      "an entry without a file runs its built-in, if it has one, or is unavailable."));
    intro->set_line_wrap(true);
    intro->set_max_width_chars(90);
    intro->set_xalign(0.0f);
    content->pack_start(*intro, Gtk::PACK_SHRINK);

    auto *body = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 10));
    content->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    // The sidebar: a heading per family, bold with its theme icon, over its entries.
    SidebarColumns cols;
    auto           store = Gtk::TreeStore::create(cols);
    auto           icons = Gtk::IconTheme::get_default();
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
    {
        Gtk::TreeModel::Row heading = *store->append();
        heading[cols.name]   = _(S9xGetBiosFamily(g)->name);
        heading[cols.data]   = kHeading + g;
        heading[cols.weight] = Pango::WEIGHT_BOLD;
        heading[cols.icon]   = (icons && !icons->has_icon(kIcons[g][0])) ? kIcons[g][1] : kIcons[g][0];

        std::map<int, Gtk::TreeModel::iterator> under;   // the entries so far, by item data
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
        {
            const S9xBiosNesting n = S9xGetBiosNesting(*s);
            if (!n.listed)
                continue;
            const int parent = parent_of(*s);
            Gtk::TreeModel::Row host = heading;
            if (parent >= kGroup)
            {
                if (!under.count(parent))
                {
                    auto it = store->append(heading.children());
                    (*it)[cols.name]   = _(S9xBiosGroupName(parent - kGroup));
                    (*it)[cols.data]   = parent;
                    (*it)[cols.weight] = Pango::WEIGHT_NORMAL;
                    under[parent] = it;
                }
                host = *under[parent];
            }
            else if (parent >= 0 && under.count(parent))
                host = *under[parent];
            auto it = store->append(host.children());
            (*it)[cols.name]   = n.name ? _(n.name) : S9xGetBiosSlotInfo(*s)->label;
            (*it)[cols.data]   = *s;
            (*it)[cols.weight] = Pango::WEIGHT_NORMAL;
            under[*s] = it;
        }
    }
    auto *sidebar = Gtk::manage(new Gtk::TreeView(store));
    sidebar->set_headers_visible(false);
    auto *column = Gtk::manage(new Gtk::TreeViewColumn());
    auto *pix    = Gtk::manage(new Gtk::CellRendererPixbuf());
    auto *name   = Gtk::manage(new Gtk::CellRendererText());
    column->pack_start(*pix, false);
    column->add_attribute(pix->property_icon_name(), cols.icon);
    column->pack_start(*name, true);
    column->add_attribute(name->property_text(), cols.name);
    column->add_attribute(name->property_weight(), cols.weight);
    sidebar->append_column(*column);
    auto *side = Gtk::manage(new Gtk::ScrolledWindow());
    side->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    side->set_shadow_type(Gtk::SHADOW_IN);
    side->add(*sidebar);
    body->pack_start(*side, Gtk::PACK_SHRINK);

    // The cards, one per BIOS in sidebar order; the pick shows its own.
    std::vector<Card> cards(S9X_NUM_BIOS_SLOTS);
    auto *list = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, th / 2));
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
        {
            const int   slot = *s;
            const auto *info = S9xGetBiosSlotInfo(slot);
            Card       &c    = cards[slot];

            auto *frame = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
            frame->get_style_context()->add_class("bios-card");
            auto *grid = Gtk::manage(new Gtk::Grid());
            grid->set_column_spacing(th / 2);
            grid->set_row_spacing(th / 4);
            frame->pack_start(*grid, Gtk::PACK_EXPAND_WIDGET);

            // The label and the icons after it, centred on the card: the dumps it takes,
            // a chip's speed chart.
            c.label = Gtk::manage(new Gtk::Label(info->label));
            c.label->set_xalign(0.0f);
            c.label->set_valign(Gtk::ALIGN_CENTER);
            grid->attach(*c.label, 0, 0, 1, 2);
            auto *icon_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
            icon_box->set_valign(Gtk::ALIGN_CENTER);
            if (info->info)
            {
                auto *about = Gtk::manage(new Gtk::Button());
                about->set_image_from_icon_name("dialog-information", Gtk::ICON_SIZE_MENU);
                about->set_relief(Gtk::RELIEF_NONE);
                const std::string about_text = S9xBiosSlotInfoText(slot);
                about->set_tooltip_text(about_text);
                about->signal_clicked().connect([&dialog, info, about_text] {
                    Gtk::MessageDialog msg(dialog, about_text, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
                    msg.set_title(info->label);
                    msg.run();
                });
                icon_box->pack_start(*about, Gtk::PACK_SHRINK);
            }
            if (S9xBiosBenchScenes(slot))
            {
                auto *speed = Gtk::manage(new Gtk::Button());
                speed->add(*bench_face(16));
                speed->set_relief(Gtk::RELIEF_NONE);
                speed->set_tooltip_text(_("How fast Legacy, Native and Firmware run"));
                speed->signal_clicked().connect([&dialog, slot] { show_bench_chart(dialog, slot); });
                icon_box->pack_start(*speed, Gtk::PACK_SHRINK);
            }
            grid->attach(*icon_box, 1, 0, 1, 2);

            // The status line, after a chip's mode box, over the path box, Browse and Clear.
            auto *line = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, th / 3));
            if (S9xBiosSlotHasChipMode(slot))
            {
                c.mode = Gtk::manage(new Gtk::ComboBoxText());
                for (const auto &m : kModes)
                    c.mode->append(std::to_string(m.mode), _(m.name));
                c.mode->set_active_id(std::to_string(S9xChipModeInEffect(slot)));
                c.mode->set_tooltip_text(_("Legacy (HLE): the old high-level code, some games glitch\n"
                                           "Native (LLE): the chip itself, exact, no file needed\n"
                                           "Firmware: the chip running the dump picked here, exact but slower\n"
                                           "Takes effect at the next load or hard reset"));
                line->pack_start(*c.mode, Gtk::PACK_SHRINK);
            }
            c.mark = cross_badge(th * 3 / 4);
            line->pack_start(*c.mark, Gtk::PACK_SHRINK);
            c.status = Gtk::manage(new Gtk::Label());
            c.status->set_xalign(0.0f);
            c.status->set_ellipsize(Pango::ELLIPSIZE_END);
            line->pack_start(*c.status, Gtk::PACK_EXPAND_WIDGET);
            grid->attach(*line, 2, 0, 3, 1);

            c.entry = Gtk::manage(new Gtk::Entry());
            c.entry->set_text(S9xGetBiosPath(slot));
            c.entry->set_placeholder_text(info->names[0]);
            c.entry->set_width_chars(30);
            c.entry->set_hexpand(true);
            c.browse = Gtk::manage(new Gtk::Button(_("Browse")));
            c.browse->set_image_from_icon_name("document-open", Gtk::ICON_SIZE_BUTTON);
            c.browse->set_always_show_image(true);
            c.clear = Gtk::manage(new Gtk::Button(_("Clear")));
            c.clear->set_image_from_icon_name("edit-delete", Gtk::ICON_SIZE_BUTTON);
            c.clear->set_always_show_image(true);
            grid->attach(*c.entry, 2, 1, 1, 1);
            grid->attach(*c.browse, 3, 1, 1, 1);
            grid->attach(*c.clear, 4, 1, 1, 1);

            c.browse->signal_clicked().connect([&dialog, &cards, slot] {
                Gtk::FileChooserDialog chooser(dialog, _("Select BIOS File"), Gtk::FILE_CHOOSER_ACTION_OPEN);
                chooser.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
                chooser.add_button(_("_Open"), Gtk::RESPONSE_ACCEPT);

                const std::string current = cards[slot].entry->get_text();
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
                    cards[slot].entry->set_text(chooser.get_filename());
            });
            c.clear->signal_clicked().connect([&cards, slot] { cards[slot].entry->set_text(""); });
            c.entry->signal_changed().connect([&cards, slot] { refresh_card(slot, cards[slot]); });
            if (c.mode)
                c.mode->signal_changed().connect([&cards, slot] { refresh_card(slot, cards[slot]); });

            c.frame = frame;
            list->pack_start(*frame, Gtk::PACK_SHRINK);
        }
    auto *scroll = Gtk::manage(new Gtk::ScrolledWindow());
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->add(*list);
    body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);

    // The pick's cards: a slot's own, a heading's whole family, or what a slot or group holds;
    // their labels as wide as the widest of them, so their icons line up.
    auto show_pick = [&] {
        auto      it   = sidebar->get_selection()->get_selected();
        const int pick = it ? (int)(*it)[cols.data] : -1;
        int       widest = 0;
        for (Card &c : cards)
            c.label->set_size_request(-1, -1);
        for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
            for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
            {
                const int  parent = parent_of(*s);
                const bool shown  = pick == *s || pick == kHeading + g || (parent >= 0 && pick == parent);
                cards[*s].frame->set_visible(shown);
                if (shown)
                {
                    int lmin, lnat;
                    cards[*s].label->get_preferred_width(lmin, lnat);
                    widest = std::max(widest, lnat);
                }
            }
        for (Card &c : cards)
            c.label->set_size_request(widest, -1);
    };
    sidebar->get_selection()->signal_changed().connect(show_pick);

    // Room for the biggest family's cards without scrolling, as on win32.
    dialog.set_default_size(th * 64, th * 44);
    dialog.show_all();
    // As wide as its widest entry with everything open, so opening one never resizes it.
    // Measured once shown: an unrealized tree view sizes its rows without the theme.
    sidebar->expand_all();
    int min_w, nat_w;
    sidebar->get_preferred_width(min_w, nat_w);
    side->set_size_request(nat_w + th * 2, -1);
    sidebar->collapse_all();
    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
        refresh_card(slot, cards[slot]);
    sidebar->get_selection()->select(store->children().begin());
    show_pick();

    const int response = dialog.run();
    Gtk::StyleContext::remove_provider_for_screen(dialog.get_screen(), css);
    if (response != Gtk::RESPONSE_OK)
        return;

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        S9xSetBiosPath(slot, cards[slot].entry->get_text().c_str());
        const int mode = chip_mode(cards[slot]);
        if (mode >= 0)
            S9xSetChipMode(slot, mode);
    }

    gui_config->save_config_file();

    // The running cart keeps the BIOS it was loaded against - these paths are
    // only read at load time. What changes is which Game Boy Model entries are
    // selectable, and this menu is only rebuilt here and on a load, so it has
    // to be asked for explicitly.
    top_level->configure_widgets();
}
