/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "gtk_compat.h"
#include "gtk_s9x.h"
#include "gtk_builder_window.h"
#include <map>
#include <vector>

void snes9x_preferences_create(Snes9xConfig *config);
void snes9x_preferences_open(Snes9xWindow *window, int page = -1);

class Snes9xPreferences final : public GtkBuilderWindow
{
  public:
    explicit Snes9xPreferences(Snes9xConfig *config);
    void show();
    void bindings_to_dialog(int joypad);
    int get_focused_binding();
    void store_binding(const char *string, Binding binding);
    int combo_value(const std::string &driver_name);
    void focus_next();
    void swap_with();
    void clear_binding(const char *name);
    void reset_current_joypad();
    void load_ntsc_settings();
    void store_ntsc_settings();
    void calibration_dialog();
    void connect_signals();
    void input_rate_changed();
    void update_sgb_volume_enable_state();
    void update_gb_blend_enable_state();
    void populate_gb_cameras();
    void update_gb_camera_enable_state();
    bool key_pressed(GdkEventKey *event);
    bool key_released(GdkEventKey *event);
    void shader_select();
    void game_data_browse(const std::string &folder);
    void about_dialog();

    Snes9xConfig *config;
    bool awaiting_key;
    bool polling_joystick;
    // Track the Regular slider's value so it can drag the per-source SGB
    // sliders by the same delta. Mirrors the Win32 dialog's behavior.
    int prev_regular_volume = 100;
    bool suppress_volume_sync = false;
    std::array<JoypadBinding, NUM_JOYPADS> pad;
    std::array<Binding, NUM_EMU_LINKS> shortcut;

    // The joypad page's pad picture: held buttons lit, the hovered and edited ones ringed.
    void track_joystick(const Binding &binding, bool pressed);
    void update_pad_picture();
    std::vector<Binding> held_joystick;

  private:
    bool draw_pad_picture(const Cairo::RefPtr<Cairo::Context> &cr);
    bool bound_on_current_pad(const Binding &binding);
    int pad_picture_held();
    int pad_picture_marked();
    int pad_picture_button_at(double x, double y);
    void set_pad_picture_hover(int button);
    void build_pad_style_menu();

    std::map<guint16, Binding> held_keys;	// by hardware keycode
    int pad_picture_lit = 0;
    int pad_picture_outlined = 0;
    int pad_picture_hover = 0;
    Cairo::RefPtr<Cairo::ImageSurface> pad_picture_base;	// scaled to the area, unlit
    int pad_picture_base_style = -1;
    uint32_t pad_picture_base_label = 0;	// the theme's text colour, for the L and R labels
    Gtk::Menu pad_style_menu;
    Gtk::RadioMenuItem *pad_style_items[3] = {};
    bool syncing_pad_style = false;

    void get_settings_from_dialog();
    void move_settings_to_dialog();
    Glib::ustring format_sound_input_rate_value(double);
};