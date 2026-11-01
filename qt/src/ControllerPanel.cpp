#include "ControllerPanel.hpp"
#include "SDL3/SDL_gamepad.h"
#include "SDLInputManager.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#include "padpicture.h"
#include <QAction>
#include <QtEvents>
#include <QTimer>
#include <algorithm>

// The table's rows as pad picture buttons; the turbo rows light their buttons too.
static const int kRowButtons[EmuConfig::num_controller_bindings] = {
    PADPIC_UP, PADPIC_DOWN, PADPIC_LEFT, PADPIC_RIGHT, PADPIC_A, PADPIC_B, PADPIC_X, PADPIC_Y,
    PADPIC_L, PADPIC_R, PADPIC_START, PADPIC_SELECT,
    PADPIC_A, PADPIC_B, PADPIC_X, PADPIC_Y, PADPIC_L, PADPIC_R
};
static const int kMainRows = 12;

// The Sunsoft Pachinko's rows, after the eight pads in the controller list.
static const int kPachinkoController = EmuConfig::num_controllers;
static const int kPachinkoRowButtons[EmuConfig::num_pachinko_bindings] = { PACHPIC_STRONGER, PACHPIC_WEAKER, PACHPIC_STOP };

ControllerPanel::ControllerPanel(EmuApplication *app_)
    : BindingPanel(app_)
{
    setupUi(this);
    controllerComboBox->addItem(tr("Sunsoft Pachinko"));
    portComboBox->addItem(tr("Sunsoft Pachinko"));
    QObject::connect(controllerComboBox, &QComboBox::currentIndexChanged, [&](int index) {
        showController(index);
    });

    BindingPanel::setTableWidget(tableWidget_controller,
                                 app->config->binding.controller[0].buttons,
                                 EmuConfig::allowed_bindings,
                                 EmuConfig::num_controller_bindings);

    auto action = edit_menu.addAction(QObject::tr("Clear Current Controller"));
    connect(action, &QAction::triggered, [&](bool checked) {
        clearCurrentController();
    });

    action = edit_menu.addAction(QObject::tr("Clear All Controllers"));
    connect(action, &QAction::triggered, [&](bool checked) {
        clearAllControllers();
    });

    auto swap_menu = edit_menu.addMenu(QObject::tr("Swap With"));
    for (auto i = 0; i < EmuConfig::num_controllers; i++)
    {
        action = swap_menu->addAction(QObject::tr("Controller %1").arg(i + 1));
        connect(action, &QAction::triggered, [&, i](bool) {
            auto current_index = controllerComboBox->currentIndex();
            if (current_index == i || pachinkoShown())
                return;
            swapControllers(i, current_index);
            fillTable();
        });
    }

    editToolButton->setMenu(&edit_menu);
    editToolButton->setPopupMode(QToolButton::InstantPopup);

    QString iconset = app->iconPrefix();
    const char *icons[] = {
        "up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select", "a", "b", "x", "y", "l", "r"
    };
    for (int i = 0; i < 18; i++)
    {
        pad_row_icons[i] = QIcon(iconset + icons[i] + ".svg");
        pad_row_names[i] = tableWidget_controller->verticalHeaderItem(i)->text();
        tableWidget_controller->verticalHeaderItem(i)->setIcon(pad_row_icons[i]);
    }

    recreateAutoAssignMenu();
    onJoypadsChanged([&]{ recreateAutoAssignMenu(); });

    connect(automapGamepadsCheckbox, &QCheckBox::toggled, [&](bool checked) {
       this->app->config->automap_gamepads = checked;
        app->updateBindings();
    });

    // Same setting as the Input menu's device list.
    connect(portComboBox, &QComboBox::currentIndexChanged, [&](int index) {
        if (index < 0 || index == this->app->config->port_configuration)
            return;
        app->setPortConfiguration(index);
    });

    // The pad picture under the table: a click on a button edits its binding.
    pad_picture = new PadPictureWidget(this);
    verticalLayout->addSpacing(10);
    verticalLayout->addWidget(pad_picture);
    pad_picture->setPadStyle(app->config->pad_picture_style);
    pad_picture->buttonClicked = [&](int button) {
        const int rows = pachinkoShown() ? EmuConfig::num_pachinko_bindings : kMainRows;
        for (int row = 0; row < rows; row++)
            if (rowButton(row) == button)
            {
                int column = std::max(tableWidget_controller->currentColumn(), 0);
                tableWidget_controller->setFocus();
                tableWidget_controller->setCurrentCell(row, column);
                cellActivated(row, column);
                break;
            }
    };
    // As win32: a click off the buttons ends editing.
    pad_picture->backgroundClicked = [&] {
        cancelBinding();
        tableWidget_controller->clearSelection();
        tableWidget_controller->clearFocus();
        updatePadPicture();
    };
    pad_picture->styleChosen = [&](int style) {
        this->app->config->pad_picture_style = style;
    };
    pad_picture_timer.setInterval(30);
    connect(&pad_picture_timer, &QTimer::timeout, [&] { updatePadPicture(); });
}

ControllerPanel::~ControllerPanel()
{
    app->binding_monitor = nullptr;
}

void ControllerPanel::recreateAutoAssignMenu()
{
    auto_assign_menu.clear();
    auto controller_list = app->input_manager->getXInputControllers();

    for (int i = 0; i < EmuConfig::allowed_bindings; i++)
    {
        auto slot_menu = auto_assign_menu.addMenu(tr("Binding Set #%1").arg(i + 1));
        auto default_keyboard = slot_menu->addAction(tr("Default Keyboard"));
        connect(default_keyboard, &QAction::triggered, [&, slot = i](bool) {
            autoPopulateWithKeyboard(slot);
        });

        for (const auto& c : controller_list)
        {
            auto controller_item = slot_menu->addAction(c.second.c_str());
            connect(controller_item, &QAction::triggered, [&, id = c.first, slot = i](bool) {
                autoPopulateWithJoystick(id, slot);
            });
        }
    }
    autoAssignToolButton->setMenu(&auto_assign_menu);
    autoAssignToolButton->setPopupMode(QToolButton::InstantPopup);
}

void ControllerPanel::autoPopulateWithKeyboard(int slot)
{
    if (pachinkoShown())
    {
        const char *pachinko_list[] = { "e", "q", "w" };
        for (int i = 0; i < EmuConfig::num_pachinko_bindings; i++)
            binding[EmuConfig::allowed_bindings * i + slot] = EmuBinding::keyboard(QKeySequence::fromString(pachinko_list[i])[0].key());
        fillTable();
        app->updateBindings();
        return;
    }

    auto &buttons = app->config->binding.controller[controllerComboBox->currentIndex()].buttons;
    const char *button_list[] = { "Up", "Down", "Left", "Right", "d", "c", "s", "x", "z", "a", "Return", "Space" };

    for (int i = 0; i < std::size(button_list); i++)
        buttons[EmuConfig::allowed_bindings * i + slot] = EmuBinding::keyboard(QKeySequence::fromString(button_list[i])[0].key());

    fillTable();
    app->updateBindings();
}

void ControllerPanel::autoPopulateWithJoystick(int joystick_id, int slot)
{
    auto &device = app->input_manager->devices[joystick_id];
    auto sdl_controller = device.gamepad;
    EmuBinding *buttons = binding;
    // The Pachinko: the shoulders turn the dial, A (where its own button reads) is Stop
    const SDL_GamepadButton pachinko_list[] = { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                                                SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                                SDL_GAMEPAD_BUTTON_EAST };
    const SDL_GamepadButton list[] = { SDL_GAMEPAD_BUTTON_DPAD_UP,
                                       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                       SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                       SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                                       // B, A and X, Y are inverted on XInput vs SNES
                                       SDL_GAMEPAD_BUTTON_EAST,
                                       SDL_GAMEPAD_BUTTON_SOUTH,
                                       SDL_GAMEPAD_BUTTON_NORTH,
                                       SDL_GAMEPAD_BUTTON_WEST,
                                       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                       SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                                       SDL_GAMEPAD_BUTTON_START,
                                       SDL_GAMEPAD_BUTTON_BACK };

    auto bindings = SDLInputManager::getXInputButtonBindings(sdl_controller);
    const SDL_GamepadButton *assign = pachinkoShown() ? pachinko_list : list;
    const int count = pachinkoShown() ? (int)std::size(pachinko_list) : (int)std::size(list);

    for (auto i = 0; i < count; i++)
    {
        if (!bindings.contains({ SDL_GAMEPAD_BINDTYPE_BUTTON, assign[i] }))
            continue;

        auto &sdl_binding = bindings[{SDL_GAMEPAD_BINDTYPE_BUTTON, assign[i]}];
        if (SDL_GAMEPAD_BINDTYPE_BUTTON == sdl_binding.input_type)
            buttons[4 * i + slot] = EmuBinding::joystick_button(device.index, sdl_binding.input.button);
        else if (SDL_GAMEPAD_BINDTYPE_HAT == sdl_binding.input_type)
            buttons[4 * i + slot] = EmuBinding::joystick_hat(device.index, sdl_binding.input.hat.hat, sdl_binding.input.hat.hat_mask);
        else if (SDL_GAMEPAD_BINDTYPE_AXIS == sdl_binding.input_type)
            buttons[4 * i + slot] = EmuBinding::joystick_axis(device.index, sdl_binding.input.axis.axis, sdl_binding.input.axis.axis);
    }

    fillTable();
    app->updateBindings();
}

void ControllerPanel::swapControllers(int first, int second)
{
    auto &a = app->config->binding.controller[first].buttons;
    auto &b = app->config->binding.controller[second].buttons;

    int count = std::size(a);
    for (int i = 0; i < count; i++)
    {
        EmuBinding swap = b[i];
        b[i] = a[i];
        a[i] = swap;
    }

    app->updateBindings();
}

void ControllerPanel::clearCurrentController()
{
    for (int i = 0; i < table_width * table_height; i++)
        binding[i] = {};
    fillTable();
    app->updateBindings();
}

void ControllerPanel::clearAllControllers()
{
    for (auto &c : app->config->binding.controller)
        for (auto &b : c.buttons)
            b = {};
    for (auto &b : app->config->binding.pachinko.buttons)
        b = {};
    fillTable();
    app->updateBindings();
}

bool ControllerPanel::pachinkoShown()
{
    return controllerComboBox->currentIndex() == kPachinkoController;
}

// The picture button a table row stands for.
int ControllerPanel::rowButton(int row)
{
    if (pachinkoShown())
        return row >= 0 && row < EmuConfig::num_pachinko_bindings ? kPachinkoRowButtons[row] : 0;
    return row >= 0 && row < EmuConfig::num_controller_bindings ? kRowButtons[row] : 0;
}

// A pad's eighteen rows, or the Sunsoft Pachinko's three with its picture.
void ControllerPanel::showController(int index)
{
    const bool pachinko = index == kPachinkoController;
    const QString pachinko_names[EmuConfig::num_pachinko_bindings] = { tr("Stronger"), tr("Weaker"), tr("Stop") };

    BindingPanel::binding = pachinko ? app->config->binding.pachinko.buttons : app->config->binding.controller[index].buttons;
    table_height = pachinko ? EmuConfig::num_pachinko_bindings : EmuConfig::num_controller_bindings;
    for (int row = 0; row < EmuConfig::num_controller_bindings; row++)
    {
        auto header = tableWidget_controller->verticalHeaderItem(row);
        const bool renamed = pachinko && row < EmuConfig::num_pachinko_bindings;
        header->setText(renamed ? pachinko_names[row] : pad_row_names[row]);
        header->setIcon(renamed ? QIcon() : pad_row_icons[row]);
        tableWidget_controller->setRowHidden(row, pachinko && !renamed);
    }
    pad_picture->setPachinko(pachinko);
    fillTable();
    awaiting_binding = false;
    updatePadPicture();
}

void ControllerPanel::showEvent(QShowEvent *event)
{
    BindingPanel::showEvent(event);
    recreateAutoAssignMenu();
    portComboBox->setCurrentIndex(app->config->port_configuration);
    automapGamepadsCheckbox->setChecked(app->config->automap_gamepads);

    // Gamepads report here whatever has focus; keys come through eventFilter.
    held_keys.clear();
    held_joystick.clear();
    app->binding_monitor = [&](const EmuBinding &b, bool pressed) {
        if (b.type != EmuBinding::Joystick)
            return;
        auto it = std::find(held_joystick.begin(), held_joystick.end(), b.hash());
        if (pressed && it == held_joystick.end())
            held_joystick.push_back(b.hash());
        else if (!pressed && it != held_joystick.end())
            held_joystick.erase(it);
    };
    app->qtapp->installEventFilter(this);
    pad_picture->setPadStyle(app->config->pad_picture_style);
    pachinko_preview.reset();
    pad_picture->setPower(0);
    pad_picture_timer.start();
    updatePadPicture();
}

void ControllerPanel::hideEvent(QHideEvent *event)
{
    pad_picture_timer.stop();
    app->qtapp->removeEventFilter(this);
    app->binding_monitor = nullptr;
    held_keys.clear();
    held_joystick.clear();
    BindingPanel::hideEvent(event);
}

// Keys held for the pad picture. One bound on this pad lights the picture rather than moving
// around the dialog: arrows on the table or a list, Enter starting a binding.
bool ControllerPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ApplicationDeactivate)
        held_keys.clear();
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
        return false;
    auto widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window())
        return false;

    auto key_event = static_cast<QKeyEvent *>(event);
    auto mods = key_event->modifiers();
    auto b = EmuBinding::keyboard(key_event->key(),
                                  mods.testFlag(Qt::ShiftModifier),
                                  mods.testFlag(Qt::AltModifier),
                                  mods.testFlag(Qt::ControlModifier),
                                  mods.testFlag(Qt::MetaModifier));
    bool bound;
    if (event->type() == QEvent::KeyPress)
    {
        bound = boundHere(b);
        if (!key_event->isAutoRepeat())
            held_keys[key_event->key()] = b;
    }
    else
    {
        auto it = held_keys.find(key_event->key());
        bound = boundHere(it != held_keys.end() ? it->second : b);
        if (!key_event->isAutoRepeat() && it != held_keys.end())
            held_keys.erase(it);
    }

    // While a binding is awaited the key goes to it.
    return !awaiting_binding && bound;
}

bool ControllerPanel::boundHere(const EmuBinding &b)
{
    for (int i = 0; i < table_height * EmuConfig::allowed_bindings; i++)
        if (binding[i].type != EmuBinding::None && binding[i].hash() == b.hash())
            return true;
    return false;
}

int ControllerPanel::heldButtons()
{
    int lit = 0;
    for (int row = 0; row < table_height; row++)
        for (int column = 0; column < EmuConfig::allowed_bindings; column++)
        {
            const EmuBinding &b = binding[row * EmuConfig::allowed_bindings + column];
            if (b.type == EmuBinding::None)
                continue;
            bool held = std::find(held_joystick.begin(), held_joystick.end(), b.hash()) != held_joystick.end();
            for (auto &key : held_keys)
                held = held || key.second.hash() == b.hash();
            if (held)
                lit |= rowButton(row);
        }
    return lit;
}

// The button of the row being edited, or of the selected row while the table has focus.
int ControllerPanel::markedButtons()
{
    int row = -1;
    if (awaiting_binding)
        row = cell_row;
    else if (tableWidget_controller->hasFocus())
        row = tableWidget_controller->currentRow();
    return rowButton(row);
}

void ControllerPanel::updatePadPicture()
{
    const int held = heldButtons();
    pad_picture->setLit(held);
    pad_picture->setMarked(markedButtons());
    if (pachinkoShown() && pachinko_preview.turn(held))
        pad_picture->setPower(pachinko_preview.power());
}
