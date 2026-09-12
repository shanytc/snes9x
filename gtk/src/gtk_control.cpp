/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <fcntl.h>

#include "SDL_joystick.h"
#include "fscompat.h"
#include "gtk_s9x.h"
#include "gtk_config.h"
#include "gtk_control.h"
#include "gtk_file.h"

#include "snes9x.h"
#include "controls.h"
#include "crosshairs.h"
#include "memmap.h"
#include "movie.h"
#include "display.h"
#include "gfx.h"

#ifdef RETROACHIEVEMENTS_SUPPORT
#include "retroachievements.h"
#endif

const BindingLink b_links[] =
{
        /* Joypad-specific bindings. "Joypad# " will be prepended */
        { "b_up",        "Up"          },
        { "b_down",      "Down"        },
        { "b_left",      "Left"        },
        { "b_right",     "Right"       },
        { "b_start",     "Start"       },
        { "b_select",    "Select"      },
        { "b_a",         "A"           },
        { "b_b",         "B"           },
        { "b_x",         "X"           },
        { "b_y",         "Y"           },
        { "b_l",         "L"           },
        { "b_r",         "R"           },
        { "b_a_turbo",   "Turbo A"     },
        { "b_b_turbo",   "Turbo B"     },
        { "b_x_turbo",   "Turbo X"     },
        { "b_y_turbo",   "Turbo Y"     },
        { "b_l_turbo",   "Turbo L"     },
        { "b_r_turbo",   "Turbo R"     },
        { "b_a_sticky",  "Sticky A"    },
        { "b_b_sticky",  "Sticky B"    },
        { "b_x_sticky",  "Sticky X"    },
        { "b_y_sticky",  "Sticky Y"    },
        { "b_l_sticky",  "Sticky L"    },
        { "b_r_sticky",  "Sticky R"    },

        /* Emulator based bindings */
        { "b_open_rom",            "GTK_open_rom"      },
        { "b_enable_turbo",        "EmuTurbo"          },
        { "b_toggle_turbo",        "ToggleEmuTurbo"    },
        { "b_pause",               "GTK_pause"         },
        { "b_decrease_frame_rate", "DecFrameRate"      },
        { "b_increase_frame_rate", "IncFrameRate"      },
        { "b_decrease_frame_time", "DecFrameTime"      },
        { "b_increase_frame_time", "IncFrameTime"      },
        { "b_hardware_reset",      "Reset"             },
        { "b_soft_reset",          "SoftReset"         },
        { "b_quit",                "GTK_quit"          },
        { "b_bg_layer_0",          "ToggleBG0"         },
        { "b_bg_layer_1",          "ToggleBG1"         },
        { "b_bg_layer_2",          "ToggleBG2"         },
        { "b_bg_layer_3",          "ToggleBG3"         },
        { "b_sprites",             "ToggleSprites"     },
        { "toggle_backdrop",       "ToggleBackdrop"    },
        { "b_screenshot",          "Screenshot"        },
        { "b_fullscreen",          "GTK_fullscreen"    },
        { "b_state_save_current",  "GTK_state_save_current" },
        { "b_state_load_current",  "GTK_state_load_current" },
        { "b_state_increment_save","GTK_state_increment_save" },
        { "b_state_decrement_load","GTK_state_decrement_load" },
        { "b_state_increment",     "GTK_state_increment" },
        { "b_state_decrement",     "GTK_state_decrement" },
        { "b_state_bank_increment","GTK_state_bank_increment" },
        { "b_state_bank_decrement","GTK_state_bank_decrement" },
        { "b_save_0",              "QuickSave000"      },
        { "b_save_1",              "QuickSave001"      },
        { "b_save_2",              "QuickSave002"      },
        { "b_save_3",              "QuickSave003"      },
        { "b_save_4",              "QuickSave004"      },
        { "b_save_5",              "QuickSave005"      },
        { "b_save_6",              "QuickSave006"      },
        { "b_save_7",              "QuickSave007"      },
        { "b_save_8",              "QuickSave008"      },
        { "b_save_9",              "QuickSave009"      },
        { "b_load_0",              "QuickLoad000"      },
        { "b_load_1",              "QuickLoad001"      },
        { "b_load_2",              "QuickLoad002"      },
        { "b_load_3",              "QuickLoad003"      },
        { "b_load_4",              "QuickLoad004"      },
        { "b_load_5",              "QuickLoad005"      },
        { "b_load_6",              "QuickLoad006"      },
        { "b_load_7",              "QuickLoad007"      },
        { "b_load_8",              "QuickLoad008"      },
        { "b_load_9",              "QuickLoad009"      },
        { "b_select_slot_0",       "GTK_state_select_0" },
        { "b_select_slot_1",       "GTK_state_select_1" },
        { "b_select_slot_2",       "GTK_state_select_2" },
        { "b_select_slot_3",       "GTK_state_select_3" },
        { "b_select_slot_4",       "GTK_state_select_4" },
        { "b_select_slot_5",       "GTK_state_select_5" },
        { "b_select_slot_6",       "GTK_state_select_6" },
        { "b_select_slot_7",       "GTK_state_select_7" },
        { "b_select_slot_8",       "GTK_state_select_8" },
        { "b_select_slot_9",       "GTK_state_select_9" },
        { "b_state_dialog_save",   "GTK_state_dialog_save" },
        { "b_state_dialog_load",   "GTK_state_dialog_load" },
        { "b_state_file_save",     "GTK_state_file_save" },
        { "b_state_file_load",     "GTK_state_file_load" },
        { "b_sound_channel_0",     "SoundChannel0"     },
        { "b_sound_channel_1",     "SoundChannel1"     },
        { "b_sound_channel_2",     "SoundChannel2"     },
        { "b_sound_channel_3",     "SoundChannel3"     },
        { "b_sound_channel_4",     "SoundChannel4"     },
        { "b_sound_channel_5",     "SoundChannel5"     },
        { "b_sound_channel_6",     "SoundChannel6"     },
        { "b_sound_channel_7",     "SoundChannel7"     },
        { "b_all_sound_channels",  "SoundChannelsOn"   },
        { "b_save_spc",            "GTK_save_spc"      },
        { "b_begin_recording_movie", "BeginRecordingMovie" },
        { "b_stop_recording_movie", "EndRecordingMovie" },
        { "b_load_movie",          "LoadMovie" },
        { "b_seek_to_frame",       "GTK_seek_to_frame" },
        { "b_swap_controllers",    "GTK_swap_controllers" },
        { "b_rewind",              "GTK_rewind"        },
        { "b_grab_mouse",          "GTK_grab_mouse"    },
        { "b_gb_model_gb",         "GTK_gb_model_0"    },
        { "b_gb_model_gbc",        "GTK_gb_model_1"    },
        { "b_gb_model_sgb",        "GTK_gb_model_2"    },
        { "b_gb_model_sgb2",       "GTK_gb_model_3"    },
        { "b_gb_model_sgbc",       "GTK_gb_model_4"    },
        { "b_bios_manager",        "GTK_bios_manager"  },

        { nullptr, nullptr }
};

// Adding a binding without moving NUM_EMU_LINKS leaves the shortcut array short
// and walks focus_next() onto the terminator, so make it a build error instead.
static_assert(sizeof(b_links) / sizeof(b_links[0]) ==
                  NUM_JOYPAD_LINKS + NUM_EMU_LINKS + 1,
              "b_links is out of step with NUM_JOYPAD_LINKS + NUM_EMU_LINKS");

/* Where the page breaks occur in the preferences pane */
const int b_breaks[] =
{
        12, /* End of main buttons */
        24, /* End of turbo/sticky buttons */
        35, /* End of base emulator buttons */
        43, /* End of Graphic options */
        85, /* End of save/load states */
        94, /* End of sound buttons */
        NUM_JOYPAD_LINKS + NUM_EMU_LINKS, /* End of miscellaneous buttons */
        -1
};

static int joystick_lock = 0;

bool S9xPollButton(uint32 id, bool *pressed)
{
    return true;
}

bool S9xPollAxis(uint32 id, int16 *value)
{
    return true;
}

// A light gun can only point at the screen, unlike the free-roaming mouse.
static bool using_gun()
{
    for (int i = 0; i < 2; i++)
    {
        enum controllers ctl;
        int8_t id1, id2, id3, id4;
        S9xGetController(i, &ctl, &id1, &id2, &id3, &id4);
        if (ctl == CTL_SUPERSCOPE || ctl == CTL_JUSTIFIER || ctl == CTL_MACSRIFLE)
            return true;
    }

    return false;
}

bool S9xPollPointer(uint32 id, int16 *x, int16 *y)
{
    if (using_gun())
    {
        top_level->snes_mouse_x = std::clamp(top_level->snes_mouse_x, 0.0, 256.0);
        top_level->snes_mouse_y = std::clamp(top_level->snes_mouse_y, 0.0, 239.0);
    }

    *x = top_level->snes_mouse_x;
    *y = top_level->snes_mouse_y;

    return true;
}

bool S9xIsMousePluggedIn()
{
    enum controllers ctl;
    int8 id1, id2, id3, id4;

    for (int i = 0; i <= 1; i++)
    {
        S9xGetController(i, &ctl, &id1, &id2, &id3, &id4);
        if (ctl == CTL_MOUSE || ctl == CTL_SUPERSCOPE || ctl == CTL_JUSTIFIER || ctl == CTL_MACSRIFLE)
            return true;
    }

    return false;
}

/* Mirrors win32's ChangeInputDevice(): raise only the selected device's
 * master flag and seat the devices where they sit on real hardware (a mouse
 * or pad in port 1, a gun or multitap in port 2). Pad N is driven by the
 * "Joypad N" bindings. */
void S9xApplyControllerOption()
{
    Settings.MouseMaster = false;
    Settings.JustifierMaster = false;
    Settings.SuperScopeMaster = false;
    Settings.MultiPlayer5Master = false;
    Settings.MacsRifleMaster = false;

    switch (gui_config->controller_option)
    {
    case CONTROLLER_MOUSE:
        Settings.MouseMaster = true;
        S9xSetController(0, CTL_MOUSE, 0, 0, 0, 0);
        S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
        break;
    case CONTROLLER_MOUSE_SWAPPED:
        Settings.MouseMaster = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_MOUSE, 1, 0, 0, 0);
        break;
    case CONTROLLER_SUPERSCOPE:
        Settings.SuperScopeMaster = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_SUPERSCOPE, 0, 0, 0, 0);
        break;
    case CONTROLLER_MULTITAP5:
        Settings.MultiPlayer5Master = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_MP5, 1, 2, 3, 4);
        break;
    case CONTROLLER_MULTITAP8:
        Settings.MultiPlayer5Master = true;
        S9xSetController(0, CTL_MP5, 0, 1, 2, 3);
        S9xSetController(1, CTL_MP5, 4, 5, 6, 7);
        break;
    case CONTROLLER_JUSTIFIER:
        Settings.JustifierMaster = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_JUSTIFIER, 0, 0, 0, 0);
        break;
    case CONTROLLER_DUAL_JUSTIFIERS:
        Settings.JustifierMaster = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_JUSTIFIER, 1, 0, 0, 0);
        break;
    case CONTROLLER_MACSRIFLE:
        Settings.MacsRifleMaster = true;
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_MACSRIFLE, 0, 0, 0, 0);
        break;
    case CONTROLLER_JOYPADS:
    default:
        S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
        S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
        break;
    }

    S9xApplySuperScopeCrosshair();
}

void S9xApplySuperScopeCrosshair()
{
    if (gui_config->superscope_crosshair_visible)
        S9xSetControllerCrosshair(X_SUPERSCOPE, 2, "White", "Black");
    else
        S9xSetControllerCrosshair(X_SUPERSCOPE, 0, "Trans", "Trans");
}

void S9xSetControllerOption(int option)
{
    if (option < 0 || option >= NUM_CONTROLLER_OPTIONS)
        return;

    gui_config->controller_option = option;
    // A manual pick is the new baseline: nothing to restore on the next ROM.
    gui_config->controller_option_before_rom = -1;
    S9xApplyControllerOption();
    gui_config->rebind_keys();
}

bool S9xControllerOptionValid(int option)
{
    return (gui_config->valid_controller_options >> option) & 1;
}

void S9xAutoDetectControllerOption()
{
    // Port of win32's S9xPostRomInit(): undo whatever the previous ROM
    // forced, then let this ROM's NSRT header (or the M.A.C.S. rifle title)
    // choose the devices and restrict the menu to the ones it supports.
    if (S9xMovieActive())
        return;

    const int applied = gui_config->controller_option;

    if (gui_config->controller_option_before_rom >= 0)
        gui_config->controller_option = gui_config->controller_option_before_rom;

    const int previous = gui_config->controller_option;
    int &option = gui_config->controller_option;
    int &valid = gui_config->valid_controller_options;
    valid = 0xffff;

    if (!Settings.DisableGameSpecificHacks && strncmp(Memory.ROMName, "MAC:Basic Rifle", 15) == 0)
        option = CONTROLLER_MACSRIFLE;

    if (!strncmp((const char *)Memory.NSRTHeader + 24, "NSRT", 4))
    {
        switch (Memory.NSRTHeader[29])
        {
        default: // unknown or unsupported
            break;
        case 0x00: // Gamepad / Gamepad
            option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS);
            break;
        case 0x10: // Mouse / Gamepad
            option = CONTROLLER_MOUSE;
            valid = (1 << CONTROLLER_MOUSE);
            break;
        case 0x20: // Mouse_or_Gamepad / Gamepad
            if (option == CONTROLLER_MOUSE_SWAPPED)
                option = CONTROLLER_MOUSE;
            if (option != CONTROLLER_MOUSE)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS) | (1 << CONTROLLER_MOUSE);
            break;
        case 0x01: // Gamepad / Mouse
            option = CONTROLLER_MOUSE_SWAPPED;
            valid = (1 << CONTROLLER_MOUSE_SWAPPED);
            break;
        case 0x22: // Mouse_or_Gamepad / Mouse_or_Gamepad
            if (option != CONTROLLER_MOUSE && option != CONTROLLER_MOUSE_SWAPPED)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS) | (1 << CONTROLLER_MOUSE) | (1 << CONTROLLER_MOUSE_SWAPPED);
            break;
        case 0x03: // Gamepad / Superscope
            option = CONTROLLER_SUPERSCOPE;
            valid = (1 << CONTROLLER_SUPERSCOPE);
            break;
        case 0x04: // Gamepad / Gamepad_or_Superscope
            if (option == CONTROLLER_JUSTIFIER || option == CONTROLLER_DUAL_JUSTIFIERS)
                option = CONTROLLER_SUPERSCOPE;
            if (option != CONTROLLER_SUPERSCOPE)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS) | (1 << CONTROLLER_SUPERSCOPE);
            break;
        case 0x05: // Gamepad / Justifier
            if (option != CONTROLLER_DUAL_JUSTIFIERS)
                option = CONTROLLER_JUSTIFIER;
            valid = (1 << CONTROLLER_JUSTIFIER) | (1 << CONTROLLER_DUAL_JUSTIFIERS);
            break;
        case 0x06: // Gamepad / Multitap_or_Gamepad
            option = CONTROLLER_MULTITAP5;
            valid = (1 << CONTROLLER_MULTITAP5) | (1 << CONTROLLER_JOYPADS);
            break;
        case 0x66: // Multitap_or_Gamepad / Multitap_or_Gamepad
            option = CONTROLLER_MULTITAP8;
            valid = (1 << CONTROLLER_MULTITAP8) | (1 << CONTROLLER_MULTITAP5) | (1 << CONTROLLER_JOYPADS);
            break;
        case 0x24: // Gamepad_or_Mouse / Gamepad_or_Superscope
            if (option == CONTROLLER_JUSTIFIER || option == CONTROLLER_DUAL_JUSTIFIERS)
                option = CONTROLLER_SUPERSCOPE;
            if (option != CONTROLLER_SUPERSCOPE && option != CONTROLLER_MOUSE)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS) | (1 << CONTROLLER_MOUSE) | (1 << CONTROLLER_SUPERSCOPE);
            break;
        case 0x27: // Gamepad_or_Mouse / Gamepad_or_Mouse_or_Superscope
            if (option == CONTROLLER_JUSTIFIER || option == CONTROLLER_DUAL_JUSTIFIERS)
                option = CONTROLLER_SUPERSCOPE;
            if (option != CONTROLLER_SUPERSCOPE && option != CONTROLLER_MOUSE && option != CONTROLLER_MOUSE_SWAPPED)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_JOYPADS) | (1 << CONTROLLER_MOUSE) | (1 << CONTROLLER_MOUSE_SWAPPED) | (1 << CONTROLLER_SUPERSCOPE);
            break;
        case 0x08: // Gamepad / Mouse_or_Multitap_or_Gamepad
            if (option == CONTROLLER_MOUSE)
                option = CONTROLLER_MOUSE_SWAPPED;
            if (option == CONTROLLER_MULTITAP8)
                option = CONTROLLER_MULTITAP5;
            if (option != CONTROLLER_MULTITAP5 && option != CONTROLLER_MOUSE_SWAPPED)
                option = CONTROLLER_JOYPADS;
            valid = (1 << CONTROLLER_MOUSE_SWAPPED) | (1 << CONTROLLER_MULTITAP5) | (1 << CONTROLLER_JOYPADS);
            break;
        }
    }

    // Remember what (if anything) the devices were forced away from.
    gui_config->controller_option_before_rom = previous;

    if (option != applied)
    {
        S9xApplyControllerOption();
        gui_config->rebind_keys();
    }
}

/* The joypad buttons that also drive the device in the neighbouring port,
 * as win32's S9xPollButton() lets them: pad 1 works the mouse or Justifier
 * that replaces it, pad 2 fires the gun or second mouse in port 2. In Dual
 * Justifiers mode pad 2's D-pad also steers the second gun, which is aimed
 * through pseudo pointer 1 (see rebind_keys). */
struct DeviceButton
{
    int option;
    int player;
    const char *button;
    const char *command;
};

static const DeviceButton device_buttons[] = {
    { CONTROLLER_MOUSE, 0, "A", "Mouse1 L" },
    { CONTROLLER_MOUSE, 0, "L", "Mouse1 L" },
    { CONTROLLER_MOUSE, 0, "B", "Mouse1 R" },
    { CONTROLLER_MOUSE, 0, "R", "Mouse1 R" },

    { CONTROLLER_MOUSE_SWAPPED, 1, "A", "Mouse2 L" },
    { CONTROLLER_MOUSE_SWAPPED, 1, "L", "Mouse2 L" },
    { CONTROLLER_MOUSE_SWAPPED, 1, "B", "Mouse2 R" },
    { CONTROLLER_MOUSE_SWAPPED, 1, "R", "Mouse2 R" },

    { CONTROLLER_SUPERSCOPE, 1, "A", "Superscope Fire" },
    { CONTROLLER_SUPERSCOPE, 1, "L", "Superscope Fire" },
    { CONTROLLER_SUPERSCOPE, 1, "B", "Superscope Cursor" },
    { CONTROLLER_SUPERSCOPE, 1, "R", "Superscope Cursor" },
    { CONTROLLER_SUPERSCOPE, 1, "Y", "Superscope ToggleTurbo" },
    { CONTROLLER_SUPERSCOPE, 1, "Start", "Superscope Pause" },
    { CONTROLLER_SUPERSCOPE, 1, "Select", "Superscope Pause" },
    { CONTROLLER_SUPERSCOPE, 1, "X", "Superscope AimOffscreen" },

    { CONTROLLER_JUSTIFIER, 0, "A", "Justifier1 Trigger" },
    { CONTROLLER_JUSTIFIER, 0, "L", "Justifier1 Trigger" },
    { CONTROLLER_JUSTIFIER, 0, "B", "Justifier1 Start" },
    { CONTROLLER_JUSTIFIER, 0, "R", "Justifier1 Start" },
    { CONTROLLER_JUSTIFIER, 0, "X", "Justifier1 AimOffscreen" },
    { CONTROLLER_JUSTIFIER, 0, "Start", "Justifier1 AimOffscreen" },

    { CONTROLLER_DUAL_JUSTIFIERS, 0, "A", "Justifier1 Trigger" },
    { CONTROLLER_DUAL_JUSTIFIERS, 0, "L", "Justifier1 Trigger" },
    { CONTROLLER_DUAL_JUSTIFIERS, 0, "B", "Justifier1 Start" },
    { CONTROLLER_DUAL_JUSTIFIERS, 0, "R", "Justifier1 Start" },
    { CONTROLLER_DUAL_JUSTIFIERS, 0, "X", "Justifier1 AimOffscreen" },
    { CONTROLLER_DUAL_JUSTIFIERS, 0, "Start", "Justifier1 AimOffscreen" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "A", "Justifier2 Trigger" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "L", "Justifier2 Trigger" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "B", "Justifier2 Start" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "R", "Justifier2 Start" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "X", "Justifier2 AimOffscreen" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "Start", "Justifier2 AimOffscreen" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "Up", "ButtonToPointer 1u Med" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "Down", "ButtonToPointer 1d Med" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "Left", "ButtonToPointer 1l Med" },
    { CONTROLLER_DUAL_JUSTIFIERS, 1, "Right", "ButtonToPointer 1r Med" },

    { CONTROLLER_MACSRIFLE, 1, "A", "MacsRifle Trigger" },
    { CONTROLLER_MACSRIFLE, 1, "L", "MacsRifle Trigger" },
};

void S9xJoypadDeviceCommands(int option, int player, const char *button, std::vector<std::string> &commands)
{
    for (auto &d : device_buttons)
        if (d.option == option && d.player == player && !strcmp(d.button, button))
            commands.push_back(d.command);
}

bool S9xGrabJoysticks()
{
    if (joystick_lock)
        return false;

    joystick_lock++;

    return true;
}

void S9xReleaseJoysticks()
{
    joystick_lock--;
}

static void swap_controllers_1_2()
{
    JoypadBinding interrim = gui_config->pad[0];
    gui_config->pad[0] = gui_config->pad[1];
    gui_config->pad[1] = interrim;

    gui_config->rebind_keys();
}

/* Flat state index of the currently selected bank/slot pair. */
int S9xCurrentSaveSlot()
{
    return gui_config->current_save_bank * SAVE_SLOTS_PER_BANK +
           gui_config->current_save_slot;
}

static void show_slot_info()
{
    if (!gui_config->rom_loaded)
        return;

    char extension_string[5];
    snprintf(extension_string, 5, ".%03d", S9xCurrentSaveSlot());
    auto filename = S9xGetFilename(extension_string, SNAPSHOT_DIR);
    struct stat info{};
    std::string exists = "empty";
    if (stat(filename.c_str(), &info) == 0)
        exists = "used";

    auto info_string = "State Slot: " + std::to_string(gui_config->current_save_slot) +
                       ", Bank: " + std::to_string(gui_config->current_save_bank) +
                       " [" + exists + "]";
    S9xSetInfoString(info_string.c_str());
    GFX.InfoStringTimeout = 60;
}

/* Slots wrap inside the current bank; the bank is only changed explicitly. */
static void change_slot(int difference)
{
    gui_config->current_save_slot += difference;
    gui_config->current_save_slot %= SAVE_SLOTS_PER_BANK;
    if (gui_config->current_save_slot < 0)
        gui_config->current_save_slot += SAVE_SLOTS_PER_BANK;

    show_slot_info();
}

static void change_bank(int difference)
{
    gui_config->current_save_bank += difference;
    gui_config->current_save_bank %= NUM_SAVE_BANKS;
    if (gui_config->current_save_bank < 0)
        gui_config->current_save_bank += NUM_SAVE_BANKS;

    top_level->update_accelerators();

    show_slot_info();
}

void S9xHandlePortCommand(s9xcommand_t cmd, int16 data1, int16 data2)
{
    static bool quit_binding_down = false;

    if (data1 == true)
    {
        if (cmd.port[0] == PORT_QUIT)
            quit_binding_down = true;
        else if (cmd.port[0] == PORT_REWIND)
        {
#ifdef RETROACHIEVEMENTS_SUPPORT
            if (RA_IsHardcoreModeActive())
                S9xSetInfoString(_("Rewind is not allowed in Hardcore mode"));
            else
#endif
            Settings.Rewinding = true;
        }
    }

    if (data1 == false) /* Release */
    {
        if (cmd.port[0] != PORT_QUIT)
        {
            quit_binding_down = false;
        }
        if (cmd.port[0] == PORT_COMMAND_FULLSCREEN)
        {
            top_level->toggle_fullscreen_mode();
        }
        else if (cmd.port[0] == PORT_COMMAND_SAVE_SPC)
        {
            top_level->save_spc_dialog();
        }
        else if (cmd.port[0] == PORT_OPEN_ROM)
        {
            top_level->open_rom_dialog();
        }
        else if (cmd.port[0] == PORT_PAUSE)
        {
            if (!(top_level->user_pause))
                top_level->pause_from_user();
            else
                top_level->unpause_from_user();
        }
        else if (cmd.port[0] == PORT_REWIND)
        {
            Settings.Rewinding = false;
        }
        else if (cmd.port[0] == PORT_SEEK_TO_FRAME)
        {
            top_level->movie_seek_dialog();
        }
        else if (cmd.port[0] == PORT_RECORD_MOVIE)
        {
            top_level->record_movie_dialog();
        }
        else if (cmd.port[0] == PORT_PLAY_MOVIE)
        {
            top_level->play_movie_dialog();
        }
        else if (cmd.port[0] == PORT_SWAP_CONTROLLERS)
        {
            swap_controllers_1_2();
        }
        else if (cmd.port[0] == PORT_QUIT)
        {
            if (quit_binding_down)
                S9xExit();
        }
        else if (cmd.port[0] >= PORT_QUICKLOAD0 && cmd.port[0] <= PORT_QUICKLOAD9)
        {
            /* The numbered save/load hotkeys address slots inside the
             * currently selected bank, as on win32. */
            S9xQuickLoadSlot(gui_config->current_save_bank * SAVE_SLOTS_PER_BANK +
                             cmd.port[0] - PORT_QUICKLOAD0);
        }
        else if (cmd.port[0] >= PORT_QUICKSAVE0 && cmd.port[0] <= PORT_QUICKSAVE9)
        {
            S9xQuickSaveSlot(gui_config->current_save_bank * SAVE_SLOTS_PER_BANK +
                             cmd.port[0] - PORT_QUICKSAVE0);
        }
        else if (cmd.port[0] == PORT_SAVESLOT)
        {
            S9xQuickSaveSlot(S9xCurrentSaveSlot());
        }
        else if (cmd.port[0] == PORT_LOADSLOT)
        {
            S9xQuickLoadSlot(S9xCurrentSaveSlot());
        }
        else if (cmd.port[0] == PORT_INCREMENTSAVESLOT)
        {
            change_slot(1);
            S9xQuickSaveSlot(S9xCurrentSaveSlot());
        }
        else if (cmd.port[0] == PORT_DECREMENTLOADSLOT)
        {
            change_slot(-1);
            S9xQuickLoadSlot(S9xCurrentSaveSlot());
        }
        else if (cmd.port[0] == PORT_INCREMENTSLOT)
        {
            change_slot(1);
        }
        else if (cmd.port[0] == PORT_DECREMENTSLOT)
        {
            change_slot(-1);
        }
        else if (cmd.port[0] == PORT_INCREMENTBANK)
        {
            change_bank(1);
        }
        else if (cmd.port[0] == PORT_DECREMENTBANK)
        {
            change_bank(-1);
        }
        else if (cmd.port[0] >= PORT_SELECTSLOT0 && cmd.port[0] <= PORT_SELECTSLOT9)
        {
            /* Select a slot in the current bank without saving or loading. */
            gui_config->current_save_slot = cmd.port[0] - PORT_SELECTSLOT0;
            show_slot_info();
        }
        else if (cmd.port[0] == PORT_DIALOGSAVE)
        {
            top_level->state_preview_dialog(true);
        }
        else if (cmd.port[0] == PORT_DIALOGLOAD)
        {
            top_level->state_preview_dialog(false);
        }
        else if (cmd.port[0] == PORT_FILESAVE)
        {
            top_level->save_state_dialog();
        }
        else if (cmd.port[0] == PORT_FILELOAD)
        {
            top_level->load_state_dialog();
        }
        else if (cmd.port[0] == PORT_GRABMOUSE)
        {
            top_level->toggle_grab_mouse();
        }
        else if (cmd.port[0] >= PORT_GBMODEL0 &&
                 cmd.port[0] < PORT_GBMODEL0 + S9xGBModelHotkeyCount)
        {
            top_level->set_gb_boot_policy(
                S9xGBModelHotkeys[cmd.port[0] - PORT_GBMODEL0].policy);
        }
        else if (cmd.port[0] == PORT_BIOS_MANAGER)
        {
            top_level->open_bios_manager();
        }
    }
}

Binding S9xGetBindingByName(const char *name)
{
    for (int i = 0; i < NUM_EMU_LINKS; i++)
    {
        if (!strcasecmp(b_links[i + NUM_JOYPAD_LINKS].snes9x_name, name))
        {
            return gui_config->shortcut[i];
        }
    }

    return {};
}

s9xcommand_t S9xGetPortCommandT(const char *name)
{
    s9xcommand_t cmd;

    cmd.type = S9xButtonPort;
    cmd.multi_press = 0;
    cmd.button_norpt = 0;
    cmd.port[0] = 0;
    cmd.port[1] = 0;
    cmd.port[2] = 0;
    cmd.port[3] = 0;

    if (!strcasecmp(name, "GTK_fullscreen"))
    {
        cmd.port[0] = PORT_COMMAND_FULLSCREEN;
    }
    else if (!strcasecmp(name, "GTK_save_spc"))
    {
        cmd.port[0] = PORT_COMMAND_SAVE_SPC;
    }
    else if (!strcasecmp(name, "GTK_open_rom"))
    {
        cmd.port[0] = PORT_OPEN_ROM;
    }
    else if (!strcasecmp(name, "GTK_pause"))
    {
        cmd.port[0] = PORT_PAUSE;
    }
    else if (!strcasecmp(name, "GTK_seek_to_frame"))
    {
        cmd.port[0] = PORT_SEEK_TO_FRAME;
    }
    // The core's own versions of these are stubs; the port shows the File
    // menu's dialogs instead. The config keys stay the same.
    else if (!strcasecmp(name, "BeginRecordingMovie"))
    {
        cmd.port[0] = PORT_RECORD_MOVIE;
    }
    else if (!strcasecmp(name, "LoadMovie"))
    {
        cmd.port[0] = PORT_PLAY_MOVIE;
    }
    else if (!strcasecmp(name, "GTK_quit"))
    {
        cmd.port[0] = PORT_QUIT;
    }
    else if (!strcasecmp(name, "GTK_swap_controllers"))
    {
        cmd.port[0] = PORT_SWAP_CONTROLLERS;
    }
    else if (!strncasecmp(name, "GTK_gb_model_", 13))
    {
        cmd.port[0] = PORT_GBMODEL0 + (name[13] - '0');
    }
    else if (!strcasecmp(name, "GTK_bios_manager"))
    {
        cmd.port[0] = PORT_BIOS_MANAGER;
    }
    else if (!strcasecmp(name, "GTK_rewind"))
    {
        cmd.port[0] = PORT_REWIND;
    }
    else if (strstr(name, "QuickLoad000"))
    {
        cmd.port[0] = PORT_QUICKLOAD0;
    }
    else if (strstr(name, "QuickLoad001"))
    {
        cmd.port[0] = PORT_QUICKLOAD1;
    }
    else if (strstr(name, "QuickLoad002"))
    {
        cmd.port[0] = PORT_QUICKLOAD2;
    }
    else if (strstr(name, "QuickLoad003"))
    {
        cmd.port[0] = PORT_QUICKLOAD3;
    }
    else if (strstr(name, "QuickLoad004"))
    {
        cmd.port[0] = PORT_QUICKLOAD4;
    }
    else if (strstr(name, "QuickLoad005"))
    {
        cmd.port[0] = PORT_QUICKLOAD5;
    }
    else if (strstr(name, "QuickLoad006"))
    {
        cmd.port[0] = PORT_QUICKLOAD6;
    }
    else if (strstr(name, "QuickLoad007"))
    {
        cmd.port[0] = PORT_QUICKLOAD7;
    }
    else if (strstr(name, "QuickLoad008"))
    {
        cmd.port[0] = PORT_QUICKLOAD8;
    }
    else if (strstr(name, "QuickLoad009"))
    {
        cmd.port[0] = PORT_QUICKLOAD9;
    }
    else if (strstr(name, "QuickSave000"))
    {
        cmd.port[0] = PORT_QUICKSAVE0;
    }
    else if (strstr(name, "QuickSave001"))
    {
        cmd.port[0] = PORT_QUICKSAVE1;
    }
    else if (strstr(name, "QuickSave002"))
    {
        cmd.port[0] = PORT_QUICKSAVE2;
    }
    else if (strstr(name, "QuickSave003"))
    {
        cmd.port[0] = PORT_QUICKSAVE3;
    }
    else if (strstr(name, "QuickSave004"))
    {
        cmd.port[0] = PORT_QUICKSAVE4;
    }
    else if (strstr(name, "QuickSave005"))
    {
        cmd.port[0] = PORT_QUICKSAVE5;
    }
    else if (strstr(name, "QuickSave006"))
    {
        cmd.port[0] = PORT_QUICKSAVE6;
    }
    else if (strstr(name, "QuickSave007"))
    {
        cmd.port[0] = PORT_QUICKSAVE7;
    }
    else if (strstr(name, "QuickSave008"))
    {
        cmd.port[0] = PORT_QUICKSAVE8;
    }
    else if (strstr(name, "QuickSave009"))
    {
        cmd.port[0] = PORT_QUICKSAVE9;
    }
    else if (!strncmp(name, "GTK_state_select_", 17) &&
             name[17] >= '0' && name[17] <= '9')
    {
        cmd.port[0] = PORT_SELECTSLOT0 + (name[17] - '0');
    }
    else if (strstr(name, "GTK_state_dialog_save"))
    {
        cmd.port[0] = PORT_DIALOGSAVE;
    }
    else if (strstr(name, "GTK_state_dialog_load"))
    {
        cmd.port[0] = PORT_DIALOGLOAD;
    }
    else if (strstr(name, "GTK_state_file_save"))
    {
        cmd.port[0] = PORT_FILESAVE;
    }
    else if (strstr(name, "GTK_state_file_load"))
    {
        cmd.port[0] = PORT_FILELOAD;
    }
    else if (strstr(name, "GTK_state_bank_increment"))
    {
        cmd.port[0] = PORT_INCREMENTBANK;
    }
    else if (strstr(name, "GTK_state_bank_decrement"))
    {
        cmd.port[0] = PORT_DECREMENTBANK;
    }
    else if (strstr(name, "GTK_state_save_current"))
    {
        cmd.port[0] = PORT_SAVESLOT;
    }
    else if (strstr(name, "GTK_state_load_current"))
    {
        cmd.port[0] = PORT_LOADSLOT;
    }
    else if (strstr(name, "GTK_state_increment_save"))
    {
        cmd.port[0] = PORT_INCREMENTSAVESLOT;
    }
    else if (strstr(name, "GTK_state_decrement_load"))
    {
        cmd.port[0] = PORT_DECREMENTLOADSLOT;
    }
    else if (strstr(name, "GTK_state_increment"))
    {
        cmd.port[0] = PORT_INCREMENTSLOT;
    }
    else if (strstr(name, "GTK_state_decrement"))
    {
        cmd.port[0] = PORT_DECREMENTSLOT;
    }
    else if (strstr(name, "GTK_grab_mouse"))
    {
        cmd.port[0] = PORT_GRABMOUSE;
    }
    else
    {
        cmd = S9xGetCommandT(name);
    }

    return cmd;
}


void S9xProcessEvents(bool8 block)
{
    if (S9xGrabJoysticks())
    {
        gui_config->joysticks.poll_events();
        for (auto &j : gui_config->joysticks)
        {
            JoyEvent event;
            while (j.second->get_event(&event))
            {
                Binding binding(j.second->joynum, event.parameter, 0);
                S9xReportButton(binding.hex(), event.state == JOY_PRESSED);
                gui_config->screensaver_needs_reset = true;
            }
        }

        S9xReleaseJoysticks();
    }
}

// LRG rumble dongle -> the SDL device holding SNES Port 1's bindings, as on
// win32. Called once per emulated frame: the SDL effect's 120ms duration
// outlives one refresh interval, so the motors keep running while the game
// drives them and auto-stop if we go quiet (pause, ROM close). A single
// zero-send stops them promptly when the magnitudes drop to zero.
void S9xUpdateRumble()
{
#if SDL_VERSION_ATLEAST(2, 0, 9)
    static uint16 last_low = 0, last_high = 0;

    uint8 l = 0, r = 0;
    if (gui_config->enable_rumble && !Settings.Paused)
        S9xGetRumble(l, r);
    const uint16 low = l * 0x1111, high = r * 0x1111;

    if (low || high || last_low || last_high)
    {
        // Joypad 1's bindings store the device as joynum + 1.
        Binding *pad = (Binding *)&gui_config->pad[0];
        unsigned int devnum = 0;
        for (int i = 0; i < NUM_JOYPAD_LINKS && !devnum; i++)
            if (pad[i].is_joy())
                devnum = pad[i].get_device();

        if (devnum)
        {
            for (auto &j : gui_config->joysticks)
                if (j.second->joynum == (int) (devnum - 1) && j.second->filedes)
                    SDL_JoystickRumble(j.second->filedes, low, high, 120);
        }
    }

    last_low = low;
    last_high = high;
#endif
}


void S9xInitInputDevices()
{
    SDL_Init(SDL_INIT_JOYSTICK);
    size_t num_joysticks = SDL_NumJoysticks();

    for (size_t i = 0; i < num_joysticks; i++)
    {
        gui_config->joysticks.add(i);
    }

    //First plug in both, they'll change later as needed
    S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
    S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
}

void S9xDeinitInputDevices()
{
    gui_config->joysticks.clear();
    SDL_Quit();
}

JoyDevice::JoyDevice()
{
    enabled = false;
    filedes = nullptr;
    mode = JOY_MODE_INDIVIDUAL;
}

JoyDevice::~JoyDevice()
{
    if (filedes)
    {
        SDL_JoystickClose(filedes);
    }
}

bool JoyDevice::set_sdl_joystick(unsigned int sdl_device_index, int new_joynum)
{
    if ((int)sdl_device_index >= SDL_NumJoysticks())
    {
        enabled = false;
        return false;
    }

    filedes = SDL_JoystickOpen(sdl_device_index);

    if (!filedes)
        return false;

    enabled = true;
    instance_id = SDL_JoystickInstanceID(filedes);
    joynum = new_joynum;

    int num_axes = SDL_JoystickNumAxes(filedes);
    int num_hats = SDL_JoystickNumHats(filedes);
    axis.resize(num_axes);
    hat.resize(num_hats);
    calibration.resize(num_axes);

    for (int i = 0; i < num_axes; i++)
    {
        calibration[i].min = -32767;
        calibration[i].max = 32767;
        calibration[i].center = 0;
    }

    description = SDL_JoystickName(filedes);
    description += ": ";
    description += std::to_string(SDL_JoystickNumButtons(filedes));
    description += " buttons, ";
    description += std::to_string(num_axes);
    description += " axes, ";
    description += std::to_string(num_hats);
    description += " hats\n";

    for (auto &i : axis)
        i = 0;

    return true;
}

void JoyDevice::add_event(unsigned int parameter, unsigned int state)
{
    JoyEvent event = { parameter, state };

    queue.push(event);
}

void JoyDevice::register_centers()
{
    for (size_t i = 0; i < axis.size(); i++)
    {
        calibration[i].center = SDL_JoystickGetAxis(filedes, i);

        /* Snap centers to specific target points */
        if (calibration[i].center < -24576)
            calibration[i].center = -32768;
        else if (calibration[i].center < -8192)
            calibration[i].center = -16384;
        else if (calibration[i].center < 8192)
            calibration[i].center = 0;
        else if (calibration[i].center < 24576)
            calibration[i].center = 16383;
        else
            calibration[i].center = 32767;
    }
}

void JoyDevice::handle_event(SDL_Event *event)
{
    if (event->type == SDL_JOYAXISMOTION)
    {
        int cal_min = calibration[event->jaxis.axis].min;
        int cal_max = calibration[event->jaxis.axis].max;
        int cal_cen = calibration[event->jaxis.axis].center;
        int t = gui_config->joystick_threshold;
        int ax_min = (cal_min - cal_cen) * t / 100 + cal_cen;
        int ax_max = (cal_max - cal_cen) * t / 100 + cal_cen;

        if (mode == JOY_MODE_INDIVIDUAL)
        {
            for (int i = 0; i < NUM_JOYPADS; i++)
            {
                Binding *pad = (Binding *)&(gui_config->pad[i]);

                for (int j = 0; j < NUM_JOYPAD_LINKS; j++)
                {
                    if (pad[j].get_axis() == event->jaxis.axis &&
                        pad[j].get_device() == (unsigned int)(joynum + 1))
                    {
                        t = pad[j].get_threshold();

                        if (pad[j].is_positive())
                        {
                            ax_max = (cal_max - cal_cen) * t / 100 + cal_cen;
                        }
                        else if (pad[j].is_negative())
                        {
                            ax_min = (cal_min - cal_cen) * t / 100 + cal_cen;
                        }
                    }
                }
            }

            for (int i = 0; i < NUM_EMU_LINKS; i++)
            {
                if (gui_config->shortcut[i].get_axis() == event->jaxis.axis &&
                    gui_config->shortcut[i].get_device() ==
                        (unsigned int)(joynum + 1))
                {
                    t = gui_config->shortcut[i].get_threshold();
                    if (gui_config->shortcut[i].is_positive())
                    {
                        ax_max = (cal_max - cal_cen) * t / 100 + cal_cen;
                    }
                    else if (gui_config->shortcut[i].is_negative())
                    {
                        ax_min = (cal_min - cal_cen) * t / 100 + cal_cen;
                    }
                }
            }
        }
        else if (mode == JOY_MODE_CALIBRATE)
        {
            if (event->jaxis.value < calibration[event->jaxis.axis].min)
                calibration[event->jaxis.axis].min = event->jaxis.value;
            if (event->jaxis.value > calibration[event->jaxis.axis].max)
                calibration[event->jaxis.axis].min = event->jaxis.value;
        }

        /* Sanity Check */
        if (ax_min >= cal_cen)
            ax_min = cal_cen - 1;
        if (ax_max <= cal_cen)
            ax_max = cal_cen + 1;

        if (event->jaxis.value <= ax_min &&
            axis[event->jaxis.axis] > ax_min)
        {
            add_event(JOY_AXIS(event->jaxis.axis, AXIS_NEG), 1);
        }

        if (event->jaxis.value > ax_min &&
            axis[event->jaxis.axis] <= ax_min)
        {
            add_event(JOY_AXIS(event->jaxis.axis, AXIS_NEG), 0);
        }

        if (event->jaxis.value >= ax_max &&
            axis[event->jaxis.axis] < ax_max)
        {
            add_event(JOY_AXIS(event->jaxis.axis, AXIS_POS), 1);
        }

        if (event->jaxis.value < ax_max &&
            axis[event->jaxis.axis] >= ax_max)
        {
            add_event(JOY_AXIS(event->jaxis.axis, AXIS_POS), 0);
        }

        axis[event->jaxis.axis] = event->jaxis.value;
    }

    else if (event->type == SDL_JOYBUTTONUP ||
             event->type == SDL_JOYBUTTONDOWN)
    {
        add_event(event->jbutton.button,
                  event->jbutton.state == SDL_PRESSED ? 1 : 0);
    }

    else if (event->type == SDL_JOYHATMOTION)
    {
        if ((event->jhat.value & SDL_HAT_UP) &&
            !(hat[event->jhat.hat] & SDL_HAT_UP))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2), AXIS_POS), 1);
        }

        if (!(event->jhat.value & SDL_HAT_UP) &&
            (hat[event->jhat.hat] & SDL_HAT_UP))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2), AXIS_POS), 0);
        }

        if ((event->jhat.value & SDL_HAT_DOWN) &&
            !(hat[event->jhat.hat] & SDL_HAT_DOWN))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2), AXIS_NEG), 1);
        }

        if (!(event->jhat.value & SDL_HAT_DOWN) &&
            (hat[event->jhat.hat] & SDL_HAT_DOWN))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2), AXIS_NEG), 0);
        }

        if ((event->jhat.value & SDL_HAT_LEFT) &&
            !(hat[event->jhat.hat] & SDL_HAT_LEFT))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2) + 1, AXIS_NEG), 1);
        }

        if (!(event->jhat.value & SDL_HAT_LEFT) &&
            (hat[event->jhat.hat] & SDL_HAT_LEFT))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2) + 1, AXIS_NEG), 0);
        }

        if ((event->jhat.value & SDL_HAT_RIGHT) &&
            !(hat[event->jhat.hat] & SDL_HAT_RIGHT))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2) + 1, AXIS_POS), 1);
        }

        if (!(event->jhat.value & SDL_HAT_RIGHT) &&
            (hat[event->jhat.hat] & SDL_HAT_RIGHT))
        {
            add_event(JOY_AXIS(axis.size() + (event->jhat.hat * 2) + 1, AXIS_POS), 0);
        }

        hat[event->jhat.hat] = event->jhat.value;
    }
}

int JoyDevice::get_event(JoyEvent *event)
{
    if (queue.empty())
        return 0;

    event->parameter = queue.front().parameter;
    event->state = queue.front().state;

    queue.pop();

    return 1;
}

void JoyDevice::flush()
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
    }

    while (!queue.empty())
        queue.pop();
}

void JoyDevices::clear()
{
    joysticks.clear();
}

bool JoyDevices::add(int sdl_device_index)
{
    std::array<bool, NUM_JOYPADS> joynums{};
    joynums.fill(false);
    for (auto &j : joysticks)
    {
        joynums[j.second->joynum] = true;
    }

    // New joystick always gets the lowest available joynum
    int joynum = 0;
    for (; joynum < NUM_JOYPADS && joynums[joynum]; ++joynum) {};

    if (joynum == NUM_JOYPADS)
    {
        printf("Joystick slots are full, cannot add joystick (device index %d)\n", sdl_device_index);
        return false;
    }

    auto ujd = std::make_unique<JoyDevice>();
    ujd->set_sdl_joystick(sdl_device_index, joynum);
    printf("Joystick %d, %s", ujd->joynum+1, ujd->description.c_str());
    joysticks[ujd->instance_id] = std::move(ujd);
    return true;
}

bool JoyDevices::remove(SDL_JoystickID instance_id)
{
    if (!joysticks.contains(instance_id))
    {
        printf("joystick_remove: invalid instance id %d", instance_id);
        return false;
    }
    printf("Removed joystick %d, %s", joysticks[instance_id]->joynum+1, joysticks[instance_id]->description.c_str());
    joysticks.erase(instance_id);
    return true;
}

JoyDevice *JoyDevices::get_joystick(SDL_JoystickID instance_id)
{
    if (joysticks.contains(instance_id)){
        return joysticks[instance_id].get();
    }
    printf("BUG: Event for unknown joystick instance id: %d", instance_id);
    return nullptr;
}

void JoyDevices::register_centers()
{
    for (auto &j : joysticks)
        j.second->register_centers();
}

void JoyDevices::flush_events()
{
    for (auto &j : joysticks)
        j.second->flush();
}

void JoyDevices::set_mode(int mode)
{
    for (auto &j : joysticks)
        j.second->mode = mode;
}

void JoyDevices::poll_events()
{
    SDL_Event event;
    JoyDevice *jd{};

    while (SDL_PollEvent(&event))
    {
        switch(event.type) {
            case SDL_JOYAXISMOTION:
                jd = get_joystick(event.jaxis.which);
                break;
            case SDL_JOYHATMOTION:
                jd = get_joystick(event.jhat.which);
                break;
            case SDL_JOYBUTTONUP:
            case SDL_JOYBUTTONDOWN:
                jd = get_joystick(event.jbutton.which);
                break;
            case SDL_JOYDEVICEADDED:
                add(event.jdevice.which);
                continue;
            case SDL_JOYDEVICEREMOVED:
                remove(event.jdevice.which);
                continue;
        }
        
        if (jd)
        {
            jd->handle_event(&event);
        }
    }
}

