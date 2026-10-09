#pragma once
#include "ui_ControllerPanel.h"
#include "BindingPanel.hpp"
#include "PadPictureWidget.hpp"
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

    QMenu edit_menu;
    QMenu auto_assign_menu;
    PadPictureWidget *pad_picture;
    QTimer pad_picture_timer;
    std::map<int, EmuBinding> held_keys;	// by Qt key, while the panel is shown
    std::vector<uint32_t> held_joystick;	// hashes of the joystick inputs held
};
