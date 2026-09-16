/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_ppu_viewer.h"
#include "gtk_s9x.h"

#include <memory>
#include <vector>

namespace pv = ppuviewer;

namespace
{

/* How often the list and the composition are re-read. The SNES rewrites OAM
 * every frame, so a slower rate is what makes the numbers readable. */
struct RateOption
{
    const char *label;
    int interval_ms;
};

const RateOption kRates[] = {
    { N_("Realtime"), 1000 / 60 }, { N_("0.25s"), 250 }, { N_("0.5s"), 500 },
    { N_("1s"), 1000 },            { N_("2s"), 2000 },
};
constexpr int kRateCount = sizeof(kRates) / sizeof(kRates[0]);
constexpr int kDefaultRate = 3; // 1s

std::string hex3(uint32_t value)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%03X", value & 0x1FF);
    return buffer;
}

std::string flags_text(const pv::Sprite &sprite)
{
    return std::string(sprite.hflip ? "H" : "-") + (sprite.vflip ? "V" : "-");
}

struct SpriteColumns : public Gtk::TreeModel::ColumnRecord
{
    Gtk::TreeModelColumn<bool> visible;
    Gtk::TreeModelColumn<int> index;
    Gtk::TreeModelColumn<Glib::ustring> size;
    Gtk::TreeModelColumn<int> x;
    Gtk::TreeModelColumn<int> y;
    Gtk::TreeModelColumn<Glib::ustring> character;
    Gtk::TreeModelColumn<int> priority;
    Gtk::TreeModelColumn<int> palette;
    Gtk::TreeModelColumn<Glib::ustring> flags;

    SpriteColumns()
    {
        add(visible);
        add(index);
        add(size);
        add(x);
        add(y);
        add(character);
        add(priority);
        add(palette);
        add(flags);
    }
};

/* Emulation > S-PPU > Sprite Viewer, as win32's CSpriteViewerDlg offers it:
 * every object composed where it sits at its priority, a list that can hide
 * them one at a time, and a preview of whichever one is selected. */
class Snes9xSpriteViewer : public PPUViewerWindow
{
  public:
    Snes9xSpriteViewer();

  protected:
    void refresh() override;

  private:
    void update_list();
    void draw_screen();
    void draw_preview();
    std::vector<int> selected_sprites() const;
    bool on_list_button_press(GdkEventButton *event);
    void apply_visibility_to_list();
    void export_selected();
    void export_screen();
    void export_preview();

    pv::OAMSnapshot oam;
    pv::Image screen_picture;
    pv::Image preview_picture;
    bool visible[128] = {};
    int selected = 0;
    int background = pv::kBackgroundTransparent;
    bool show_outline = true;
    /* Guards the bulk check-state updates the list's context menu makes, so
     * the screen is redrawn once instead of 128 times. */
    bool updating_list = false;

    SpriteColumns columns;
    Glib::RefPtr<Gtk::ListStore> store;

    Gtk::Box outer{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::Box bottom{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    PPUImageArea screen_view;
    Gtk::ScrolledWindow screen_scroller;
    Gtk::TreeView list;
    Gtk::ScrolledWindow list_scroller;
    PPUImageArea preview;
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::ComboBoxText rate_combo;
    Gtk::CheckButton outline_check{ _("Show Screen Outline") };
    Gtk::ComboBoxText background_combo;
    Gtk::Label first_sprite_label;
    Gtk::Label details_label;
    Gtk::Menu list_menu;
    Gtk::Menu image_menu;
    /* Which of the two picture areas the export menu was asked for. */
    bool image_menu_is_screen = true;
};

Snes9xSpriteViewer::Snes9xSpriteViewer()
    : PPUViewerWindow(_("Sprite Viewer"))
{
    set_default_size(1000, 780);
    for (bool &shown : visible)
        shown = true;

    auto screen_frame = Gtk::manage(new Gtk::Frame(_("Screen")));
    screen_view.signal_menu_requested.connect([this](GdkEventButton *event) {
        image_menu_is_screen = true;
        image_menu.popup_at_pointer((GdkEvent *)event);
    });
    screen_scroller.add(screen_view);
    screen_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    screen_frame->add(screen_scroller);
    outer.pack_start(*screen_frame, Gtk::PACK_EXPAND_WIDGET);

    store = Gtk::ListStore::create(columns);
    for (int i = 0; i < 128; i++)
    {
        auto row = *store->append();
        row[columns.visible] = true;
        row[columns.index] = i;
    }
    list.set_model(store);

    auto toggle = Gtk::manage(new Gtk::CellRendererToggle());
    toggle->set_activatable(true);
    toggle->signal_toggled().connect([this](const Glib::ustring &path) {
        auto row = *store->get_iter(path);
        const int index = row[columns.index];
        if (index < 0 || index >= 128)
            return;
        visible[index] = !visible[index];
        row[columns.visible] = visible[index];
        draw_screen();
    });
    auto toggle_column = Gtk::manage(new Gtk::TreeViewColumn("", *toggle));
    toggle_column->add_attribute(toggle->property_active(), columns.visible);
    list.append_column(*toggle_column);

    list.append_column(_("#"), columns.index);
    list.append_column(_("Size"), columns.size);
    list.append_column(_("X"), columns.x);
    list.append_column(_("Y"), columns.y);
    list.append_column(_("Char"), columns.character);
    list.append_column(_("Pri"), columns.priority);
    list.append_column(_("Pal"), columns.palette);
    list.append_column(_("Flags"), columns.flags);

    list.get_selection()->set_mode(Gtk::SELECTION_MULTIPLE);
    list.get_selection()->signal_changed().connect([this] {
        if (updating_list)
            return;
        const std::vector<int> indices = selected_sprites();
        if (indices.empty())
            return;
        selected = indices.front();
        draw_preview();
    });
    list.signal_button_press_event().connect(
        sigc::mem_fun(*this, &Snes9xSpriteViewer::on_list_button_press), false);
    list_scroller.add(list);
    list_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    bottom.pack_start(list_scroller, Gtk::PACK_EXPAND_WIDGET);

    auto preview_frame = Gtk::manage(new Gtk::Frame(_("Preview")));
    auto preview_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    preview_box->set_border_width(4);
    preview.set_fit_to_widget(true);
    /* Pinned, not expanded: on_draw floods the whole widget with the
     * background colour, so a wider widget means a wider block of colour
     * rather than more room around the sprite. The panel's spare width goes
     * to the gap before the controls instead. */
    preview.set_size_request(140, 140);
    preview.signal_menu_requested.connect([this](GdkEventButton *event) {
        image_menu_is_screen = false;
        image_menu.popup_at_pointer((GdkEvent *)event);
    });
    preview_box->pack_start(preview, Gtk::PACK_SHRINK);

    auto controls = Gtk::manage(new Gtk::Grid());
    controls->set_row_spacing(4);
    controls->set_column_spacing(6);
    /* Stacked at the image's top with the slack left below it, as win32 has
     * them, rather than spread down the whole height of the preview. */
    controls->set_valign(Gtk::ALIGN_START);
    int row = 0;
    controls->attach(*Gtk::manage(new Gtk::Label(_("Zoom:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    S9xFillZoomCombo(zoom_combo, 1);
    zoom_combo.signal_changed().connect([this] {
        const int index = zoom_combo.get_active_row_number();
        if (index >= 0)
            screen_view.set_zoom(index + 1);
    });
    controls->attach(zoom_combo, 1, row++, 1, 1);

    auto_update_check.set_active(true);
    auto_update_check.signal_toggled().connect(
        [this] { auto_update = auto_update_check.get_active(); });
    controls->attach(auto_update_check, 0, row++, 2, 1);

    refresh_button.signal_clicked().connect([this] { refresh(); });
    controls->attach(refresh_button, 0, row++, 2, 1);

    controls->attach(*Gtk::manage(new Gtk::Label(_("Rate:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (const auto &rate : kRates)
        rate_combo.append(_(rate.label));
    rate_combo.set_active(kDefaultRate);
    rate_combo.signal_changed().connect([this] {
        const int index = rate_combo.get_active_row_number();
        if (index >= 0 && index < kRateCount)
            set_update_interval(kRates[index].interval_ms);
    });
    controls->attach(rate_combo, 1, row++, 1, 1);
    preview_box->pack_end(*controls, Gtk::PACK_SHRINK);
    preview_frame->add(*preview_box);
    side.pack_start(*preview_frame, Gtk::PACK_SHRINK);

    outline_check.set_active(show_outline);
    outline_check.signal_toggled().connect([this] {
        show_outline = outline_check.get_active();
        draw_screen();
    });
    side.pack_start(outline_check, Gtk::PACK_SHRINK);

    auto background_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    background_row->pack_start(*Gtk::manage(new Gtk::Label(_("Background:"))), Gtk::PACK_SHRINK);
    for (int i = 0; i < pv::kSpriteBackgroundCount; i++)
    {
        const char *name = pv::sprite_background_name(i);
        if (name)
            background_combo.append(_(name));
        else
            background_combo.append(Glib::ustring::compose(_("Sprite Palette %1"),
                                                           i - pv::kBackgroundPalette0));
    }
    background_combo.set_active(background);
    background_combo.signal_changed().connect([this] {
        const int index = background_combo.get_active_row_number();
        if (index < 0)
            return;
        background = index;
        draw_screen();
        draw_preview();
    });
    background_row->pack_start(background_combo, Gtk::PACK_EXPAND_WIDGET);
    side.pack_start(*background_row, Gtk::PACK_SHRINK);

    auto first_sprite_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    first_sprite_row->pack_start(*Gtk::manage(new Gtk::Label(_("First Sprite:"))),
                                 Gtk::PACK_SHRINK);
    first_sprite_row->pack_start(first_sprite_label, Gtk::PACK_SHRINK);
    side.pack_start(*first_sprite_row, Gtk::PACK_SHRINK);

    details_label.set_xalign(0.0f);
    details_label.set_yalign(0.0f);
    details_label.set_selectable(true);
    side.pack_start(details_label, Gtk::PACK_SHRINK);

    side.set_size_request(380, -1);
    bottom.pack_start(side, Gtk::PACK_SHRINK);
    outer.pack_start(bottom, Gtk::PACK_EXPAND_WIDGET);
    outer.set_border_width(6);

    /* The list's menu: the visibility bulk actions plus the export, as on
     * win32's list context menu. */
    auto add_item = [](Gtk::Menu &menu, const char *label, const std::function<void()> &action) {
        auto item = Gtk::manage(new Gtk::MenuItem(label, true));
        item->signal_activate().connect(action);
        menu.append(*item);
    };
    add_item(list_menu, _("Toggle Visibility"), [this] {
        for (int index : selected_sprites())
            visible[index] = !visible[index];
        apply_visibility_to_list();
        draw_screen();
    });
    add_item(list_menu, _("Show Only Selected Objects"), [this] {
        const std::vector<int> indices = selected_sprites();
        for (bool &shown : visible)
            shown = false;
        for (int index : indices)
            visible[index] = true;
        apply_visibility_to_list();
        draw_screen();
    });
    add_item(list_menu, _("Show All Objects"), [this] {
        for (bool &shown : visible)
            shown = true;
        apply_visibility_to_list();
        draw_screen();
    });
    list_menu.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    add_item(list_menu, _("Export Selected to PNG..."), [this] { export_selected(); });
    list_menu.attach_to_widget(*this);
    list_menu.show_all();

    add_item(image_menu, _("Export to PNG..."), [this] {
        if (image_menu_is_screen)
            export_screen();
        else
            export_preview();
    });
    image_menu.attach_to_widget(*this);
    image_menu.show_all();

    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseSpriteViewer();
        return true;
    });

    set_update_interval(kRates[kDefaultRate].interval_ms);
    show_all();
    refresh();
    list.get_selection()->select(store->children().begin());
}

void Snes9xSpriteViewer::refresh()
{
    pv::snapshot_oam(oam);

    update_list();
    first_sprite_label.set_text(std::to_string(oam.first_sprite));
    draw_screen();
    draw_preview();
}

void Snes9xSpriteViewer::update_list()
{
    updating_list = true;
    int i = 0;
    for (auto row : store->children())
    {
        const pv::Sprite &sprite = oam.sprites[i];
        int width, height;
        pv::sprite_size(oam.size_select, sprite.large, &width, &height);

        row[columns.size] = Glib::ustring::compose("%1x%2", width, height);
        row[columns.x] = (int)sprite.x;
        row[columns.y] = (int)sprite.y;
        row[columns.character] = hex3(sprite.name);
        row[columns.priority] = (int)sprite.priority;
        row[columns.palette] = (int)sprite.palette;
        row[columns.flags] = flags_text(sprite);
        i++;
    }
    updating_list = false;
}

void Snes9xSpriteViewer::apply_visibility_to_list()
{
    updating_list = true;
    int i = 0;
    for (auto row : store->children())
        row[columns.visible] = visible[i++];
    updating_list = false;
}

void Snes9xSpriteViewer::draw_screen()
{
    pv::render_sprite_screen(oam, visible, background, show_outline, false, screen_picture);

    pv::Pixel palette[256];
    pv::snapshot_palette(palette);
    screen_view.set_background(pv::sprite_background_color(background, palette));
    screen_view.set_image(screen_picture);
}

void Snes9xSpriteViewer::draw_preview()
{
    if (selected < 0 || selected >= 128)
    {
        details_label.set_text("");
        preview.set_image(pv::Image());
        return;
    }

    pv::render_sprite(oam, selected, preview_picture, background, false);

    pv::Pixel palette[256];
    pv::snapshot_palette(palette);
    preview.set_background(pv::sprite_background_color(background, palette));
    preview.set_image(preview_picture);

    const pv::Sprite &sprite = oam.sprites[selected];
    int width, height;
    pv::sprite_size(oam.size_select, sprite.large, &width, &height);
    details_label.set_text(Glib::ustring::compose(
        _("Sprite #%1\nsize %2x%3\npos (%4,%5)\ntile %6\npal %7  pri %8\nflags %9"), selected,
        width, height, (int)sprite.x, (int)sprite.y, hex3(sprite.name), (int)sprite.palette,
        (int)sprite.priority, flags_text(sprite)));
}

std::vector<int> Snes9xSpriteViewer::selected_sprites() const
{
    std::vector<int> indices;
    for (const auto &path : list.get_selection()->get_selected_rows())
    {
        auto row = *store->get_iter(path);
        const int index = row[columns.index];
        if (index >= 0 && index < 128)
            indices.push_back(index);
    }
    return indices;
}

bool Snes9xSpriteViewer::on_list_button_press(GdkEventButton *event)
{
    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return false;

    /* Right-clicking outside the selection moves it there first, so the menu
     * always acts on what the user is pointing at. */
    Gtk::TreeModel::Path path;
    if (list.get_path_at_pos((int)event->x, (int)event->y, path) &&
        !list.get_selection()->is_selected(path))
    {
        list.get_selection()->unselect_all();
        list.get_selection()->select(path);
    }

    list_menu.popup_at_pointer((GdkEvent *)event);
    return true;
}

void Snes9xSpriteViewer::export_selected()
{
    const std::vector<int> indices = selected_sprites();
    if (indices.empty())
        return;

    /* One object goes out as a PNG; a set of them as a zip of PNGs, which is
     * what win32's list context menu does. */
    if (indices.size() == 1)
    {
        pv::Image exported;
        pv::render_sprite(oam, indices[0], exported, pv::kBackgroundTransparent, true);
        export_image(exported, ("sprite_" + std::to_string(indices[0]) + ".png").c_str());
        return;
    }

    std::vector<pv::ZipBlob> entries;
    for (int index : indices)
    {
        pv::Image exported;
        pv::render_sprite(oam, index, exported, pv::kBackgroundTransparent, true);
        pv::ZipBlob blob;
        blob.name = "sprite_" + std::to_string(index) + ".png";
        if (pv::write_png_memory(exported, blob.data))
            entries.push_back(std::move(blob));
    }
    if (entries.empty())
        return;

    Gtk::FileChooserDialog dialog(*this, _("Export Selected to PNG..."),
                                  Gtk::FILE_CHOOSER_ACTION_SAVE);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_Save"), Gtk::RESPONSE_ACCEPT);
    dialog.set_do_overwrite_confirmation(true);
    dialog.set_current_name("sprites.zip");

    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("ZIP Archive"));
    filter->add_pattern("*.zip");
    dialog.add_filter(filter);

    if (dialog.run() != Gtk::RESPONSE_ACCEPT)
        return;

    std::string filename = dialog.get_filename();
    dialog.hide();
    if (filename.size() < 4 || filename.compare(filename.size() - 4, 4, ".zip") != 0)
        filename += ".zip";

    if (!pv::write_zip(filename, entries))
        Gtk::MessageDialog(*this, _("Failed to save ZIP."), false, Gtk::MESSAGE_ERROR,
                           Gtk::BUTTONS_CLOSE, true)
            .run();
}

void Snes9xSpriteViewer::export_screen()
{
    /* No screen outline in the file, and a transparent background stays
     * transparent rather than becoming the viewer's gray. */
    pv::Image exported;
    pv::render_sprite_screen(oam, visible, background, false, true, exported);
    export_image(exported, "sprite_screen.png");
}

void Snes9xSpriteViewer::export_preview()
{
    if (selected < 0 || selected >= 128)
        return;
    pv::Image exported;
    pv::render_sprite(oam, selected, exported, pv::kBackgroundTransparent, true);
    export_image(exported, ("sprite_" + std::to_string(selected) + ".png").c_str());
}

std::unique_ptr<Snes9xSpriteViewer> sprite_viewer;

} // namespace

void S9xShowSpriteViewer()
{
    if (!sprite_viewer)
        sprite_viewer = std::make_unique<Snes9xSpriteViewer>();
    sprite_viewer->present();
}

void S9xCloseSpriteViewer()
{
    if (!sprite_viewer)
        return;
    sprite_viewer->shutdown();
    sprite_viewer->hide();
    auto *window = sprite_viewer.release();
    Glib::signal_idle().connect_once([window] { delete window; });
}
