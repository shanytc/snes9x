/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_ppu_viewer.h"
#include "gtk_s9x.h"
#include "common/video/gb_ppu_viewer.hpp"

#include <memory>

namespace gv = gbviewer;
namespace pv = ppuviewer;

namespace
{

std::string hex(uint32_t value, int digits)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%0*X", digits, value);
    return buffer;
}

/* Zoom, Auto update and Refresh: the rows every GB viewer opens with. */
Gtk::Box *make_update_rows(Gtk::ComboBoxText &zoom_combo, int zoom, PPUImageArea &canvas,
                           Gtk::CheckButton &auto_update_check, bool &auto_update,
                           Gtk::Button &refresh_button, const std::function<void()> &refresh,
                           Gtk::Widget *beside_zoom)
{
    auto rows = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));

    auto top_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    top_row->pack_start(*Gtk::manage(new Gtk::Label(_("Zoom:"))), Gtk::PACK_SHRINK);
    S9xFillZoomCombo(zoom_combo, zoom);
    canvas.set_zoom(zoom);
    zoom_combo.signal_changed().connect([&zoom_combo, &canvas] {
        const int index = zoom_combo.get_active_row_number();
        if (index >= 0)
            canvas.set_zoom(index + 1);
    });
    top_row->pack_start(zoom_combo, Gtk::PACK_SHRINK);
    if (beside_zoom)
        top_row->pack_start(*beside_zoom, Gtk::PACK_SHRINK);
    rows->pack_start(*top_row, Gtk::PACK_SHRINK);

    auto update_row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto_update_check.set_active(true);
    auto_update_check.signal_toggled().connect(
        [&auto_update_check, &auto_update] { auto_update = auto_update_check.get_active(); });
    update_row->pack_start(auto_update_check, Gtk::PACK_SHRINK);
    refresh_button.signal_clicked().connect([refresh] { refresh(); });
    update_row->pack_start(refresh_button, Gtk::PACK_SHRINK);
    rows->pack_start(*update_row, Gtk::PACK_SHRINK);

    return rows;
}

/* The canvas half: a titled frame around a scrolling picture, with the
 * right-click export menu. */
Gtk::Frame *make_canvas_frame(const char *title, PPUImageArea &canvas, Gtk::ScrolledWindow &scroller,
                              Gtk::Menu &menu, Gtk::Window &owner,
                              const std::function<void()> &export_png)
{
    auto frame = Gtk::manage(new Gtk::Frame(title));
    canvas.signal_menu_requested.connect(
        [&menu](GdkEventButton *event) { menu.popup_at_pointer((GdkEvent *)event); });
    scroller.add(canvas);
    scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    frame->add(scroller);

    auto export_item = Gtk::manage(new Gtk::MenuItem(_("Export to PNG..."), true));
    export_item->signal_activate().connect([export_png] { export_png(); });
    menu.append(*export_item);
    menu.attach_to_widget(owner);
    menu.show_all();
    return frame;
}

Gtk::Label *form_label(const char *text)
{
    return Gtk::manage(new Gtk::Label(text, Gtk::ALIGN_START));
}

void setup_info_label(Gtk::Label &label)
{
    label.set_xalign(0.0f);
    label.set_yalign(0.0f);
    label.set_selectable(true);
}

/* ---- GB Tile Viewer ------------------------------------------------------ */

/* Emulation > GB-PPU > GB Tile Viewer, as win32's CGBTileViewerDlg offers it:
 * the 384 tiles of one VRAM bank through a DMG, CGB or SGB palette. */
class Snes9xGBTileViewer : public PPUViewerWindow
{
  public:
    Snes9xGBTileViewer();

  protected:
    void refresh() override;

  private:
    void show_tile_info();
    void export_tiles();

    gv::TileViewerState state;
    pv::Image picture;
    int selected_tile = -1;

    Gtk::Box outer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton grid_check{ _("Show Grid") };
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::ComboBoxText palette_combo;
    Gtk::SpinButton index_spin;
    Gtk::ComboBoxText bank_combo;
    Gtk::SpinButton width_spin;
    Gtk::Label tile_info;
    PPUImageArea canvas;
    Gtk::ScrolledWindow scroller;
    Gtk::Menu canvas_menu;
};

Snes9xGBTileViewer::Snes9xGBTileViewer()
    : PPUViewerWindow(_("GB Tile Viewer"))
{
    set_default_size(640, 620);

    grid_check.signal_toggled().connect([this] {
        state.show_grid = grid_check.get_active();
        refresh();
    });
    // GB tiles are small, so this one opens at 3x.
    side.pack_start(*make_update_rows(zoom_combo, 3, canvas, auto_update_check, auto_update,
                                      refresh_button, [this] { refresh(); }, &grid_check),
                    Gtk::PACK_SHRINK);

    auto form = Gtk::manage(new Gtk::Grid());
    form->set_row_spacing(4);
    form->set_column_spacing(6);
    int row = 0;

    form->attach(*form_label(_("Palette:")), 0, row, 1, 1);
    for (int i = 0; i < gv::kPaletteModeCount; i++)
        palette_combo.append(_(gv::palette_mode_name(i)));
    palette_combo.set_active(state.palette_mode);
    palette_combo.signal_changed().connect([this] {
        const int index = palette_combo.get_active_row_number();
        if (index < 0)
            return;
        state.palette_mode = index;
        refresh();
    });
    form->attach(palette_combo, 1, row++, 3, 1);

    form->attach(*form_label(_("Index:")), 0, row, 1, 1);
    index_spin.set_adjustment(Gtk::Adjustment::create(state.palette_index, 0, 7, 1, 1));
    index_spin.signal_value_changed().connect([this] {
        state.palette_index = index_spin.get_value_as_int();
        refresh();
    });
    form->attach(index_spin, 1, row, 1, 1);
    form->attach(*form_label(_("Bank:")), 2, row, 1, 1);
    bank_combo.append("0");
    bank_combo.append("1");
    bank_combo.set_active(state.bank);
    bank_combo.signal_changed().connect([this] {
        const int index = bank_combo.get_active_row_number();
        if (index < 0)
            return;
        state.bank = index;
        show_tile_info();
        refresh();
    });
    form->attach(bank_combo, 3, row++, 1, 1);

    form->attach(*form_label(_("Width:")), 0, row, 1, 1);
    width_spin.set_adjustment(Gtk::Adjustment::create(state.width_tiles, 8, 32, 1, 8));
    width_spin.signal_value_changed().connect([this] {
        state.width_tiles = width_spin.get_value_as_int();
        selected_tile = -1;
        show_tile_info();
        refresh();
    });
    form->attach(width_spin, 1, row++, 1, 1);
    side.pack_start(*form, Gtk::PACK_SHRINK);

    setup_info_label(tile_info);
    side.pack_start(tile_info, Gtk::PACK_SHRINK);

    side.set_size_request(240, -1);
    outer.set_border_width(6);
    outer.pack_start(side, Gtk::PACK_SHRINK);

    canvas.signal_pixel_clicked.connect([this](int x, int y) {
        const int tile = gv::tile_at_pixel(state, x, y);
        if (tile < 0)
            return;
        selected_tile = tile;
        show_tile_info();
    });
    outer.pack_start(*make_canvas_frame(_("Tiles"), canvas, scroller, canvas_menu, *this,
                                        [this] { export_tiles(); }),
                     Gtk::PACK_EXPAND_WIDGET);
    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseGBTileViewer();
        return true;
    });

    show_all();
    refresh();
}

void Snes9xGBTileViewer::show_tile_info()
{
    if (selected_tile < 0)
    {
        tile_info.set_text("");
        return;
    }
    tile_info.set_text(Glib::ustring::compose(_("Tile #%1  bank %2\nVRAM %3"), selected_tile,
                                              state.bank, hex(gv::tile_address(selected_tile), 4)));
}

void Snes9xGBTileViewer::refresh()
{
    gv::render_tiles(state, picture);
    canvas.set_image(picture);
}

void Snes9xGBTileViewer::export_tiles()
{
    // The grid is a reading aid, not part of the tiles.
    pv::Image exported;
    gv::TileViewerState without_grid = state;
    without_grid.show_grid = false;
    gv::render_tiles(without_grid, exported);
    export_image(exported, "gb_tiles.png");
}

/* ---- GB Tilemap Viewer --------------------------------------------------- */

/* Emulation > GB-PPU > GB Tilemap Viewer, as win32's CGBTilemapViewerDlg
 * offers it: the BG or window map, with the screen the game is scrolled to. */
class Snes9xGBTilemapViewer : public PPUViewerWindow
{
  public:
    Snes9xGBTilemapViewer();

  protected:
    void refresh() override;

  private:
    void show_cell_info();
    void export_map();

    gv::TilemapViewerState state;
    pv::Image picture;
    int cell_x = -1, cell_y = -1; // source pixel of the picked cell

    Gtk::Box outer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton grid_check{ _("Show Grid") };
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::ComboBoxText map_combo;
    Gtk::ComboBoxText tile_data_combo;
    Gtk::CheckButton viewport_check{ _("Show viewport") };
    Gtk::ComboBoxText background_combo;
    Gtk::Label cell_info;
    PPUImageArea canvas;
    Gtk::ScrolledWindow scroller;
    Gtk::Menu canvas_menu;
};

Snes9xGBTilemapViewer::Snes9xGBTilemapViewer()
    : PPUViewerWindow(_("GB Tilemap Viewer"))
{
    set_default_size(800, 600);

    grid_check.signal_toggled().connect([this] {
        state.show_grid = grid_check.get_active();
        refresh();
    });
    side.pack_start(*make_update_rows(zoom_combo, 2, canvas, auto_update_check, auto_update,
                                      refresh_button, [this] { refresh(); }, &grid_check),
                    Gtk::PACK_SHRINK);

    auto form = Gtk::manage(new Gtk::Grid());
    form->set_row_spacing(4);
    form->set_column_spacing(6);
    int row = 0;

    form->attach(*form_label(_("Map:")), 0, row, 1, 1);
    for (int i = 0; i < gv::kMapCount; i++)
        map_combo.append(_(gv::map_name(i)));
    map_combo.set_active(state.map);
    map_combo.signal_changed().connect([this] {
        const int index = map_combo.get_active_row_number();
        if (index < 0)
            return;
        state.map = index;
        show_cell_info();
        refresh();
    });
    form->attach(map_combo, 1, row++, 1, 1);

    form->attach(*form_label(_("Tile Data:")), 0, row, 1, 1);
    for (int i = 0; i < gv::kTileDataCount; i++)
        tile_data_combo.append(_(gv::tile_data_name(i)));
    tile_data_combo.set_active(state.tile_data);
    tile_data_combo.signal_changed().connect([this] {
        const int index = tile_data_combo.get_active_row_number();
        if (index < 0)
            return;
        state.tile_data = index;
        refresh();
    });
    form->attach(tile_data_combo, 1, row++, 1, 1);

    viewport_check.set_active(state.show_viewport);
    viewport_check.signal_toggled().connect([this] {
        state.show_viewport = viewport_check.get_active();
        refresh();
    });
    form->attach(viewport_check, 0, row++, 2, 1);

    form->attach(*form_label(_("Background:")), 0, row, 1, 1);
    for (int i = 0; i < gv::kMapBackgroundCount; i++)
        background_combo.append(_(gv::map_background_name(i)));
    background_combo.set_active(state.background);
    background_combo.signal_changed().connect([this] {
        const int index = background_combo.get_active_row_number();
        if (index < 0)
            return;
        state.background = index;
        refresh();
    });
    form->attach(background_combo, 1, row++, 1, 1);
    side.pack_start(*form, Gtk::PACK_SHRINK);

    setup_info_label(cell_info);
    side.pack_start(cell_info, Gtk::PACK_SHRINK);

    side.set_size_request(240, -1);
    outer.set_border_width(6);
    outer.pack_start(side, Gtk::PACK_SHRINK);

    canvas.signal_pixel_clicked.connect([this](int x, int y) {
        cell_x = x;
        cell_y = y;
        show_cell_info();
    });
    outer.pack_start(*make_canvas_frame(_("Map"), canvas, scroller, canvas_menu, *this,
                                        [this] { export_map(); }),
                     Gtk::PACK_EXPAND_WIDGET);
    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseGBTilemapViewer();
        return true;
    });

    show_all();
    refresh();
}

void Snes9xGBTilemapViewer::show_cell_info()
{
    if (cell_x < 0)
    {
        cell_info.set_text("");
        return;
    }
    const gv::TilemapCell cell = gv::tilemap_cell(state, cell_x, cell_y);
    if (cell.cgb)
        cell_info.set_text(Glib::ustring::compose(
            _("Cell %1,%2  map %3\nTile %4  attr %5 (pal %6 bank %7)"), cell.x, cell.y,
            hex(cell.map_address, 4), (int)cell.tile, hex(cell.attr, 2), cell.attr & 7,
            (cell.attr >> 3) & 1));
    else
        cell_info.set_text(Glib::ustring::compose(_("Cell %1,%2  map %3\nTile %4"), cell.x, cell.y,
                                                  hex(cell.map_address, 4), (int)cell.tile));
}

void Snes9xGBTilemapViewer::refresh()
{
    gv::render_tilemap(state, picture);
    canvas.set_image(picture);
    show_cell_info();
}

void Snes9xGBTilemapViewer::export_map()
{
    // Grid and viewport are reading aids, not part of the map.
    pv::Image exported;
    gv::TilemapViewerState plain = state;
    plain.show_grid = false;
    plain.show_viewport = false;
    gv::render_tilemap(plain, exported, true);
    export_image(exported, "gb_tilemap.png");
}

/* ---- GB Sprite Viewer ---------------------------------------------------- */

/* Emulation > GB-PPU > GB Sprite Viewer, as win32's CGBSpriteViewerDlg offers
 * it: all 40 objects where OAM puts them, the visible screen outlined, and one
 * picked out for its attributes. */
class Snes9xGBSpriteViewer : public PPUViewerWindow
{
  public:
    Snes9xGBSpriteViewer();

  protected:
    void refresh() override;

  private:
    void show_sprite_info();
    void export_sprites();

    gv::SpriteViewerState state;
    pv::Image picture;

    Gtk::Box outer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::Box side{ Gtk::ORIENTATION_VERTICAL, 6 };
    Gtk::ComboBoxText zoom_combo;
    Gtk::CheckButton auto_update_check{ _("Auto update") };
    Gtk::Button refresh_button{ _("Refresh") };
    Gtk::CheckButton viewport_check{ _("Show screen") };
    Gtk::SpinButton sprite_spin;
    Gtk::ComboBoxText background_combo;
    Gtk::Label sprite_info;
    PPUImageArea canvas;
    Gtk::ScrolledWindow scroller;
    Gtk::Menu canvas_menu;
};

Snes9xGBSpriteViewer::Snes9xGBSpriteViewer()
    : PPUViewerWindow(_("GB Sprite Viewer"))
{
    set_default_size(800, 600);

    viewport_check.set_active(state.show_viewport);
    viewport_check.signal_toggled().connect([this] {
        state.show_viewport = viewport_check.get_active();
        refresh();
    });
    side.pack_start(*make_update_rows(zoom_combo, 2, canvas, auto_update_check, auto_update,
                                      refresh_button, [this] { refresh(); }, &viewport_check),
                    Gtk::PACK_SHRINK);

    auto form = Gtk::manage(new Gtk::Grid());
    form->set_row_spacing(4);
    form->set_column_spacing(6);
    int row = 0;

    form->attach(*form_label(_("Sprite:")), 0, row, 1, 1);
    sprite_spin.set_adjustment(Gtk::Adjustment::create(state.selected, 0, gv::kSpriteCount - 1, 1, 8));
    sprite_spin.signal_value_changed().connect([this] {
        state.selected = sprite_spin.get_value_as_int();
        refresh();
    });
    form->attach(sprite_spin, 1, row++, 1, 1);

    form->attach(*form_label(_("Background:")), 0, row, 1, 1);
    for (int i = 0; i < pv::kViewerBgCount; i++)
        background_combo.append(_(pv::viewer_bg_name(i)));
    background_combo.set_active(state.background);
    background_combo.signal_changed().connect([this] {
        const int index = background_combo.get_active_row_number();
        if (index < 0)
            return;
        state.background = index;
        refresh();
    });
    form->attach(background_combo, 1, row++, 1, 1);
    side.pack_start(*form, Gtk::PACK_SHRINK);

    setup_info_label(sprite_info);
    side.pack_start(sprite_info, Gtk::PACK_SHRINK);

    side.set_size_request(240, -1);
    outer.set_border_width(6);
    outer.pack_start(side, Gtk::PACK_SHRINK);

    outer.pack_start(*make_canvas_frame(_("Sprites"), canvas, scroller, canvas_menu, *this,
                                        [this] { export_sprites(); }),
                     Gtk::PACK_EXPAND_WIDGET);
    add(outer);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseGBSpriteViewer();
        return true;
    });

    show_all();
    refresh();
}

void Snes9xGBSpriteViewer::show_sprite_info()
{
    const gv::SpriteInfo s = gv::sprite_info(state.selected);
    if (!s.valid)
    {
        sprite_info.set_text("");
        return;
    }
    const Glib::ustring flips = Glib::ustring((s.flags & 0x40) ? "Yflip " : "") +
                                ((s.flags & 0x20) ? "Xflip " : "") +
                                ((s.flags & 0x80) ? "BGprio" : "");
    const Glib::ustring position = Glib::ustring::compose(
        _("Sprite %1\nX %2 Y %3 (screen %4,%5)\nTile %6  flags %7"), state.selected, s.x, s.y,
        s.x - 8, s.y - 16, s.tile, hex(s.flags, 2));
    if (s.cgb)
        sprite_info.set_text(position + Glib::ustring::compose(_("\npal %1 bank %2  %3"),
                                                               s.flags & 7, (s.flags >> 3) & 1, flips));
    else
        sprite_info.set_text(position +
                             Glib::ustring::compose(_("\nOBP%1  %2"), (s.flags >> 4) & 1, flips));
}

void Snes9xGBSpriteViewer::refresh()
{
    gv::render_sprites(state, picture);
    canvas.set_image(picture);
    show_sprite_info();
}

void Snes9xGBSpriteViewer::export_sprites()
{
    // The screen outline is a reading aid, and Transparent exports as alpha 0.
    pv::Image exported;
    gv::SpriteViewerState plain = state;
    plain.show_viewport = false;
    gv::render_sprites(plain, exported, true);
    export_image(exported, "gb_sprites.png");
}

std::unique_ptr<Snes9xGBTileViewer> gb_tile_viewer;
std::unique_ptr<Snes9xGBTilemapViewer> gb_tilemap_viewer;
std::unique_ptr<Snes9xGBSpriteViewer> gb_sprite_viewer;

/* May run from the window's own delete-event handler, where destroying it is
 * unsafe: let the main loop finish with it first. */
template <typename Viewer>
void close_viewer(std::unique_ptr<Viewer> &viewer)
{
    if (!viewer)
        return;
    viewer->shutdown();
    viewer->hide();
    auto *window = viewer.release();
    Glib::signal_idle().connect_once([window] { delete window; });
}

} // namespace

void S9xShowGBTileViewer()
{
    if (!gb_tile_viewer)
        gb_tile_viewer = std::make_unique<Snes9xGBTileViewer>();
    gb_tile_viewer->present();
}

void S9xShowGBTilemapViewer()
{
    if (!gb_tilemap_viewer)
        gb_tilemap_viewer = std::make_unique<Snes9xGBTilemapViewer>();
    gb_tilemap_viewer->present();
}

void S9xShowGBSpriteViewer()
{
    if (!gb_sprite_viewer)
        gb_sprite_viewer = std::make_unique<Snes9xGBSpriteViewer>();
    gb_sprite_viewer->present();
}

void S9xCloseGBTileViewer()
{
    close_viewer(gb_tile_viewer);
}

void S9xCloseGBTilemapViewer()
{
    close_viewer(gb_tilemap_viewer);
}

void S9xCloseGBSpriteViewer()
{
    close_viewer(gb_sprite_viewer);
}
