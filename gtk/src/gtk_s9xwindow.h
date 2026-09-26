/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include "gtk_compat.h"

#include "port.h"
#include "gtk_builder_window.h"
#include "gtk_config.h"

class Snes9xWindow : public GtkBuilderWindow
{
  public:
    explicit Snes9xWindow(Snes9xConfig *config);

    struct AcceleratorEntry
    {
        Gtk::MenuItem *item;
        unsigned int key;
        Gdk::ModifierType modifiers;
    };

    /* Pause related functions */
    void pause_from_focus_change();
    void unpause_from_focus_change();
    void focus_notify(bool state);
    void pause_from_user();
    void unpause_from_user();
    bool is_paused();
    void propagate_pause_state();

    /* Fullscreen functions */
    void enter_fullscreen_mode();
    void leave_fullscreen_mode();
    void toggle_fullscreen_mode();
    void set_bypass_compositor(bool bypass);
    void set_custom_video_mode(bool enable);

    /* Cursor modifying functions */
    void show_mouse_cursor();
    void hide_mouse_cursor();
    void toggle_grab_mouse();
    // Emulation -> Game Boy Model, shared by the menu items and their
    // hotkeys so both apply the same gates and reload the same way.
    void set_gb_boot_policy(int policy);
    void open_bios_manager();
    bool reload_loaded_game();
    void center_mouse();

    /* Rom-related functions */
    std::string open_rom_dialog(bool run = true);
    void save_state_dialog();
    void load_state_dialog();
    void state_preview_dialog(bool is_save);
    void configure_widgets();
    void save_spc_dialog();
    void save_screenshot();
    void save_sram();
    void save_memory_pack();
    bool try_open_rom(const std::string &filename);
    std::string prompt_rename_msu1_pack(const std::string &filename);
    /* File->Movie Play/Record/Stop and AVI Recording, as on win32. */
    void play_movie_dialog();
    void record_movie_dialog();
    void stop_movie();
    void toggle_avi_recording();
    void update_movie_menu();
    void movie_seek_dialog();
    void open_multicart_dialog();
    void open_voicekun_dialog();
    /* Emulation -> Super Famicom Box / Nintendo Super System front panels,
     * shared by the menus and their hotkeys. */
    void create_arcade_menus();
    void refresh_arcade_menus();
    void insert_coin(int slot);
    void sfcbox_set_keyswitch(int panel_pos);
    void nss_pulse(uint16_t buttons, bool game_only);
    void nss_game(int slot);
    void nss_eject(int slot);
    void nss_toggle_dip(int sw);
    void open_superdisc_dialog();
    void eject_superdisc();
    void show_rom_info();
    void show_color_correction_dialog();

    /* GTK-base-related functions */
    void show();
    void set_menu_item_selected(const char *name);
    void update_controller_option_menu();
    void apply_window_icon();
    void choose_window_icon(int index);
    void set_write_icon_to_launcher(bool enabled);
    void sync_launcher_icon();
    void update_icon_menu();
    void set_mouseable_area(int x, int y, int width, int height);
    void set_accelerator_to_binding(const char *name,
                                        const char *binding);
    void set_accelerator_to_binding(Gtk::MenuItem *item,
                                        const char *binding);
    void reset_screensaver();
    void build_state_menus();
    void update_accelerators();
    void toggle_ui();
    void resize_to_multiple(int factor);
    void resize_viewport(int width, int height);
    /* View->Always on Top / Lock Screen Resize, as on win32's Video menu. */
    void apply_always_on_top();
    void apply_resize_lock();
    void set_window_size_pin(int width, int height);
    /* The window size Change Size gives a multiple, and which multiple the
     * window currently has (0 for none of them). */
    void size_for_multiple(int factor, int *width, int *height);
    int current_size_multiple();
    bool draw(const Cairo::RefPtr<Cairo::Context> &cr);
    void setup_splash();
    double get_refresh_rate();
    int get_auto_input_rate();
    void connect_signals();
    bool event_key(GdkEventKey *event);
    bool button_press(GdkEventButton *event);
    bool button_release(GdkEventButton *event);
    bool motion_notify(GdkEventMotion *event);

    cairo_t *get_cairo();
    void release_cairo();

    Snes9xConfig *config;
    bool refreshing_bios_menu = false;
    bool refreshing_runahead_menu = false;
    bool refreshing_controller_menu = false;
    bool refreshing_icon_menu = false;
    /* Ticking a check menu item also fires its activate handler, so the
     * menu-open syncs raise this while they stamp the current state. */
    bool syncing_menu = false;
    // Emulation -> GB-PPU: the Game Boy core's viewers and layer switches.
    void create_gbppu_menu();
    Gtk::MenuItem *gbppu_item = nullptr;
    Gtk::CheckMenuItem *gb_layer_items[3] = {};
    Gtk::MenuItem *sfcbox_item = nullptr;
    Gtk::RadioMenuItem *sfcbox_keyswitch_items[5] = {};
    Gtk::CheckMenuItem *sfcbox_backdrop_item = nullptr;
    Gtk::CheckMenuItem *sfcbox_english_item = nullptr;
    Gtk::MenuItem *nss_item = nullptr;
    Gtk::MenuItem *nss_game_items[3] = {};
    Gtk::MenuItem *nss_eject_items[3] = {};
    Gtk::MenuItem *nss_game_only_items[3] = {};
    Gtk::MenuItem *nss_dips_item = nullptr;
    Gtk::CheckMenuItem *nss_dip_items[8] = {};
    // Emulation -> PowerFest '94 / Campus Challenge '92 session timer.
    Gtk::MenuItem *event_item = nullptr;
    Gtk::RadioMenuItem *event_minutes_items[16] = {};
    Gtk::RadioMenuItem *event_display_items[3] = {};
    std::string event_title_suffix;
    void set_event_timer(int minutes, int display);
    bool update_event_title();
    std::string rom_title();
    int user_pause, sys_pause;
    int last_width, last_height;
    int mouse_region_x, mouse_region_y;
    int mouse_region_width, mouse_region_height;
    int autovrr_saved_frameskip;
    int autovrr_saved_sound_input_rate;
    bool autovrr_saved_sync_to_vblank;
    bool autovrr_saved_sound_sync;
    int fullscreen_state;
    int maximized_state;
    bool focused;
    bool paused_from_focus_loss;
    double snes_mouse_x, snes_mouse_y;
    double gdk_mouse_x, gdk_mouse_y;
    bool mouse_grabbed;
    GdkPixbuf *icon, *splash;
    Gtk::DrawingArea *drawing_area;
    Gtk::RecentChooserMenu *recent_menu;
    cairo_t *cr;
    bool cairo_owned;
    Glib::RefPtr<Gdk::DrawingContext> gdk_drawing_context;
    Glib::RefPtr<Gtk::AccelGroup> accel_group;
    std::vector<AcceleratorEntry> accelerators;

    /* File->Save/Load State slot items, indexed by bank * SAVE_SLOTS_PER_BANK
     * + slot. Built at runtime since there are too many for the .ui file. */
    std::array<Gtk::MenuItem *, NUM_SAVE_SLOTS> save_state_items;
    std::array<Gtk::MenuItem *, NUM_SAVE_SLOTS> load_state_items;

    unsigned int last_key_pressed_keyval;
    GdkEventType last_key_pressed_type;
};

typedef struct gtk_splash_t
{
    unsigned int width;
    unsigned int height;
    unsigned int bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */
    unsigned char pixel_data[256 * 224 * 3 + 1];
} gtk_splash_t;
