/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_ppu_viewer.h"
#include "gtk_s9x.h"

#include <memory>

namespace pv = ppuviewer;

namespace
{

std::string hex16(uint32_t value)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%04X", value & 0xFFFF);
    return buffer;
}

/* Emulation > S-PPU > Tilemap Viewer, as win32's CTilemapViewerDlg offers
 * it: one background layer drawn whole, either as the PPU has it set up or
 * as a screen mode, map and character addresses the user names instead. */
class Snes9xTilemapViewer : public PPUViewerWindow
{
  public:
    Snes9xTilemapViewer();

  protected:
    void refresh() override;

  private:
    void apply_enabled_state();
    void seed_automatic_fields(const pv::TilemapInfo &info);
    void read_override_fields();
    void rerender();
    void export_tilemap();
    void show_canvas_menu(GdkEventButton *event);

    pv::TilemapViewerState state;
    pv::Image picture;
    bool syncing = false;

    Gtk::Box outer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton grid_check{ _("Show Grid") };
    Gtk::ComboBoxText background_combo;
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::CheckButton custom_mode_check{ _("Custom Screen Mode") };
    Gtk::SpinButton mode_spin;
    Gtk::RadioButton bg_radios[4];
    Gtk::CheckButton override_check{ _("Override Tilemap") };
    Gtk::ComboBoxText bit_depth_combo;
    Gtk::ComboBoxText map_size_combo;
    Gtk::ComboBoxText tile_size_combo;
    Gtk::Entry map_address_entry;
    Gtk::Entry tile_address_entry;
    Gtk::Label info_label;
    PPUImageArea canvas;
    Gtk::ScrolledWindow scroller;
    Gtk::Menu canvas_menu;
};

Snes9xTilemapViewer::Snes9xTilemapViewer()
    : PPUViewerWindow(_("Tilemap Viewer"))
{
    set_default_size(940, 680);

    auto top_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    top_row->pack_start(*Gtk::manage(new Gtk::Label(_("Zoom:"))), Gtk::PACK_SHRINK);
    S9xFillZoomCombo(zoom_combo, 1);
    zoom_combo.signal_changed().connect([this] {
        const int index = zoom_combo.get_active_row_number();
        if (index >= 0)
            canvas.set_zoom(index + 1);
    });
    top_row->pack_start(zoom_combo, Gtk::PACK_SHRINK);
    grid_check.signal_toggled().connect([this] {
        state.show_grid = grid_check.get_active();
        rerender();
    });
    top_row->pack_start(grid_check, Gtk::PACK_SHRINK);
    side.pack_start(*top_row, Gtk::PACK_SHRINK);

    auto background_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    background_row->pack_start(*Gtk::manage(new Gtk::Label(_("Background:"))), Gtk::PACK_SHRINK);
    for (int i = 0; i < pv::kTilemapBackgroundCount; i++)
        background_combo.append(_(pv::tilemap_background_name(i)));
    background_combo.set_active(state.background);
    background_combo.signal_changed().connect([this] {
        const int index = background_combo.get_active_row_number();
        if (index < 0)
            return;
        state.background = index;
        rerender();
    });
    background_row->pack_start(background_combo, Gtk::PACK_EXPAND_WIDGET);
    side.pack_start(*background_row, Gtk::PACK_SHRINK);

    auto update_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto_update_check.set_active(true);
    auto_update_check.signal_toggled().connect(
        [this] { auto_update = auto_update_check.get_active(); });
    update_row->pack_start(auto_update_check, Gtk::PACK_SHRINK);
    refresh_button.signal_clicked().connect([this] { refresh(); });
    update_row->pack_start(refresh_button, Gtk::PACK_SHRINK);
    side.pack_start(*update_row, Gtk::PACK_SHRINK);

    custom_mode_check.signal_toggled().connect([this] {
        state.custom_screen_mode = custom_mode_check.get_active();
        if (state.custom_screen_mode)
            state.custom_mode = mode_spin.get_value_as_int();
        apply_enabled_state();
        rerender();
    });
    side.pack_start(custom_mode_check, Gtk::PACK_SHRINK);

    auto mode_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    mode_row->pack_start(*Gtk::manage(new Gtk::Label(_("Mode:"))), Gtk::PACK_SHRINK);
    mode_spin.set_adjustment(Gtk::Adjustment::create(state.custom_mode, 0, 7, 1, 1));
    mode_spin.signal_value_changed().connect([this] {
        if (syncing || !state.custom_screen_mode)
            return;
        state.custom_mode = mode_spin.get_value_as_int();
        rerender();
    });
    mode_row->pack_start(mode_spin, Gtk::PACK_SHRINK);
    side.pack_start(*mode_row, Gtk::PACK_SHRINK);

    auto bg_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    bg_row->pack_start(*Gtk::manage(new Gtk::Label(_("BG:"))), Gtk::PACK_SHRINK);
    for (int i = 0; i < 4; i++)
    {
        bg_radios[i].set_label(std::to_string(i + 1));
        if (i > 0)
            bg_radios[i].join_group(bg_radios[0]);
        bg_radios[i].signal_toggled().connect([this, i] {
            if (!bg_radios[i].get_active())
                return;
            state.bg = i;
            rerender();
        });
        bg_row->pack_start(bg_radios[i], Gtk::PACK_SHRINK);
    }
    bg_radios[0].set_active(true);
    side.pack_start(*bg_row, Gtk::PACK_SHRINK);

    override_check.signal_toggled().connect([this] {
        state.override_tilemap = override_check.get_active();
        if (state.override_tilemap)
            read_override_fields();
        apply_enabled_state();
        rerender();
    });
    side.pack_start(override_check, Gtk::PACK_SHRINK);

    auto form = Gtk::manage(new Gtk::Grid());
    form->set_row_spacing(4);
    form->set_column_spacing(6);
    int row = 0;

    auto on_override_changed = [this] {
        if (syncing || !state.override_tilemap)
            return;
        read_override_fields();
        rerender();
    };

    form->attach(*Gtk::manage(new Gtk::Label(_("Bit Depth:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    bit_depth_combo.append(_("2bpp"));
    bit_depth_combo.append(_("4bpp"));
    bit_depth_combo.append(_("8bpp"));
    bit_depth_combo.append(_("Mode 7"));
    bit_depth_combo.set_active(state.override_bit_depth);
    bit_depth_combo.signal_changed().connect(on_override_changed);
    form->attach(bit_depth_combo, 1, row++, 1, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Map Size:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (const char *size : { "32x32", "64x32", "32x64", "64x64" })
        map_size_combo.append(size);
    map_size_combo.set_active(0);
    map_size_combo.signal_changed().connect(on_override_changed);
    form->attach(map_size_combo, 1, row++, 1, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Map Addr:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    map_address_entry.set_text("0x0000");
    map_address_entry.set_width_chars(8);
    map_address_entry.signal_changed().connect(on_override_changed);
    form->attach(map_address_entry, 1, row++, 1, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Tile Size:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (const char *size : { "8x8", "16x16" })
        tile_size_combo.append(size);
    tile_size_combo.set_active(0);
    tile_size_combo.signal_changed().connect(on_override_changed);
    form->attach(tile_size_combo, 1, row++, 1, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Tile Addr:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    tile_address_entry.set_text("0x0000");
    tile_address_entry.set_width_chars(8);
    tile_address_entry.signal_changed().connect(on_override_changed);
    form->attach(tile_address_entry, 1, row++, 1, 1);

    side.pack_start(*form, Gtk::PACK_SHRINK);

    info_label.set_xalign(0.0f);
    info_label.set_yalign(0.0f);
    info_label.set_selectable(true);
    side.pack_start(info_label, Gtk::PACK_SHRINK);

    side.set_size_request(280, -1);
    outer.set_border_width(6);
    outer.pack_start(side, Gtk::PACK_SHRINK);

    auto canvas_frame = Gtk::manage(new Gtk::Frame(_("Tilemap")));
    canvas.signal_menu_requested.connect(
        sigc::mem_fun(*this, &Snes9xTilemapViewer::show_canvas_menu));
    scroller.add(canvas);
    scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    canvas_frame->add(scroller);
    outer.pack_start(*canvas_frame, Gtk::PACK_EXPAND_WIDGET);

    auto export_item = Gtk::manage(new Gtk::MenuItem(_("Export to PNG..."), true));
    export_item->signal_activate().connect([this] { export_tilemap(); });
    canvas_menu.append(*export_item);
    canvas_menu.attach_to_widget(*this);
    canvas_menu.show_all();

    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseTilemapViewer();
        return true;
    });

    apply_enabled_state();
    show_all();
    refresh();
}

void Snes9xTilemapViewer::apply_enabled_state()
{
    mode_spin.set_sensitive(state.custom_screen_mode);
    for (Gtk::Widget *widget : { (Gtk::Widget *)&bit_depth_combo, (Gtk::Widget *)&map_size_combo,
                                 (Gtk::Widget *)&map_address_entry,
                                 (Gtk::Widget *)&tile_size_combo,
                                 (Gtk::Widget *)&tile_address_entry })
        widget->set_sensitive(state.override_tilemap);
}

void Snes9xTilemapViewer::read_override_fields()
{
    state.override_bit_depth = bit_depth_combo.get_active_row_number();
    state.override_map_size = map_size_combo.get_active_row_number();
    state.override_tile_size = tile_size_combo.get_active_row_number();

    uint32_t value;
    if (pv::parse_hex(map_address_entry.get_text(), &value))
        state.override_map_address = value & 0xFFFF;
    if (pv::parse_hex(tile_address_entry.get_text(), &value))
        state.override_tile_address = value & 0xFFFF;
}

/* While the override is off the fields show what the PPU actually has, so
 * turning it on starts from the live configuration rather than from zero. */
void Snes9xTilemapViewer::seed_automatic_fields(const pv::TilemapInfo &info)
{
    syncing = true;
    if (!state.custom_screen_mode)
        mode_spin.set_value(info.mode);
    if (!state.override_tilemap)
    {
        bit_depth_combo.set_active(info.mode7 ? pv::kTilemapMode7
                                              : pv::tilemap_bit_depth_index(info.bpp));
        map_size_combo.set_active((info.width_tiles == 64 ? 1 : 0) |
                                  (info.height_tiles == 64 ? 2 : 0));
        tile_size_combo.set_active(info.tile_size == 16 ? 1 : 0);
        map_address_entry.set_text(hex16(info.map_address));
        tile_address_entry.set_text(hex16(info.tile_address));
    }
    syncing = false;
}

void Snes9xTilemapViewer::rerender()
{
    pv::TilemapInfo info;
    pv::render_tilemap(state, picture, &info);

    canvas.set_image(picture);
    seed_automatic_fields(info);

    if (!info.valid)
    {
        info_label.set_text(Glib::ustring::compose(_("BG%1 not valid in mode %2"), info.bg + 1,
                                                   info.mode));
        return;
    }
    if (info.mode7)
    {
        info_label.set_text(_("Mode 7  128x128  8bpp"));
        return;
    }
    info_label.set_text(Glib::ustring::compose(
        _("Mode %1  BG%2  %3bpp\n%4x%5 tiles  %6x%7 px\nMap @ %8\nTiles @ %9"), info.mode,
        info.bg + 1, info.bpp, info.width_tiles, info.height_tiles,
        info.width_tiles * info.tile_size, info.height_tiles * info.tile_size,
        hex16(info.map_address), hex16(info.tile_address)));
}

void Snes9xTilemapViewer::refresh()
{
    rerender();
}

void Snes9xTilemapViewer::export_tilemap()
{
    /* As in the Tile Viewer, the grid overlay stays out of the file, and a
     * Transparent background exports as alpha 0. */
    pv::Image exported;
    pv::TilemapViewerState without_grid = state;
    without_grid.show_grid = false;
    pv::render_tilemap(without_grid, exported, nullptr, true);
    export_image(exported, "tilemap.png");
}

void Snes9xTilemapViewer::show_canvas_menu(GdkEventButton *event)
{
    canvas_menu.popup_at_pointer((GdkEvent *)event);
}

std::unique_ptr<Snes9xTilemapViewer> tilemap_viewer;

} // namespace

void S9xShowTilemapViewer()
{
    if (!tilemap_viewer)
        tilemap_viewer = std::make_unique<Snes9xTilemapViewer>();
    tilemap_viewer->present();
}

void S9xCloseTilemapViewer()
{
    if (!tilemap_viewer)
        return;
    tilemap_viewer->shutdown();
    tilemap_viewer->hide();
    auto *window = tilemap_viewer.release();
    Glib::signal_idle().connect_once([window] { delete window; });
}
