#ifndef __EMU_MAIN_WINDOW_HPP
#define __EMU_MAIN_WINDOW_HPP

#include <QAction>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include "EmuCanvas.hpp"
#include "snes9x.h"   // memmap.h relies on its types/macros being in scope
#include "memmap.h"   // S9X_NUM_GBBOOT_POLICIES sizes the BIOS menu array

class EmuApplication;
class CheatsDialog;
class AudioWaveformWindow;
class TileViewerWindow;
class TilemapViewerWindow;
class SpriteViewerWindow;
class GBTileViewerWindow;
class GBTilemapViewerWindow;
class GBSpriteViewerWindow;

class EmuMainWindow : public QMainWindow
{
  Q_OBJECT

  public Q_SLOTS:
    void output(uint8_t *buffer, int width, int height, QImage::Format format, int bytes_per_line, double frame_rate);

  public:
    EmuMainWindow(EmuApplication &app);
    ~EmuMainWindow();

    void toggleFullscreen();
    bool createCanvas();
    void destroyCanvas();
    void recreateCanvas();
    void setBypassCompositor(bool);
    void setCoreActionsEnabled(bool);
    bool event(QEvent *event) override;
    bool eventFilter(QObject *, QEvent *event) override;
    void resizeToMultiple(int multiple);
    // View->Always on Top / Lock Screen Resize, as on win32's Video menu.
    void applyAlwaysOnTop();
    void applyResizeLock();
    void populateRecentlyUsed();
    void chooseState(bool save);
    void statePreviewDialog(bool save);
    void pauseContinue();
    bool isActivelyDrawing();
    void openFile();
    // Emulation -> Game Boy Model, shared by the menu entries and their
    // hotkeys so both apply the same gates and reload the same way.
    void setGBBootPolicy(int policy);
    void openBiosManager();
    // A coin-op front-panel hotkey; false when the name is not one.
    bool arcadeShortcut(const std::string &name);
    // Emulation -> Super Disc; the shortcuts call these too.
    void superDiscInsert();
    void superDiscEject();
    void powerCycle();
    bool openFile(const std::string &filename);
    void playMovieDialog();
    void recordMovieDialog();
    void toggleAVIRecording();
    void recreateUIAssets();
    void shaderChanged();
    void updateShaderSettingsItem();
    void gameChanging();
    void toggleAudioWaveform();
    /* Emulation > S-PPU: one window each, raised again if already open. */
    void showTileViewer();
    void showTilemapViewer();
    void showSpriteViewer();
    void showGBTileViewer();
    void showGBTilemapViewer();
    void showGBSpriteViewer();
    /* "Pause emulation when unfocused". The audio waveform viewer counts as
     * part of this window: moving between the two doesn't pause, leaving
     * from either does, so the viewer reports its focus changes here too. */
    void handleFocusChange(bool active);
    void toggleMouseGrab();
    void updatePortConfigurationMenu();
    void applyWindowIcon();
    void chooseWindowIcon(int index);
    void setWriteIconToLauncher(bool enabled);
    void syncLauncherIcon();
    static QIcon logoIcon(int index);
    std::vector<std::string> getDisplayDeviceList();
    EmuApplication &app;
    EmuCanvas *canvas = nullptr;

  private:
    void idle();
    void createWidgets();
    std::string promptRenameMSU1Pack(const std::string &filename);
    // The window size View->Set Size gives a multiple, and which multiple the
    // window currently has (0 for none of them).
    QSize sizeForMultiple(int multiple);
    int currentSizeMultiple();
    // Resize past a Lock Screen Resize, which only takes away the drag border.
    void resizeLocked(const QSize &size);

    static const size_t recent_menu_size = 10;

    std::unique_ptr<CheatsDialog> cheats_dialog;
    // Sound->Show Audio Waveform; deletes itself on close, so this goes null.
    QPointer<AudioWaveformWindow> audio_waveform_window;
    // Emulation->S-PPU viewers; these delete themselves on close too.
    QPointer<TileViewerWindow> tile_viewer_window;
    QPointer<TilemapViewerWindow> tilemap_viewer_window;
    QPointer<SpriteViewerWindow> sprite_viewer_window;
    QPointer<GBTileViewerWindow> gb_tile_viewer_window;
    QPointer<GBTilemapViewerWindow> gb_tilemap_viewer_window;
    QPointer<GBSpriteViewerWindow> gb_sprite_viewer_window;

    bool manual_pause = false;
    bool focus_pause = false;
    bool minimized_pause = false;
    bool mouse_grabbed = false;
    QMenu *load_state_menu;
    QMenu *save_state_menu;
    QMenu *recent_menu;
    QMenu *bios_menu = nullptr;
    QAction *bios_menu_action = nullptr;
    // One per S9xGBBootPolicy, in enum order.
    QAction *bios_policy_actions[S9X_NUM_GBBOOT_POLICIES] = {};
    void refreshBiosMenu();

    QMenu *voicekun_menu = nullptr;
    QAction *voicekun_menu_action = nullptr;
    QAction *voicekun_attach_action = nullptr;
    QAction *voicekun_detach_action = nullptr;
    void refreshVoicekunMenu();
    void voicekunAttach();
    void voicekunDetach();

    // Emulation -> Super Famicom Box / Nintendo Super System front panels.
    QAction *sfcbox_menu_action = nullptr;
    QAction *sfcbox_keyswitch_actions[5] = {};
    QAction *sfcbox_backdrop_action = nullptr;
    QAction *sfcbox_english_action = nullptr;
    QAction *nss_menu_action = nullptr;
    QAction *nss_game_actions[3] = {};
    QAction *nss_eject_actions[3] = {};
    QAction *nss_game_only_actions[3] = {};
    QAction *nss_dips_action = nullptr;
    QAction *nss_dip_actions[8] = {};
    // Emulation -> PowerFest '94 / Campus Challenge '92 session timer.
    QAction *event_menu_action = nullptr;
    QAction *event_minutes_actions[16] = {};
    QAction *event_display_actions[3] = {};
    QTimer event_title_timer;
    QString event_title_suffix;
    void setEventTimer(int minutes, int display);
    void updateEventTitle();
    void createArcadeMenus(QMenu *emulation_menu);
    void refreshArcadeMenus();
    void sfcboxSetKeyswitch(int panel_pos);
    void insertCoin(int slot);
    void nssPulse(uint16_t buttons, bool game_only);
    void nssGame(int slot);
    void nssEject(int slot);
    void nssToggleDip(int sw);

    QMenu *superdisc_menu = nullptr;
    QAction *superdisc_menu_action = nullptr;
    QAction *superdisc_insert_action = nullptr;
    QAction *superdisc_eject_action = nullptr;
    void refreshSuperDiscMenu();
    void updateWindowTitle();

    QTimer mouse_timer;
    bool cursor_visible = true;
    // Light guns aim at the spot under the (ungrabbed) host pointer.
    bool gunAimsAtPointer();
    void reportGunAim(const QPoint &global_pos);
    // Input menu device list, one per EmuConfig::PortConfiguration.
    std::vector<QAction *> port_configuration_actions;
    QAction *superscope_crosshair_action = nullptr;
    QAction *shader_settings_item;
    QAction *movie_stop_action = nullptr;
    QAction *avi_recording_action = nullptr;
    std::vector<QAction *> core_actions;
    std::vector<QAction *> recent_menu_items;

  public:
#ifdef RETROACHIEVEMENTS_SUPPORT
    QAction *ra_enabled_action = nullptr;
    QAction *ra_login_action = nullptr;
    QAction *ra_hardcore_action = nullptr;
    QAction *ra_achievements_action = nullptr;
    QAction *ra_view_profile_action = nullptr;
#endif
#ifdef KAILLERA_SUPPORT
    QAction *kaillera_host_action = nullptr;
    QAction *kaillera_end_action = nullptr;
#endif
};

#endif