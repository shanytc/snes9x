/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_ppu_viewer.h"
#include "gtk_s9x.h"
#include "gtk_s9xwindow.h"

#include <algorithm>
#include <cstring>

namespace
{

void set_source_rgb(const Cairo::RefPtr<Cairo::Context> &cr, ppuviewer::Pixel color)
{
    cr->set_source_rgb(((color >> 16) & 0xFF) / 255.0, ((color >> 8) & 0xFF) / 255.0,
                       (color & 0xFF) / 255.0);
}

} // namespace

/* ---- PPUImageArea -------------------------------------------------------- */

PPUImageArea::PPUImageArea()
{
    add_events(Gdk::BUTTON_PRESS_MASK);
}

void PPUImageArea::set_image(const ppuviewer::Image &image)
{
    if (image.empty())
    {
        surface.clear();
        apply_size();
        queue_draw();
        return;
    }

    if (!surface || surface->get_width() != image.width || surface->get_height() != image.height)
        surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, image.width, image.height);

    surface->flush();
    unsigned char *data = surface->get_data();
    const int stride = surface->get_stride();
    for (int y = 0; y < image.height; y++)
        memcpy(data + (size_t)y * stride, image.row(y), (size_t)image.width * 4);
    surface->mark_dirty();

    apply_size();
    queue_draw();
}

void PPUImageArea::set_zoom(int new_zoom)
{
    zoom = std::max(1, new_zoom);
    apply_size();
    queue_draw();
}

void PPUImageArea::set_fit_to_widget(bool fit)
{
    fit_to_widget = fit;
    apply_size();
    queue_draw();
}

void PPUImageArea::set_background(ppuviewer::Pixel color)
{
    background = color;
    queue_draw();
}

void PPUImageArea::apply_size()
{
    if (fit_to_widget || !surface)
        return;
    set_size_request(surface->get_width() * zoom, surface->get_height() * zoom);
}

int PPUImageArea::fitted_scale() const
{
    if (!surface)
        return 1;
    return std::max(1, std::min(get_allocated_width() / std::max(1, surface->get_width()),
                                get_allocated_height() / std::max(1, surface->get_height())));
}

bool PPUImageArea::on_draw(const Cairo::RefPtr<Cairo::Context> &cr)
{
    set_source_rgb(cr, background);
    cr->paint();

    if (!surface)
        return true;

    const int scale = fit_to_widget ? fitted_scale() : zoom;
    const int x = fit_to_widget ? (get_allocated_width() - surface->get_width() * scale) / 2 : 0;
    const int y = fit_to_widget ? (get_allocated_height() - surface->get_height() * scale) / 2 : 0;

    auto pattern = Cairo::SurfacePattern::create(surface);
    /* Whole-number zoom of 8x8 tiles: anything but nearest would blur them. */
    pattern->set_filter(Cairo::FILTER_NEAREST);

    cr->save();
    cr->translate(x, y);
    cr->scale(scale, scale);
    cr->set_source(pattern);
    cr->paint();
    cr->restore();
    return true;
}

bool PPUImageArea::on_button_press_event(GdkEventButton *event)
{
    if (event->button == 3)
    {
        signal_menu_requested.emit(event);
        return true;
    }
    if (event->button != 1 || !surface)
        return false;

    const int scale = fit_to_widget ? fitted_scale() : zoom;
    const int origin_x =
        fit_to_widget ? (get_allocated_width() - surface->get_width() * scale) / 2 : 0;
    const int origin_y =
        fit_to_widget ? (get_allocated_height() - surface->get_height() * scale) / 2 : 0;

    const int x = ((int)event->x - origin_x) / scale;
    const int y = ((int)event->y - origin_y) / scale;
    if (x >= 0 && x < surface->get_width() && y >= 0 && y < surface->get_height())
        signal_pixel_clicked.emit(x, y);
    return true;
}

/* ---- PPUPaletteArea ------------------------------------------------------ */

PPUPaletteArea::PPUPaletteArea()
{
    add_events(Gdk::BUTTON_PRESS_MASK);
    set_size_request(-1, 80);
    for (auto &color : colors)
        color = 0xFF000000u;
}

void PPUPaletteArea::set_colors(const ppuviewer::Pixel new_colors[256])
{
    std::copy(new_colors, new_colors + 256, colors);
    queue_draw();
}

void PPUPaletteArea::set_selected(int index)
{
    selected = std::clamp(index, 0, 255);
    queue_draw();
}

bool PPUPaletteArea::on_draw(const Cairo::RefPtr<Cairo::Context> &cr)
{
    const double cell_w = get_allocated_width() / 16.0;
    const double cell_h = get_allocated_height() / 16.0;

    for (int i = 0; i < 256; i++)
    {
        set_source_rgb(cr, colors[i]);
        cr->rectangle((i % 16) * cell_w, (i / 16) * cell_h, cell_w, cell_h);
        cr->fill();
    }

    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->set_line_width(1.0);
    cr->rectangle((selected % 16) * cell_w + 0.5, (selected / 16) * cell_h + 0.5, cell_w - 1,
                  cell_h - 1);
    cr->stroke();
    return true;
}

bool PPUPaletteArea::on_button_press_event(GdkEventButton *event)
{
    if (event->button != 1)
        return false;

    const int column =
        std::clamp((int)(event->x * 16 / std::max(1, get_allocated_width())), 0, 15);
    const int row = std::clamp((int)(event->y * 16 / std::max(1, get_allocated_height())), 0, 15);
    set_selected(row * 16 + column);
    signal_selection_changed.emit(selected);
    return true;
}

/* ---- PPUViewerWindow ----------------------------------------------------- */

PPUViewerWindow::PPUViewerWindow(const char *title)
{
    set_title(title);
    if (top_level && top_level->window)
        set_transient_for(*top_level->window.get());
    set_update_interval(interval);
}

PPUViewerWindow::~PPUViewerWindow()
{
    shutdown();
}

void PPUViewerWindow::shutdown()
{
    tick_connection.disconnect();
}

void PPUViewerWindow::set_update_interval(int milliseconds)
{
    interval = std::max(1, milliseconds);
    tick_connection.disconnect();
    tick_connection = Glib::signal_timeout().connect(sigc::mem_fun(*this, &PPUViewerWindow::tick),
                                                     interval);
}

bool PPUViewerWindow::tick()
{
    if (!auto_update)
        return true;

    auto gdk_window = get_window();
    if (gdk_window && (gdk_window->get_state() & Gdk::WINDOW_STATE_ICONIFIED))
        return true;

    refresh();
    return true;
}

void PPUViewerWindow::export_image(const ppuviewer::Image &image, const char *default_name)
{
    if (image.empty())
        return;

    Gtk::FileChooserDialog dialog(*this, _("Export to PNG..."), Gtk::FILE_CHOOSER_ACTION_SAVE);
    dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("_Save"), Gtk::RESPONSE_ACCEPT);
    dialog.set_do_overwrite_confirmation(true);
    dialog.set_current_name(default_name);

    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("PNG Image"));
    filter->add_pattern("*.png");
    dialog.add_filter(filter);

    if (dialog.run() != Gtk::RESPONSE_ACCEPT)
        return;

    std::string filename = dialog.get_filename();
    dialog.hide();

    if (filename.size() < 4 ||
        filename.compare(filename.size() - 4, 4, ".png") != 0)
        filename += ".png";

    if (!ppuviewer::write_png(filename, image))
        Gtk::MessageDialog(*this, _("Failed to save PNG."), false, Gtk::MESSAGE_ERROR,
                           Gtk::BUTTONS_CLOSE, true)
            .run();
}

/* ---- PPURepeatButton ----------------------------------------------------- */

namespace
{
constexpr int kRepeatDelayMs = 250;
constexpr int kRepeatRateMs = 50;
} // namespace

PPURepeatButton::PPURepeatButton(const Glib::ustring &label, const std::function<void()> &action_)
    : Gtk::Button(label), action(action_)
{
    signal_clicked().connect([this] { action(); });
}

PPURepeatButton::~PPURepeatButton()
{
    repeat.disconnect();
}

bool PPURepeatButton::on_button_press_event(GdkEventButton *event)
{
    const bool handled = Gtk::Button::on_button_press_event(event);
    if (event->button == 1)
    {
        repeat.disconnect();
        repeat = Glib::signal_timeout().connect(
            [this] {
                repeat.disconnect();
                repeat = Glib::signal_timeout().connect(
                    [this] {
                        action();
                        return true;
                    },
                    kRepeatRateMs);
                return false;
            },
            kRepeatDelayMs);
    }
    return handled;
}

bool PPURepeatButton::on_button_release_event(GdkEventButton *event)
{
    repeat.disconnect();
    return Gtk::Button::on_button_release_event(event);
}

void S9xFillZoomCombo(Gtk::ComboBoxText &combo, int zoom)
{
    for (int i = 1; i <= 9; i++)
        combo.append(std::to_string(i) + "x");
    combo.set_active(std::clamp(zoom, 1, 9) - 1);
}

void S9xClosePPUViewers()
{
    S9xCloseTileViewer();
    S9xCloseTilemapViewer();
    S9xCloseSpriteViewer();
    S9xCloseGBTileViewer();
    S9xCloseGBTilemapViewer();
    S9xCloseGBSpriteViewer();
}
