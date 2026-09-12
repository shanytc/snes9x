/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gtk_audio_waveform.h"
#include "gtk_compat.h"
#include "gtk_s9x.h"
#include "gtk_sound.h"

#include "common/audio/audio_waveform.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace aw = audiowave;

namespace
{

struct Rect
{
    int x, y, w, h;
    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/* [R]ecord, [M]ute and [S]olo button rects inside a row: the right side of
 * the header, vertically centered. Shared by the painter and the hit test. */
Rect rec_rect(const Rect &row)
{
    return { row.x + aw::kHeaderWidth - 84, row.y + row.h / 2 - 9, 22, 18 };
}

Rect mute_rect(const Rect &row)
{
    return { row.x + aw::kHeaderWidth - 58, row.y + row.h / 2 - 9, 22, 18 };
}

Rect solo_rect(const Rect &row)
{
    return { row.x + aw::kHeaderWidth - 32, row.y + row.h / 2 - 9, 22, 18 };
}

void set_color(const Cairo::RefPtr<Cairo::Context> &cr, aw::Color c)
{
    cr->set_source_rgb(c.r / 255.0, c.g / 255.0, c.b / 255.0);
}

void fill_rect(const Cairo::RefPtr<Cairo::Context> &cr, const Rect &r, aw::Color c)
{
    set_color(cr, c);
    cr->rectangle(r.x, r.y, r.w, r.h);
    cr->fill();
}

void frame_rect(const Cairo::RefPtr<Cairo::Context> &cr, const Rect &r, aw::Color c)
{
    set_color(cr, c);
    cr->set_line_width(1.0);
    cr->rectangle(r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    cr->stroke();
}

enum Align
{
    kAlignLeft,
    kAlignCenter
};

/* Text vertically centered in r, clipped to it. */
void draw_text(const Cairo::RefPtr<Cairo::Context> &cr, const Glib::RefPtr<Pango::Layout> &layout,
               const Rect &r, const std::string &text, aw::Color c, Align align = kAlignLeft)
{
    if (text.empty())
        return;
    layout->set_text(text);
    int tw, th;
    layout->get_pixel_size(tw, th);
    const int x = (align == kAlignCenter) ? r.x + (r.w - tw) / 2 : r.x;
    cr->save();
    cr->rectangle(r.x, r.y, r.w, r.h);
    cr->clip();
    set_color(cr, c);
    cr->move_to(x, r.y + (r.h - th) / 2);
    layout->show_in_cairo_context(cr);
    cr->restore();
}

} // namespace

class Snes9xAudioWaveform : public Gtk::Window
{
  public:
    Snes9xAudioWaveform();
    ~Snes9xAudioWaveform() override;
    /* Stops capturing and honors reset-on-close; safe to call twice. */
    void shutdown();

  private:
    bool draw(const Cairo::RefPtr<Cairo::Context> &cr);
    void draw_row(const Cairo::RefPtr<Cairo::Context> &cr, const Rect &r, int src,
                  const int16_t *lr, int n, bool show_axis);
    bool button_press(GdkEventButton *event);
    bool tick();
    void apply_refresh();
    void toggle_recording(int src);
    /* Rows split the height evenly; the last one takes the remainder. */
    Rect row_rect(int index, int panels) const;

    aw::Viewer viewer;
    Gtk::Box vbox{ Gtk::ORIENTATION_VERTICAL };
    Gtk::DrawingArea area;
    Gtk::Box footer{ Gtk::ORIENTATION_HORIZONTAL, 8 };
    Gtk::CheckButton reset_check;
    Gtk::Label zoom_label;
    Gtk::ComboBoxText zoom_combo;
    Gtk::Label rate_label;
    Gtk::ComboBoxText rate_combo;
    Gtk::CheckButton nerd_check;
    sigc::connection tick_connection;
    Glib::RefPtr<Pango::Layout> layout;
    std::vector<int16_t> buffer;
    std::vector<int> y_min, y_max;
};

Snes9xAudioWaveform::Snes9xAudioWaveform()
    : reset_check(_("reset channels on close")),
      zoom_label(_("zoom")),
      rate_label(_("rate")),
      nerd_check(_("stats for nerds"))
{
    set_title(_("Audio Waveform  (click SPC/GB to expand channels)"));
    set_default_size(800, 560);
    if (top_level && top_level->window)
        set_transient_for(*top_level->window.get());

    // The MIX row's [M] is the preferences dialog's "Mute sound output".
    aw::set_master_mute_hooks(
        [] { return gui_config->mute_sound; },
        [](bool muted) {
            gui_config->mute_sound = muted;
            S9xPortSoundReinit();
        });

    buffer.resize(aw::kSnapshotFrames * 2);

    layout = area.create_pango_layout("");
    auto font = area.get_pango_context()->get_font_description();
    font.set_absolute_size(12 * PANGO_SCALE);
    layout->set_font_description(font);

    area.set_size_request(-1, 120);
    area.add_events(Gdk::BUTTON_PRESS_MASK);
    area.signal_draw().connect(sigc::mem_fun(*this, &Snes9xAudioWaveform::draw));
    area.signal_button_press_event().connect(sigc::mem_fun(*this, &Snes9xAudioWaveform::button_press));

    reset_check.set_active(viewer.reset_on_close);
    reset_check.signal_toggled().connect([this] { viewer.reset_on_close = reset_check.get_active(); });

    for (int i = 0; i < aw::kScaleCount; i++)
        zoom_combo.append(aw::kScaleLabels[i]);
    zoom_combo.set_active(viewer.scale);
    zoom_combo.signal_changed().connect([this] {
        const int index = zoom_combo.get_active_row_number();
        if (index >= 0)
            viewer.scale = index;
        area.queue_draw();
    });

    for (int i = 0; i < aw::kRefreshCount; i++)
        rate_combo.append(aw::kRefreshOptions[i].label);
    rate_combo.set_active(viewer.refresh);
    rate_combo.signal_changed().connect([this] {
        const int index = rate_combo.get_active_row_number();
        if (index >= 0)
            viewer.refresh = index;
        apply_refresh();
    });

    nerd_check.set_active(viewer.nerd_stats);
    nerd_check.signal_toggled().connect([this] {
        viewer.nerd_stats = nerd_check.get_active();
        area.queue_draw();
    });

    footer.set_border_width(4);
    footer.pack_start(reset_check, Gtk::PACK_SHRINK);
    footer.pack_start(zoom_label, Gtk::PACK_SHRINK, 8);
    footer.pack_start(zoom_combo, Gtk::PACK_SHRINK);
    footer.pack_start(rate_label, Gtk::PACK_SHRINK, 8);
    footer.pack_start(rate_combo, Gtk::PACK_SHRINK);
    footer.pack_start(nerd_check, Gtk::PACK_SHRINK, 8);

    vbox.pack_start(area, Gtk::PACK_EXPAND_WIDGET);
    vbox.pack_start(footer, Gtk::PACK_SHRINK);
    add(vbox);

    signal_delete_event().connect([](GdkEventAny *) -> bool {
        S9xCloseAudioWaveformWindow();
        return true;
    });

    viewer.open();
    apply_refresh();
    show_all();
}

Snes9xAudioWaveform::~Snes9xAudioWaveform()
{
    shutdown();
}

void Snes9xAudioWaveform::shutdown()
{
    tick_connection.disconnect();
    viewer.close();
}

void Snes9xAudioWaveform::apply_refresh()
{
    tick_connection.disconnect();
    tick_connection = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Snes9xAudioWaveform::tick),
                                                     aw::kRefreshOptions[viewer.refresh].ms);
}

bool Snes9xAudioWaveform::tick()
{
    viewer.recorder.pump();
    auto gdk_window = get_window();
    if (!gdk_window || !(gdk_window->get_state() & Gdk::WINDOW_STATE_ICONIFIED))
        area.queue_draw();
    return true;
}

Rect Snes9xAudioWaveform::row_rect(int index, int panels) const
{
    const int width = area.get_allocated_width();
    const int height = area.get_allocated_height();
    const int h = height / panels;
    return { 0, index * h, width, (index == panels - 1) ? height - index * h : h };
}

bool Snes9xAudioWaveform::draw(const Cairo::RefPtr<Cairo::Context> &cr)
{
    const int width = area.get_allocated_width();
    const int height = area.get_allocated_height();
    fill_rect(cr, { 0, 0, width, height }, { 18, 18, 20 });

    int order[aw::kSourceCount];
    const int panels = viewer.build_order(order);
    if (height / panels <= 0)
        return true;

    viewer.begin_paint();
    for (int i = 0; i < panels; i++)
    {
        const int src = order[i];
        const int n = viewer.fetch(src, buffer.data(), aw::kSnapshotFrames);
        draw_row(cr, row_rect(i, panels), src, buffer.data(), n, i == panels - 1);
    }
    viewer.end_paint();
    return true;
}

void Snes9xAudioWaveform::draw_row(const Cairo::RefPtr<Cairo::Context> &cr, const Rect &r, int src,
                                   const int16_t *lr, int n, bool show_axis)
{
    const aw::TrackInfo &track = aw::kTracks[src];
    const bool group = aw::is_group(src);
    const bool expanded = (src == aw::kSPC) ? viewer.spc_open : viewer.gb_open;
    const bool user_muted = aw::is_muted(src);
    const bool soloed = aw::is_soloed(src);
    const bool audible = aw::is_audible(src);
    const int cy = r.y + r.h / 2;

    // Header strip: color tab, disclosure triangle on group rows, name.
    const Rect header{ r.x, r.y, aw::kHeaderWidth, r.h };
    fill_rect(cr, header, group ? aw::Color{ 56, 56, 60 } : aw::Color{ 40, 40, 44 });
    fill_rect(cr, { header.x, header.y, 5, header.h }, track.color);

    if (group)
    {
        const int ix = header.x + 10;
        set_color(cr, aw::tint(track.color, 30));
        if (expanded)
        {
            cr->move_to(ix, cy - 3);
            cr->line_to(ix + 10, cy - 3);
            cr->line_to(ix + 5, cy + 4);
        }
        else
        {
            cr->move_to(ix + 2, cy - 5);
            cr->line_to(ix + 2, cy + 5);
            cr->line_to(ix + 9, cy);
        }
        cr->close_path();
        cr->fill();
    }

    draw_text(cr, layout, { header.x + (group ? 26 : 30), cy - 8, 40, 16 }, track.name,
              audible ? aw::Color{ 225, 225, 228 } : aw::Color{ 130, 130, 135 });

    auto button = [&](const Rect &b, const char *label, bool on, aw::Color fill_on,
                      aw::Color frame_on, aw::Color text_on) {
        fill_rect(cr, b, on ? fill_on : aw::Color{ 52, 52, 56 });
        frame_rect(cr, b, on ? frame_on : aw::Color{ 80, 80, 86 });
        draw_text(cr, layout, b, label, on ? text_on : aw::Color{ 150, 150, 156 }, kAlignCenter);
    };
    const bool rec_this = viewer.recorder.source() == src;
    button(rec_rect(r), "R", rec_this, { 200, 40, 40 }, { 240, 90, 90 }, { 255, 255, 255 });
    button(mute_rect(r), "M", user_muted, { 96, 150, 250 }, { 140, 180, 255 }, { 20, 30, 60 });
    // The MIX row is the master bus, so it has no solo.
    if (src != aw::kMix)
        button(solo_rect(r), "S", soloed, { 230, 192, 62 }, { 250, 220, 120 }, { 60, 45, 10 });

    // Waveform lane, dimmed when the track doesn't reach the output, whether
    // by its own mute or by someone else's solo.
    const int axis_h = show_axis ? 16 : 0;
    const Rect lane{ r.x + aw::kHeaderWidth, r.y, r.w - aw::kHeaderWidth, r.h };
    const Rect body{ lane.x, lane.y, lane.w, lane.h - axis_h };
    fill_rect(cr, body, aw::shade(track.color, audible ? 28 : 14));
    const int mid_y = body.y + body.h / 2;
    set_color(cr, aw::shade(track.color, audible ? 44 : 24));
    cr->set_line_width(1.0);
    cr->move_to(body.x, mid_y + 0.5);
    cr->line_to(body.x + body.w, mid_y + 0.5);
    cr->stroke();

    viewer.build_envelope(lr, n, body.w, body.h, y_min, y_max);
    if (!y_min.empty())
    {
        set_color(cr, audible ? aw::tint(track.color, 55) : aw::shade(track.color, 52));
        for (size_t i = 0; i < y_min.size(); i++)
        {
            const double x = body.x + (int)i + 0.5;
            cr->move_to(x, body.y + y_min[i]);
            cr->line_to(x, body.y + y_max[i] + 1);
        }
        cr->stroke();
    }

    // L/R meters under the buttons: -48..0 dB with 1 s peak-hold ticks.
    if (r.h >= 46)
    {
        float level[2], peak[2];
        viewer.meter_levels(src, lr, n, level, peak);
        const int mx0 = header.x + 8;
        const int mw = header.w - 16;
        for (int ch = 0; ch < 2; ch++)
        {
            const int y0 = cy + 13 + ch * 4;
            fill_rect(cr, { mx0, y0, mw, 3 }, { 30, 30, 32 });
            float from = 0.0f;
            for (int s = 0; s < 3 && from < level[ch]; s++)
            {
                const float to = std::min(level[ch], aw::kMeterSegments[s].to);
                if (to > from)
                {
                    const int x0 = mx0 + (int)(from * mw);
                    const int x1 = mx0 + (int)(to * mw);
                    fill_rect(cr, { x0, y0, x1 - x0, 3 }, aw::kMeterSegments[s].color);
                }
                from = aw::kMeterSegments[s].to;
            }
            if (peak[ch] > 0.01f)
                fill_rect(cr, { mx0 + (int)(peak[ch] * (mw - 2)), y0, 2, 3 }, { 220, 220, 224 });
        }
    }

    // Clip-label style info at the top-left of the lane, status at the right.
    draw_text(cr, layout, { lane.x + 6, lane.y + 2, lane.w - 12, 16 }, viewer.info_text(src),
              aw::tint(track.color, audible ? 70 : 25));
    if (user_muted)
        draw_text(cr, layout, { lane.x + lane.w - 52, lane.y + 2, 50, 16 }, _("muted"),
                  aw::tint(track.color, 40));
    if (rec_this)
    {
        const int seconds = viewer.recorder.elapsed_seconds();
        char text[32];
        snprintf(text, sizeof(text), "REC %d:%02d", seconds / 60, seconds % 60);
        draw_text(cr, layout, { lane.x + lane.w - 140, lane.y + 2, 86, 16 }, text, { 235, 80, 80 });
    }

    if (show_axis)
    {
        const Rect axis{ lane.x, body.y + body.h, lane.w, axis_h };
        fill_rect(cr, axis, { 18, 18, 20 });
        const int rate = viewer.sample_rate(src);
        for (int i = 1; i < 7; i++)
        {
            const int x = lane.x + (lane.w * i) / 7;
            draw_text(cr, layout, { x - 22, axis.y + 1, 44, 14 }, aw::Viewer::axis_label(i, n, rate),
                      { 140, 140, 145 }, kAlignCenter);
        }
        draw_text(cr, layout, { lane.x + 2, axis.y + 1, 20, 14 }, "0", { 140, 140, 145 });
    }

    set_color(cr, { 20, 20, 22 });
    cr->move_to(r.x, r.y + r.h - 0.5);
    cr->line_to(r.x + r.w, r.y + r.h - 0.5);
    cr->stroke();
}

bool Snes9xAudioWaveform::button_press(GdkEventButton *event)
{
    if (event->button != 1)
        return false;
    int order[aw::kSourceCount];
    const int panels = viewer.build_order(order);
    const int h = area.get_allocated_height() / panels;
    if (h <= 0)
        return true;
    const int px = (int)event->x;
    const int py = (int)event->y;
    const int index = std::clamp(py / h, 0, panels - 1);
    const int src = order[index];
    const Rect row = row_rect(index, panels);

    if (rec_rect(row).contains(px, py))
        toggle_recording(src);
    else if (mute_rect(row).contains(px, py))
        aw::toggle_mute(src);
    else if (src != aw::kMix && solo_rect(row).contains(px, py))
        aw::toggle_solo(src);
    // Anywhere else on a group row toggles its expansion.
    else if (src == aw::kSPC)
        viewer.spc_open = !viewer.spc_open;
    else if (src == aw::kGB)
        viewer.gb_open = !viewer.gb_open;
    else
        return true;
    area.queue_draw();
    return true;
}

void Snes9xAudioWaveform::toggle_recording(int src)
{
    auto &recorder = viewer.recorder;
    // One track at a time; clicking the armed one stops and saves.
    if (recorder.source() == src)
    {
        recorder.stop();
        Gtk::FileChooserDialog dialog(*this, _("Save WAV file..."), Gtk::FILE_CHOOSER_ACTION_SAVE);
        dialog.add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("_Save"), Gtk::RESPONSE_ACCEPT);
        dialog.set_do_overwrite_confirmation(true);
        dialog.set_current_name(std::string(aw::kTracks[src].name) + ".wav");
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("WAV audio"));
        filter->add_pattern("*.wav");
        filter->add_pattern("*.WAV");
        dialog.add_filter(filter);

        const int result = dialog.run();
        dialog.hide();
        if (result != Gtk::RESPONSE_ACCEPT)
        {
            recorder.discard();
            return;
        }
        if (!recorder.write_wav(dialog.get_filename()))
        {
            std::string message = _("Couldn't save WAV file:");
            message += " " + dialog.get_filename();
            Gtk::MessageDialog(*this, message, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_CLOSE, true).run();
        }
    }
    else if (recorder.source() < 0)
    {
        recorder.start(src);
    }
}

static std::unique_ptr<Snes9xAudioWaveform> waveform_window;

void S9xToggleAudioWaveformWindow()
{
    if (waveform_window)
        S9xCloseAudioWaveformWindow();
    else
        waveform_window = std::make_unique<Snes9xAudioWaveform>();
}

bool S9xAudioWaveformWindowOpen()
{
    return waveform_window != nullptr;
}

void S9xCloseAudioWaveformWindow()
{
    if (!waveform_window)
        return;
    waveform_window->shutdown();
    waveform_window->hide();
    // This may run from the window's own delete-event handler, where
    // destroying it is unsafe: let the main loop finish with it first.
    auto *window = waveform_window.release();
    Glib::signal_idle().connect_once([window] { delete window; });
}
