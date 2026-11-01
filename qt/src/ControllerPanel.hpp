#pragma once
#include "ui_ControllerPanel.h"
#include "BindingPanel.hpp"
#include "PadPictureWidget.hpp"
#include "EmuConfig.hpp"
#include "padpicture.h"
#include <QMenu>
#include <QTimer>
#include <map>
#include <vector>

class EmuApplication;

class ControllerPanel :
    public Ui::ControllerPanel,
    public BindingPanel
{
  public:
    explicit ControllerPanel(EmuApplication *app);
    ~ControllerPanel();
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void clearAllControllers();
    void clearCurrentController();
    void autoPopulateWithKeyboard(int slot);
    void autoPopulateWithJoystick(int joystick_id, int slot);
    void swapControllers(int first, int second);
    void recreateAutoAssignMenu();
    bool boundHere(const EmuBinding &b);
    int heldButtons();
    int markedButtons();
    void updatePadPicture();
    void showController(int index);
    bool pachinkoShown();
    int rowButton(int row);

    // The pad rows' names and icons, put back after the Sunsoft Pachinko's three rows.
    QString pad_row_names[EmuConfig::num_controller_bindings];
    QIcon pad_row_icons[EmuConfig::num_controller_bindings];
    S9xPachinkoPreview pachinko_preview;	// its power gauge, from 0 each time the panel shows

    QMenu edit_menu;
    QMenu auto_assign_menu;
    PadPictureWidget *pad_picture;
    QTimer pad_picture_timer;
    std::map<int, EmuBinding> held_keys;	// by Qt key, while the panel is shown
    std::vector<uint32_t> held_joystick;	// hashes of the joystick inputs held
};
