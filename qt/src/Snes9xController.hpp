#ifndef __SNES9X_CONTROLLER_HPP
#define __SNES9X_CONTROLLER_HPP
#include <functional>
#include <vector>
#include <cstdint>
#include <string>

#include "EmuConfig.hpp"

class Snes9xController
{
  public:
    static Snes9xController *get();

    void init();
    void deinit();
    void mainLoop();
    bool openFile(const std::string &filename);
    bool slotUsed(int slot);
    bool loadState(const std::string &filename);
    bool loadState(int slot);
    void loadUndoState();
    bool saveState(const std::string &filename);
    bool saveState(int slot);
    void saveSPC();
    void takeScreenshot(bool paused);
    bool saveSRAM();
    bool saveMemoryPack();
    bool hasMemoryPack();

    /* File->Movie Play/Record/Stop, as on win32. The open/create results are
     * movie.cpp's SUCCESS/FILE_NOT_FOUND/WRONG_FORMAT/WRONG_VERSION. */
    int openMovie(const std::string &filename, bool read_only);
    int createMovie(const std::string &filename, uint8_t controllers_mask,
                    bool from_reset, bool clear_sram, const std::wstring &metadata);
    void stopMovie();
    bool movieActive();
    /* Writes the battery save out so the Record Movie dialog can tell whether
     * there is one to clear; returns whether it exists and is writable. */
    bool saveAndCheckSRAM();

    /* File->AVI Recording. */
    bool startAVIRecording(const std::string &filename, bool hires, bool include_audio, std::string &error);
    void stopAVIRecording();
    bool aviRecording();
    void updateSettings(EmuConfig *config);
    void updateBindings(const EmuConfig * const config);
    void reportBinding(EmuBinding b, bool active);
    void reportMouseButton(int button, bool pressed);
    void reportPointer(int x, int y);
    void reportPointerAbsolute(int x, int y);
    void setSuperScopeCrosshairVisible(bool visible);
    void updateSoundBufferLevel(int, int);
    bool acceptsCommand(const char *command);
    bool isAbnormalSpeed();
    void mute(bool muted);
    void reset();
    void softReset();
    void setPaused(bool paused);
    void setMessage(const std::string &message);
    void clearSoundBuffer();
    std::vector<std::tuple<bool, std::string, std::string>> getCheatList();
    void disableAllCheats();
    void enableCheat(int index);
    void disableCheat(int index);
    bool addCheat(const std::string &description, const std::string &code);
    void deleteCheat(int index);
    void moveCheat(int from, int to);
    void deleteAllCheats();
    int tryImportCheats(const std::string &filename);
    std::string validateCheat(const std::string &code);
    int modifyCheat(int index, const std::string &name,
                    const std::string &code);
    std::string getContentFolder();

    std::string getStateFolder();
    std::string getStateFilename(int slot);
    std::string config_folder;
    std::string sram_folder;
    std::string state_folder;
    std::string cheat_folder;
    std::string patch_folder;
    std::string export_folder;
    std::string bios_folder;
    int16_t mouse_x, mouse_y;
    int high_resolution_effect;
    bool blend_hires = true;
    int software_filter = 0;
    int software_filter_hires = 0;
    int rewind_buffer_size;
    int rewind_frame_interval;
    bool rewinding = false;

    std::function<void(uint16_t *, int, int, int, double)> screen_output_function = nullptr;
    std::function<void(int16_t *, int)> sound_output_function = nullptr;

    bool active = false;

  protected:
    Snes9xController();
    ~Snes9xController();

  private:
    void SamplesAvailable();
    void mainLoopWithRunAhead();
    // Light guns keep the pointer within the screen; the SNES mouse is free.
    bool clamp_pointer = false;

    // Savestate scratch buffer for run-ahead. Sized per ROM (freeze size
    // depends on which special chips the cart uses), so openFile clears it.
    std::vector<uint8_t> run_ahead_buffer;

};

uint8_t S9xGetSoundChannelMask();
void S9xSetSoundChannelMask(uint8_t mask);

#endif