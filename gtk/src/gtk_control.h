/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once
#include <queue>
#include <vector>
#include <string>
#include <array>

#include "gtk_binding.h"
#include "SDL.h"
// SDL.h may include altivec.h which redefines vector and bool
#undef vector
#undef bool

/* Binding slots: pads 1-5, their "+" alternates, then pads 6-8 and their
 * alternates, appended so older configs keep their slot numbers. Eight pads
 * cover two multitaps, as on win32. joypad_player() maps a slot to its pad. */
const int NUM_JOYPADS = 16;

inline int joypad_player(int slot)
{
    if (slot < 5)
        return slot;
    if (slot < 10)
        return slot - 5;
    if (slot < 13)
        return slot - 10 + 5;
    return slot - 13 + 5;
}

/* What is plugged into the two controller ports: win32's Input menu device
 * list, in its enum order (the NSRT auto-detection table depends on it). */
enum ControllerOption
{
    CONTROLLER_JOYPADS = 0,
    CONTROLLER_MOUSE,
    CONTROLLER_SUPERSCOPE,
    CONTROLLER_MULTITAP5,
    CONTROLLER_JUSTIFIER,
    CONTROLLER_MOUSE_SWAPPED,
    CONTROLLER_MULTITAP8,
    CONTROLLER_DUAL_JUSTIFIERS,
    CONTROLLER_MACSRIFLE,
    NUM_CONTROLLER_OPTIONS
};

/* Save states are organized in banks of slots, as on win32. The state file
 * extension is the flat index, i.e. bank * SAVE_SLOTS_PER_BANK + slot. */
const int SAVE_SLOTS_PER_BANK = 10;
const int NUM_SAVE_BANKS      = 10;
const int NUM_SAVE_SLOTS      = NUM_SAVE_BANKS * SAVE_SLOTS_PER_BANK;

enum {
    JOY_MODE_GLOBAL     = 0,
    JOY_MODE_INDIVIDUAL = 1,
    JOY_MODE_CALIBRATE  = 2
};

enum {
    JOY_RELEASED = 0,
    JOY_PRESSED  = 1
};

enum {
    PORT_COMMAND_FULLSCREEN = 1,
    PORT_COMMAND_SAVE_SPC   = 2,
    PORT_OPEN_ROM           = 3,
    PORT_PAUSE              = 4,
    PORT_SEEK_TO_FRAME      = 5,
    PORT_QUIT               = 6,
    PORT_SWAP_CONTROLLERS   = 7,
    PORT_REWIND             = 8,
    PORT_QUICKLOAD0         = 9,
    PORT_QUICKLOAD1         = 10,
    PORT_QUICKLOAD2         = 11,
    PORT_QUICKLOAD3         = 12,
    PORT_QUICKLOAD4         = 13,
    PORT_QUICKLOAD5         = 14,
    PORT_QUICKLOAD6         = 15,
    PORT_QUICKLOAD7         = 16,
    PORT_QUICKLOAD8         = 17,
    PORT_QUICKLOAD9         = 18,
    PORT_SAVESLOT           = 19,
    PORT_LOADSLOT           = 20,
    PORT_INCREMENTSAVESLOT  = 21,
    PORT_DECREMENTLOADSLOT  = 22,
    PORT_INCREMENTSLOT      = 23,
    PORT_DECREMENTSLOT      = 24,
    PORT_GRABMOUSE          = 25,
    PORT_QUICKSAVE0         = 26,
    PORT_QUICKSAVE1         = 27,
    PORT_QUICKSAVE2         = 28,
    PORT_QUICKSAVE3         = 29,
    PORT_QUICKSAVE4         = 30,
    PORT_QUICKSAVE5         = 31,
    PORT_QUICKSAVE6         = 32,
    PORT_QUICKSAVE7         = 33,
    PORT_QUICKSAVE8         = 34,
    PORT_QUICKSAVE9         = 35,
    PORT_INCREMENTBANK      = 36,
    PORT_DECREMENTBANK      = 37,
    PORT_SELECTSLOT0        = 38,
    PORT_SELECTSLOT1        = 39,
    PORT_SELECTSLOT2        = 40,
    PORT_SELECTSLOT3        = 41,
    PORT_SELECTSLOT4        = 42,
    PORT_SELECTSLOT5        = 43,
    PORT_SELECTSLOT6        = 44,
    PORT_SELECTSLOT7        = 45,
    PORT_SELECTSLOT8        = 46,
    PORT_SELECTSLOT9        = 47,
    PORT_DIALOGSAVE         = 48,
    PORT_DIALOGLOAD         = 49,
    PORT_FILESAVE           = 50,
    PORT_FILELOAD           = 51,
    PORT_RECORD_MOVIE       = 52,
    PORT_PLAY_MOVIE         = 53
};

typedef struct BindingLink
{
    const char *button_name;
    const char *snes9x_name;

} BindingLink;

extern const BindingLink b_links[];
extern const int b_breaks[];
const int NUM_JOYPAD_LINKS = 24;
const int NUM_EMU_LINKS = 78;

typedef struct JoypadBinding
{
    std::array<Binding, NUM_JOYPAD_LINKS> data;
} JoypadBinding;

bool S9xGrabJoysticks();
void S9xReleaseJoysticks();
void S9xUpdateRumble();
int S9xCurrentSaveSlot();

typedef struct JoyEvent
{
    unsigned int parameter;
    unsigned int state;

} JoyEvent;

typedef struct Calibration
{
    int min;
    int max;
    int center;
} Calibration;

class JoyDevice
{
  public:
    JoyDevice();
    ~JoyDevice();
    int get_event(JoyEvent *event);
    void flush();
    void handle_event(SDL_Event *event);
    void register_centers();
    bool set_sdl_joystick(unsigned int device_index, int slot);

    std::string description;
    SDL_Joystick *filedes;
    SDL_JoystickID instance_id;
    std::queue<JoyEvent> queue;
    int mode;
    int joynum;
    std::vector<Calibration> calibration;
    std::vector<int> axis;
    std::vector<int> hat;
    bool enabled;

  private:
    void add_event(unsigned int parameter, unsigned int state);
};

class JoyDevices
{
    public:
        void clear();
        bool add(int sdl_device_index);
        bool remove(SDL_JoystickID instance_id);
        void register_centers();
        void flush_events();
        void set_mode(int mode);

        void poll_events();
        std::map<SDL_JoystickID, std::unique_ptr<JoyDevice>>::const_iterator begin() const { return joysticks.begin(); }
        std::map<SDL_JoystickID, std::unique_ptr<JoyDevice>>::const_iterator end() const { return joysticks.end(); }

    private:
        JoyDevice *get_joystick(SDL_JoystickID instance_id);
        std::map<SDL_JoystickID, std::unique_ptr<JoyDevice>> joysticks;
};

void S9xDeinitInputDevices();
Binding S9xGetBindingByName(const char *name);
bool S9xIsMousePluggedIn();

/* Controller-port devices (see ControllerOption). Apply seats the devices
 * from gui_config, Set is a user pick, AutoDetect applies a freshly loaded
 * ROM's NSRT hints, and the last two add the joypad buttons that also work
 * the device in the neighbouring port to a pad's bindings. */
void S9xApplyControllerOption();
void S9xApplySuperScopeCrosshair();
void S9xSetControllerOption(int option);
void S9xAutoDetectControllerOption();
bool S9xControllerOptionValid(int option);
void S9xJoypadDeviceCommands(int option, int player, const char *button, std::vector<std::string> &commands);
