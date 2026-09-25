/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include "gtk_compat.h"
#include "common/video/ppu_viewer.hpp"

#include <functional>

/* Emulation > S-PPU: the three PPU inspectors win32 puts at the top of that
 * menu. One window each; asking for one that is already open brings it
 * forward instead of making a second. */
void S9xShowTileViewer();
void S9xShowTilemapViewer();
void S9xShowSpriteViewer();

void S9xCloseTileViewer();
void S9xCloseTilemapViewer();
void S9xCloseSpriteViewer();

/* Emulation > GB-PPU: the same three for the Game Boy core (gtk_gb_viewer.cpp). */
void S9xShowGBTileViewer();
void S9xShowGBTilemapViewer();
void S9xShowGBSpriteViewer();

void S9xCloseGBTileViewer();
void S9xCloseGBTilemapViewer();
void S9xCloseGBSpriteViewer();
/* Shuts every open viewer, for when the main window goes away. */
void S9xClosePPUViewers();

/* The picture half of a viewer: paints one ppuviewer::Image at a whole-number
 * zoom and reports clicks back in source pixels. Sized to the picture, so a
 * Gtk::ScrolledWindow around it pans; win32's dialogs drag-pan instead. */
class PPUImageArea : public Gtk::DrawingArea
{
  public:
    PPUImageArea();

    void set_image(const ppuviewer::Image &image);
    void set_zoom(int zoom);
    /* Scale to the widget at the largest whole multiple that fits, centered,
     * rather than to the zoom factor. What the sprite preview wants. */
    void set_fit_to_widget(bool fit);
    void set_background(ppuviewer::Pixel color);

    /* Left click, in source pixels. */
    sigc::signal<void, int, int> signal_pixel_clicked;
    /* Right click, for the export menu. */
    sigc::signal<void, GdkEventButton *> signal_menu_requested;

  protected:
    bool on_draw(const Cairo::RefPtr<Cairo::Context> &cr) override;
    bool on_button_press_event(GdkEventButton *event) override;

  private:
    int fitted_scale() const;
    void apply_size();

    Cairo::RefPtr<Cairo::ImageSurface> surface;
    int zoom = 1;
    bool fit_to_widget = false;
    ppuviewer::Pixel background = 0xFF000000u;
};

/* CGRAM as a 16x16 grid of swatches; clicking one picks the palette offset
 * the Tile Viewer indexes tiles through. */
class PPUPaletteArea : public Gtk::DrawingArea
{
  public:
    PPUPaletteArea();

    void set_colors(const ppuviewer::Pixel colors[256]);
    void set_selected(int index);

    sigc::signal<void, int> signal_selection_changed;

  protected:
    bool on_draw(const Cairo::RefPtr<Cairo::Context> &cr) override;
    bool on_button_press_event(GdkEventButton *event) override;

  private:
    ppuviewer::Pixel colors[256] = {};
    int selected = 0;
};

/* What the three viewer windows share: a title, a place beside the main
 * window, and a timer that re-reads the PPU while Auto update is on. The GTK
 * port runs the core on this same main loop, so a render here always sees a
 * settled frame. */
class PPUViewerWindow : public Gtk::Window
{
  public:
    explicit PPUViewerWindow(const char *title);
    ~PPUViewerWindow() override;

    /* Stops the timer; safe to call twice. */
    void shutdown();

  protected:
    void set_update_interval(int milliseconds);
    /* Re-read the PPU and repaint. Called by the timer and the Refresh button. */
    virtual void refresh() = 0;

    /* Save-as dialog plus the PNG write, with a message dialog if it fails. */
    void export_image(const ppuviewer::Image &image, const char *default_name);

    bool auto_update = true;

  private:
    bool tick();

    sigc::connection tick_connection;
    int interval = 1000 / 60;
};

/* A button that keeps firing while it is held down, the way win32's address
 * step buttons do, so holding one walks through the source. */
class PPURepeatButton : public Gtk::Button
{
  public:
    PPURepeatButton(const Glib::ustring &label, const std::function<void()> &action);
    ~PPURepeatButton() override;

  protected:
    bool on_button_press_event(GdkEventButton *event) override;
    bool on_button_release_event(GdkEventButton *event) override;

  private:
    std::function<void()> action;
    sigc::connection repeat;
};

/* Adds "1x".."9x" to a combo and selects `zoom`. */
void S9xFillZoomCombo(Gtk::ComboBoxText &combo, int zoom);
