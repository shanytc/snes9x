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

std::string hex(uint32_t value, int digits)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%0*X", digits, value);
    return buffer;
}

/* Emulation > S-PPU > Tile Viewer, as win32's CVRAMViewerDlg offers it:
 * decode any of the tile sources at 2/4/8bpp or Mode 7, at a chosen address,
 * width and object layout, through CGRAM or a grayscale ramp. The palette
 * strip picks the colour block, and the base-address readout jumps to what
 * the PPU is currently pointing each layer at. */
class Snes9xTileViewer : public PPUViewerWindow
{
  public:
    Snes9xTileViewer();

  protected:
    void refresh() override;

  private:
    void show_address();
    void show_tile_info();
    void rerender();
    void goto_base_address(int which);
    void export_tiles();
    void show_canvas_menu(GdkEventButton *event);

    pv::TileViewerState state;
    pv::Image picture;
    int selected_tile = -1;
    bool syncing = false;

    Gtk::Box outer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton grid_check{ _("Show Grid") };
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::CheckButton cgram_check{ _("Use CGRAM") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::ComboBoxText source_combo;
    Gtk::ComboBoxText bit_depth_combo;
    Gtk::ComboBoxText layout_combo;
    Gtk::Entry address_entry;
    std::unique_ptr<PPURepeatButton> prev_button;
    std::unique_ptr<PPURepeatButton> next_button;
    Gtk::SpinButton width_spin;
    PPUPaletteArea palette_area;
    Gtk::Entry base_entries[pv::kBaseAddressCount];
    Gtk::Label tile_info;
    PPUImageArea canvas;
    Gtk::ScrolledWindow scroller;
    Gtk::Menu canvas_menu;
};

Snes9xTileViewer::Snes9xTileViewer()
    : PPUViewerWindow(_("Tile Viewer"))
{
    set_default_size(940, 700);

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

    auto update_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto_update_check.set_active(true);
    auto_update_check.signal_toggled().connect(
        [this] { auto_update = auto_update_check.get_active(); });
    update_row->pack_start(auto_update_check, Gtk::PACK_SHRINK);
    refresh_button.signal_clicked().connect([this] { refresh(); });
    update_row->pack_start(refresh_button, Gtk::PACK_SHRINK);
    side.pack_start(*update_row, Gtk::PACK_SHRINK);

    auto form = Gtk::manage(new Gtk::Grid());
    form->set_row_spacing(4);
    form->set_column_spacing(6);
    int row = 0;

    form->attach(*Gtk::manage(new Gtk::Label(_("Source:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (int i = 0; i < pv::kSourceCount; i++)
        source_combo.append(_(pv::source_name(i)));
    source_combo.set_active(state.source);
    source_combo.signal_changed().connect([this] {
        const int index = source_combo.get_active_row_number();
        if (syncing || index < 0)
            return;
        state.source = index;
        state.address = 0;
        selected_tile = -1;
        show_address();
        show_tile_info();
        rerender();
    });
    form->attach(source_combo, 1, row++, 2, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Address:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    auto address_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
    address_entry.set_width_chars(10);
    address_entry.signal_changed().connect([this] {
        if (syncing)
            return;
        uint32_t value;
        if (!pv::parse_hex(address_entry.get_text(), &value))
            return;
        state.address = pv::align_address(state.bit_depth,
                                          value & pv::address_mask(state.source));
        rerender();
    });
    address_row->pack_start(address_entry, Gtk::PACK_EXPAND_WIDGET);
    prev_button = std::make_unique<PPURepeatButton>("<", [this] {
        pv::step_address(state, false);
        show_address();
        rerender();
    });
    next_button = std::make_unique<PPURepeatButton>(">", [this] {
        pv::step_address(state, true);
        show_address();
        rerender();
    });
    address_row->pack_start(*prev_button, Gtk::PACK_SHRINK);
    address_row->pack_start(*next_button, Gtk::PACK_SHRINK);
    form->attach(*address_row, 1, row++, 2, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Bit Depth:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (int i = 0; i < pv::kBitDepthCount; i++)
        bit_depth_combo.append(_(pv::bit_depth_name(i)));
    bit_depth_combo.set_active(state.bit_depth);
    bit_depth_combo.signal_changed().connect([this] {
        const int index = bit_depth_combo.get_active_row_number();
        if (syncing || index < 0)
            return;
        state.bit_depth = index;
        selected_tile = -1;
        show_tile_info();
        rerender();
    });
    form->attach(bit_depth_combo, 1, row++, 2, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Width:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    width_spin.set_adjustment(Gtk::Adjustment::create(state.width_tiles, 8, 64, 1, 8));
    width_spin.signal_value_changed().connect([this] {
        state.width_tiles = width_spin.get_value_as_int();
        rerender();
    });
    form->attach(width_spin, 1, row++, 1, 1);

    form->attach(*Gtk::manage(new Gtk::Label(_("Layout:"), Gtk::ALIGN_START)), 0, row, 1, 1);
    for (int i = 0; i < pv::kTileLayoutCount; i++)
        layout_combo.append(_(pv::tile_layout_name(i)));
    layout_combo.set_active(state.layout);
    layout_combo.signal_changed().connect([this] {
        const int index = layout_combo.get_active_row_number();
        if (syncing || index < 0)
            return;
        state.layout = index;
        selected_tile = -1;
        show_tile_info();
        rerender();
    });
    form->attach(layout_combo, 1, row++, 2, 1);

    side.pack_start(*form, Gtk::PACK_SHRINK);

    cgram_check.set_active(state.use_cgram);
    cgram_check.signal_toggled().connect([this] {
        if (syncing)
            return;
        state.use_cgram = cgram_check.get_active();
        rerender();
    });
    side.pack_start(cgram_check, Gtk::PACK_SHRINK);

    auto palette_frame = Gtk::manage(new Gtk::Frame(_("Palette (click to select offset)")));
    palette_area.signal_selection_changed.connect([this](int index) {
        state.palette_offset = index;
        /* Picking a colour block only means something through the real
         * palette, so this turns the grayscale ramp back off. */
        state.use_cgram = true;
        syncing = true;
        cgram_check.set_active(true);
        syncing = false;
        rerender();
    });
    palette_frame->add(palette_area);
    side.pack_start(*palette_frame, Gtk::PACK_SHRINK);

    auto base_frame = Gtk::manage(new Gtk::Frame(_("Base Tile Addresses")));
    auto base_grid = Gtk::manage(new Gtk::Grid());
    base_grid->set_row_spacing(2);
    base_grid->set_column_spacing(4);
    base_grid->set_border_width(4);
    static const char *base_labels[pv::kBaseAddressCount] = { "BG1:",  "BG2:",  "BG3:",
                                                              "BG4:",  "OAM1:", "OAM2:" };
    for (int i = 0; i < pv::kBaseAddressCount; i++)
    {
        base_grid->attach(*Gtk::manage(new Gtk::Label(base_labels[i], Gtk::ALIGN_START)), 0, i, 1, 1);
        base_entries[i].set_width_chars(8);
        base_entries[i].set_editable(false);
        base_grid->attach(base_entries[i], 1, i, 1, 1);
        auto button = Gtk::manage(new Gtk::Button(_("goto")));
        button->signal_clicked().connect([this, i] { goto_base_address(i); });
        base_grid->attach(*button, 2, i, 1, 1);
    }
    base_frame->add(*base_grid);
    side.pack_start(*base_frame, Gtk::PACK_SHRINK);

    tile_info.set_xalign(0.0f);
    tile_info.set_yalign(0.0f);
    tile_info.set_selectable(true);
    side.pack_start(tile_info, Gtk::PACK_SHRINK);

    side.set_size_request(280, -1);
    outer.set_border_width(6);
    outer.pack_start(side, Gtk::PACK_SHRINK);

    auto canvas_frame = Gtk::manage(new Gtk::Frame(_("Tiles")));
    canvas.signal_pixel_clicked.connect([this](int x, int y) {
        const int tile = pv::tile_at_pixel(state, x, y);
        if (tile < 0)
            return;
        selected_tile = tile;
        show_tile_info();
    });
    canvas.signal_menu_requested.connect(
        sigc::mem_fun(*this, &Snes9xTileViewer::show_canvas_menu));
    scroller.add(canvas);
    scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    canvas_frame->add(scroller);
    outer.pack_start(*canvas_frame, Gtk::PACK_EXPAND_WIDGET);

    auto export_item = Gtk::manage(new Gtk::MenuItem(_("Export to PNG..."), true));
    export_item->signal_activate().connect([this] { export_tiles(); });
    canvas_menu.append(*export_item);
    canvas_menu.attach_to_widget(*this);
    canvas_menu.show_all();

    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseTileViewer();
        return true;
    });

    show_address();
    show_all();
    refresh();
}

void Snes9xTileViewer::show_address()
{
    syncing = true;
    address_entry.set_text(hex(state.address & pv::address_mask(state.source),
                               state.source == pv::kSourceVRAM ? 4 : 6));
    syncing = false;
}

void Snes9xTileViewer::show_tile_info()
{
    if (selected_tile < 0)
    {
        tile_info.set_text("");
        return;
    }
    const int digits = state.source == pv::kSourceVRAM ? 4 : 6;
    tile_info.set_text(Glib::ustring::compose(
        _("Selected tile #%1\nAddress %2"), selected_tile,
        hex(pv::tile_address(state, selected_tile), digits)));
}

void Snes9xTileViewer::goto_base_address(int which)
{
    uint32_t addresses[pv::kBaseAddressCount];
    pv::base_addresses(addresses);

    state.bit_depth = pv::base_address_bit_depth(which);
    /* The base addresses name places in VRAM, wherever the view was pointed. */
    state.source = pv::kSourceVRAM;
    state.address = pv::align_address(state.bit_depth, addresses[which]);
    selected_tile = -1;

    syncing = true;
    bit_depth_combo.set_active(state.bit_depth);
    source_combo.set_active(pv::kSourceVRAM);
    syncing = false;

    show_address();
    show_tile_info();
    rerender();
}

void Snes9xTileViewer::rerender()
{
    pv::Pixel palette[256];
    pv::render_tiles(state, picture);
    pv::snapshot_palette(palette);

    canvas.set_image(picture);
    palette_area.set_colors(palette);
    palette_area.set_selected(state.palette_offset);
}

void Snes9xTileViewer::refresh()
{
    rerender();

    uint32_t addresses[pv::kBaseAddressCount];
    pv::base_addresses(addresses);
    for (int i = 0; i < pv::kBaseAddressCount; i++)
        base_entries[i].set_text(hex(addresses[i], 4));
}

void Snes9xTileViewer::export_tiles()
{
    /* The grid is a reading aid, not part of the tiles: leave it out of the
     * file however the checkbox is set. */
    pv::Image exported;
    pv::TileViewerState without_grid = state;
    without_grid.show_grid = false;
    pv::render_tiles(without_grid, exported);
    export_image(exported, "tiles.png");
}

void Snes9xTileViewer::show_canvas_menu(GdkEventButton *event)
{
    canvas_menu.popup_at_pointer((GdkEvent *)event);
}

std::unique_ptr<Snes9xTileViewer> tile_viewer;

} // namespace

void S9xShowTileViewer()
{
    if (!tile_viewer)
        tile_viewer = std::make_unique<Snes9xTileViewer>();
    tile_viewer->present();
}

void S9xCloseTileViewer()
{
    if (!tile_viewer)
        return;
    tile_viewer->shutdown();
    tile_viewer->hide();
    /* This can run from the window's own delete-event handler, where
     * destroying it is unsafe: let the main loop finish with it first. */
    auto *window = tile_viewer.release();
    Glib::signal_idle().connect_once([window] { delete window; });
}
