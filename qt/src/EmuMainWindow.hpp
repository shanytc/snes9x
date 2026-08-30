#ifndef __EMU_MAIN_WINDOW_HPP
#define __EMU_MAIN_WINDOW_HPP

#include <QAction>
#include <QMainWindow>
#include <QTimer>
#include "EmuCanvas.hpp"
#include "memmap.h"   // S9X_NUM_GBBOOT_POLICIES sizes the BIOS menu array

class EmuApplication;
class CheatsDialog;

class EmuMainWindow : public QMainWindow
{
  Q_OBJECT

  public Q_SLOTS:
    void output(uint8_t *buffer, int width, int height, QImage::Format format, int bytes_per_line, double frame_rate);

  public:
    EmuMainWindow(EmuApplication *app);
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
    void populateRecentlyUsed();
    void chooseState(bool save);
    void statePreviewDialog(bool save);
    void pauseContinue();
    bool isActivelyDrawing();
    void openFile();
    // Emulation -> Game Boy Model, shared by the menu entries and their
    // hotkeys so both apply the same gates and reload the same way.
    void setGBBootPolicy(int policy);
    bool openFile(const std::string &filename);
    void recreateUIAssets();
    void shaderChanged();
    void updateShaderSettingsItem();
    void gameChanging();
    void toggleMouseGrab();
    std::vector<std::string> getDisplayDeviceList();
    EmuApplication *app = nullptr;
    EmuCanvas *canvas = nullptr;

  private:
    void idle();
    void createWidgets();
    std::string promptRenameMSU1Pack(const std::string &filename);

    static const size_t recent_menu_size = 10;

    std::unique_ptr<CheatsDialog> cheats_dialog;

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

    QTimer mouse_timer;
    bool cursor_visible = true;
    QAction *shader_settings_item;
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