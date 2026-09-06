#include <algorithm>
#include <QTimer>
#include <QMenu>
#include <QMenuBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QtEvents>
#include <QGuiApplication>
#include <QActionGroup>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif

#include "CheatsDialog.hpp"
#include "ColorCorrectionDialog.hpp"
#include "StatePreviewDialog.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#include "snes9x.h"
#ifdef RETROACHIEVEMENTS_SUPPORT
#include "RAIntegrationQt.hpp"
#include "retroachievements.h"
#endif
#ifdef KAILLERA_SUPPORT
#include "KailleraIntegrationQt.hpp"
#include "kaillera_client.h"
#include "kaillera_server.h"
#endif
#include "EmuBinding.hpp"
#include "EmuCanvasOpenGL.hpp"
#include "EmuCanvasQt.hpp"
#ifndef __APPLE__
#include "EmuCanvasVulkan.hpp"
#endif
#include "EmuMainWindow.hpp"
#include "BiosManagerDialog.hpp"
#include "EmuPoTranslator.hpp"
#include "EmuSettingsWindow.hpp"
#include "memmap.h"
#include "display.h"
#include "msu1.h"
#include "voicekun.h"

#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#undef KeyPress

static EmuSettingsWindow *g_emu_settings_window = nullptr;

class DefaultBackground
    : public QWidget
{
public:
    explicit DefaultBackground(QWidget *parent)
        : QWidget(parent)
    {
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter paint(this);
        QLinearGradient gradient(0.0, 0.0, 0.0, event->rect().height());
        gradient.setColorAt(0.0, QColor(0, 0, 128));
        gradient.setColorAt(1.0, QColor(0, 0, 0));

        paint.setBrush(QBrush(gradient));
        paint.drawRect(0, 0, event->rect().width(), event->rect().height());
    }
};

EmuMainWindow::EmuMainWindow(EmuApplication *app)
    : app(app)
{
    createWidgets();
    recreateCanvas();
    setMouseTracking(true);

    app->qtapp->installEventFilter(this);
    mouse_timer.setTimerType(Qt::CoarseTimer);
    mouse_timer.setInterval(1000);
    mouse_timer.callOnTimeout([&] {
        if (cursor_visible && isActivelyDrawing()) {
        if (canvas)
            canvas->setCursor(QCursor(Qt::BlankCursor));
        cursor_visible = false;
        mouse_timer.stop();
        }
    });
}

EmuMainWindow::~EmuMainWindow() = default;

void EmuMainWindow::destroyCanvas()
{
    auto central_widget = centralWidget();
    if (!central_widget)
        return;

    auto widget = (EmuCanvas *)takeCentralWidget();
    widget->deinit();
    delete widget;

    canvas = nullptr;
}

bool EmuMainWindow::createCanvas()
{
    auto fallback = [this]() -> bool {
        std::string failed = app->config->display_driver;
        std::string next = (failed == "vulkan") ? "opengl" : "qt";
        QMessageBox::warning(
            this, tr("Unable to Start Display Driver"),
            tr("Unable to create a %1 context. Attempting to use %2.")
                .arg(QString::fromUtf8(failed))
                .arg(QString::fromUtf8(next)));
        app->config->display_driver = next;
        return createCanvas();
    };

    if (app->config->display_driver != "vulkan" &&
        app->config->display_driver != "opengl" &&
        app->config->display_driver != "qt")
        app->config->display_driver = "qt";

#ifdef __APPLE__
    // macOS has no native Vulkan; the driver is not built on this platform.
    if (app->config->display_driver == "vulkan")
        app->config->display_driver = "opengl";
#endif

    if (app->config->display_driver == "vulkan")
    {
#ifndef __APPLE__
        canvas = new EmuCanvasVulkan(app->config.get(), this);
        QGuiApplication::processEvents();
        if (!canvas->createContext())
        {
            delete canvas;
            return fallback();
        }
#endif
    }
    else if (app->config->display_driver == "opengl")
    {
        canvas = new EmuCanvasOpenGL(app->config.get(), this);
        QGuiApplication::processEvents();
#ifdef __APPLE__
        // -[NSOpenGLContext setView:] is main-thread only, so the context is
        // built here and simply made current on the emulation thread later.
        if (!canvas->createContext())
        {
            delete canvas;
            return fallback();
        }
#else
        // The call blocks, so context_created is safely written before the
        // check below runs. A false result (e.g. Wayland on a Qt build older
        // than 6.5, or broken GL drivers) falls back to the software driver
        // instead of leaving a dead canvas that crashes on first use.
        bool context_created = false;
        app->emu_thread->runOnThread([&] { context_created = canvas->createContext(); }, true);
        if (!context_created)
        {
            delete canvas;
            return fallback();
        }
#endif
    }
    else
        canvas = new EmuCanvasQt(app->config.get(), this);

    setCentralWidget(canvas);
    
    if (QGuiApplication::platformName() == "wayland")
    {
        // Qt 6.10+ has a bug with delayed widget repositioning, causing us to get
        // incorrect coordinates respective to the parent on Wayland.
        // This forces widget reflow.
        auto saved_width = width(), saved_height = height();
        resize(width() + 1, height());
        resize(saved_width, saved_height);
    }

    return true;
}

void EmuMainWindow::recreateCanvas()
{
    if (!canvas)
        return;

    app->suspendThread();
    destroyCanvas();
    createCanvas();

    app->unsuspendThread();
    updateShaderSettingsItem();
}

void EmuMainWindow::setCoreActionsEnabled(bool enable)
{
    for (auto &a : core_actions)
        a->setEnabled(enable);
}

void EmuMainWindow::setGBBootPolicy(int policy)
{
    // Same gates the menu shows: nothing to switch with no Game Boy content
    // loaded, and nothing to switch to when the console has no BIOS installed.
    if (!Settings.GBRomPath[0])
        return;
    if (!S9xGBBootPolicyAvailable(policy, Settings.GBRomPath))
        return;

    // Reload only on a real change, but write it either way: a config holding
    // one of the retired Automatic values loads as this one, so the menu shows
    // it chosen while the file still says 5 or 6.
    const bool changed = (Settings.GBBootPolicy != (uint8_t) policy);
    Settings.GBBootPolicy       = (uint8_t) policy;
    app->config->gb_boot_policy = Settings.GBBootPolicy;
    if (changed)
        openFile(std::string(Settings.GBRomPath));
}

void EmuMainWindow::openBiosManager()
{
    BiosManagerDialog dialog(this, app);
    if (dialog.exec() != QDialog::Accepted)
        return;
    app->config->saveFile(EmuConfig::findConfigFile());
    // The running cart keeps the BIOS it was loaded against until a hard reset
    // or the next load - these paths are only read by a load. What changes now
    // is which Game Boy Model entries are selectable.
    refreshBiosMenu();
}

void EmuMainWindow::powerCycle()
{
    // A BIOS assigned since the cart loaded is only read by a load, and a power
    // cycle is when it should take over; reload or reset, never both. Qt has no
    // multicart UI, so the one file the game came from is enough to load it.
    const std::string path = Settings.GBRomPath[0] ? std::string(Settings.GBRomPath)
                                                   : Memory.ROMFilename;
    if (S9xBiosChangedSinceLoad() && !path.empty())
        openFile(path);
    else
        app->powerCycle();
}

void EmuMainWindow::refreshBiosMenu()
{
    if (!bios_menu_action)
        return;

    const bool gb_loaded = (Settings.GBRomPath[0] != '\0');
    bios_menu_action->setVisible(gb_loaded);
    if (!gb_loaded)
        return;

    // Check the chosen console, not the one that ended up running: this is a
    // saved preference, and one whose BIOS has gone missing stays checked but
    // greyed, so the menu says what was picked and that it cannot be honoured.
    const uint8_t policy = S9xNormalizeGBBootPolicy(Settings.GBBootPolicy);
    const QString missing      = tr(" (Missing BIOS)");
    const QString experimental = tr(" (Experimental)");
    for (int n = 0; n < S9xGBBootPolicyMenuCount; n++)
    {
        const int i = S9xGBBootPolicyMenuOrder[n];
        const S9xGBPolicyBlock why = S9xGBBootPolicyBlocked(i, Settings.GBRomPath);
        const bool have = (why == S9X_GBPOLICY_OK);
        bios_policy_actions[i]->setChecked(i == policy);
        bios_policy_actions[i]->setEnabled(have);

        // Say why it is greyed, on the item. Taken off the current text rather
        // than a cached original, so a retranslation still lands on the base.
        QString base = bios_policy_actions[i]->text();
        for (const QString &tail : { missing, experimental })
            if (base.endsWith(tail))
                base.chop(tail.size());
        // Only when a BIOS is what is missing: a hack greyed because the cart
        // is mono has nothing missing to install.
        // The experimental tag yields to a missing-BIOS note rather than stacking.
        bios_policy_actions[i]->setText(
            why == S9X_GBPOLICY_NO_BIOS ? base + missing :
            i == S9X_GBBOOT_SGBC        ? base + experimental : base);
    }
}

void EmuMainWindow::refreshVoicekunMenu()
{
    if (!voicekun_menu_action)
        return;

    const bool supported = S9xVoiceKunGameSupported();
    voicekun_menu_action->setVisible(supported);
    if (!supported)
        return;

    const bool attached = S9xVoiceKunAttached();
    voicekun_attach_action->setEnabled(!attached);
    voicekun_detach_action->setEnabled(attached);
}

void EmuMainWindow::voicekunAttach()
{
    if (S9xVoiceKunAttached())
        return;

    app->pause();

    QFileDialog dialog(this, tr("Attach Audio CD (.cue or .zip)"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters({ tr("Audio CD Images (*.cue *.zip)"), tr("All Files (*)") });

    if (!dialog.exec() || dialog.selectedFiles().empty())
    {
        app->unpause();
        return;
    }

    auto filename = dialog.selectedFiles()[0].toStdString();

    if (S9xVoiceKunAttach(filename.c_str()))
    {
        auto message = tr("Voicer-kun: %1 verified, %2 tracks (%3)")
            .arg(S9xVoiceKunDiscLabel())
            .arg(S9xVoiceKunTrackCount())
            .arg(S9xVoiceKunGameTitle());
        S9xSetInfoString(message.toUtf8().constData());
    }
    else
    {
        QMessageBox::critical(this, tr("Voicer-kun"),
            tr("This audio CD was not accepted:\n%1").arg(S9xVoiceKunLastError()));
    }

    app->unpause();
}

void EmuMainWindow::voicekunDetach()
{
    if (S9xVoiceKunAttached())
    {
        S9xVoiceKunDetach();
        S9xSetInfoString("Voicer-kun: audio CD ejected");
    }
}

void EmuMainWindow::createWidgets()
{
    setWindowTitle(QString("SuperSnes9x %1").arg(VERSION_DISPLAY));
    if (QIcon::hasThemeIcon("snes9x"))
        setWindowIcon(QIcon::fromTheme("snes9x"));
    else
        setWindowIcon(QIcon(":/icons/snes9x.svg"));

#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref,
                          sizeof(cornerPref));
#endif

    auto iconset = app->iconPrefix();

    // File menu
    auto file_menu = new QMenu(tr("&File"));
    auto open_item = file_menu->addAction(QIcon(iconset + "open.svg"), tr("&Open File..."));
    connect(open_item, &QAction::triggered, this, [&] {
        openFile();
    });
    // File->Recent Files submenu
    recent_menu = new QMenu("Recent Files");
    file_menu->addMenu(recent_menu);
    populateRecentlyUsed();

    file_menu->addSeparator();

    // File->Load/Save State submenus
    load_state_menu = new QMenu(tr("&Load State"));
    save_state_menu = new QMenu(tr("&Save State"));
    // One submenu per bank, each holding one item per slot, as on win32.
    for (int bank = 0; bank < EmuConfig::num_save_banks; bank++)
    {
        auto load_bank_menu = load_state_menu->addMenu(tr("Bank &%1").arg(bank));
        auto save_bank_menu = save_state_menu->addMenu(tr("Bank &%1").arg(bank));

        for (int slot = 0; slot < EmuConfig::save_slots_per_bank; slot++)
        {
            int index = bank * EmuConfig::save_slots_per_bank + slot;

            auto action = load_bank_menu->addAction(tr("Slot &%1").arg(slot));
            connect(action, &QAction::triggered, [&, index] {
                app->loadState(index);
            });
            core_actions.push_back(action);

            action = save_bank_menu->addAction(tr("Slot &%1").arg(slot));
            connect(action, &QAction::triggered, [&, index] {
                app->saveState(index);
            });
            core_actions.push_back(action);
        }
    }

    load_state_menu->addSeparator();

    auto load_state_file_item = load_state_menu->addAction(QIcon(iconset + "open.svg"), tr("From &File..."));
    connect(load_state_file_item, &QAction::triggered, [&] {
        this->chooseState(false);
    });
    core_actions.push_back(load_state_file_item);

    load_state_menu->addSeparator();

    auto load_state_undo_item = load_state_menu->addAction(QIcon(iconset + "refresh.svg"), tr("&Undo Load State"));
    connect(load_state_undo_item, &QAction::triggered, [&] {
        app->loadUndoState();
    });
    core_actions.push_back(load_state_undo_item);

    file_menu->addMenu(load_state_menu);

    save_state_menu->addSeparator();
    auto save_state_file_item = save_state_menu->addAction(QIcon(iconset + "save.svg"), tr("To &File..."));
    connect(save_state_file_item, &QAction::triggered, [&] {
        this->chooseState(true);
    });
    core_actions.push_back(save_state_file_item);
    file_menu->addMenu(save_state_menu);

    auto save_preview_item = file_menu->addAction(tr("Sa&ve with Preview..."));
    connect(save_preview_item, &QAction::triggered, [&] {
        this->statePreviewDialog(true);
    });
    core_actions.push_back(save_preview_item);

    auto load_preview_item = file_menu->addAction(tr("Loa&d with Preview..."));
    connect(load_preview_item, &QAction::triggered, [&] {
        this->statePreviewDialog(false);
    });
    core_actions.push_back(load_preview_item);

    file_menu->addSeparator();
    auto bios_manager_item = file_menu->addAction(tr("&BIOS Manager..."));
    connect(bios_manager_item, &QAction::triggered, this, [this] {
        openBiosManager();
    });

    auto languages = EmuPoTranslator::availableLanguages();
    if (languages.size() > 1)
    {
        auto language_menu = new QMenu(tr("&Language"), file_menu);
        auto language_group = new QActionGroup(language_menu);
        for (const auto &lang : languages)
        {
            QString code = lang.code;
            auto action = language_menu->addAction(lang.name);
            action->setCheckable(true);
            action->setActionGroup(language_group);
            action->setChecked(code.toStdString() == app->config->language);
            connect(action, &QAction::triggered, this, [this, code] {
                app->config->language = code.toStdString();
                app->config->saveFile(EmuConfig::findConfigFile());
                QMessageBox::information(
                    this, tr("Language"),
                    tr("The language change will take effect after you restart SuperSnes9x."));
            });
        }
        file_menu->addMenu(language_menu);
        file_menu->addSeparator();
    }

    auto exit_item = new QAction(QIcon(iconset + "exit.svg"), tr("E&xit"));
    connect(exit_item, &QAction::triggered, this, [&](bool checked) {
        close();
    });

    file_menu->addAction(exit_item);
    menuBar()->addMenu(file_menu);

    // Emulation Menu
    auto emulation_menu = new QMenu(tr("&Emulation"));

    auto run_item = emulation_menu->addAction(tr("&Run"));
    connect(run_item, &QAction::triggered, [&] {
        if (manual_pause)
        {
            manual_pause = false;
            app->unpause();
        }
    });
    core_actions.push_back(run_item);

    auto pause_item = emulation_menu->addAction(QIcon(iconset + "pause.svg"), tr("&Pause"));
    connect(pause_item, &QAction::triggered, [&] {
        if (!manual_pause)
        {
            manual_pause = true;
            app->pause();
        }
    });
    core_actions.push_back(pause_item);

    emulation_menu->addSeparator();

    auto reset_item = emulation_menu->addAction(QIcon(iconset + "refresh.svg"), tr("Rese&t"));
    connect(reset_item, &QAction::triggered, [&] {
        app->reset();
        if (manual_pause)
        {
            manual_pause = false;
            app->unpause();
        }
    });
    core_actions.push_back(reset_item);

    auto hard_reset_item = emulation_menu->addAction(QIcon(iconset + "reset.svg"), tr("&Hard Reset"));
    connect(hard_reset_item, &QAction::triggered, [&] {
        powerCycle();
        if (manual_pause)
        {
            manual_pause = false;
            app->unpause();
        }
    });
    core_actions.push_back(hard_reset_item);

    emulation_menu->addSeparator();

    auto cheats_item = emulation_menu->addAction(tr("&Cheats"));
    connect(cheats_item, &QAction::triggered, [&] {
        if (!cheats_dialog)
            cheats_dialog = std::make_unique<CheatsDialog>(this, app);
        cheats_dialog->show();
    });
    core_actions.push_back(cheats_item);

    emulation_menu->addSeparator();

    auto run_ahead_menu = new QMenu(tr("Run &Ahead"));
    auto run_ahead_group = new QActionGroup(this);
    run_ahead_group->setExclusive(true);
    std::vector<QAction *> run_ahead_actions;
    for (int i = 0; i <= 4; i++)
    {
        auto action = run_ahead_menu->addAction(
            i == 0 ? tr("&0 (off)") :
            i == 1 ? tr("&1 frame") :
                     tr("&%1 frames").arg(i));
        action->setCheckable(true);
        run_ahead_group->addAction(action);
        connect(action, &QAction::triggered, [&, i] {
            app->config->run_ahead_frames = i;
            app->updateSettings();
        });
        run_ahead_actions.push_back(action);
    }
    core_actions.push_back(emulation_menu->addMenu(run_ahead_menu));
    // The Emulation settings panel can also change the value, so sync the
    // check state whenever the menu opens.
    connect(emulation_menu, &QMenu::aboutToShow, this, [this, run_ahead_actions] {
        int n = app->config->run_ahead_frames;
        n = n < 0 ? 0 : (n > 4 ? 4 : n);
        run_ahead_actions[n]->setChecked(true);
    });

    // Which console GB content runs on. Its BIOS is used when one is assigned
    // and skipped when not, so there is nothing else to pick here.
    bios_menu = new QMenu(tr("Game &Boy Model"));
    auto bios_group = new QActionGroup(this);
    bios_group->setExclusive(true);
    int prev_group = -1;
    for (int n = 0; n < S9xGBBootPolicyMenuCount; n++)
    {
        const int i     = S9xGBBootPolicyMenuOrder[n];
        const int group = S9xGBBootPolicyGroup(i);
        if (prev_group >= 0 && group != prev_group)
            bios_menu->addSeparator();
        prev_group = group;

        QAction *a = bios_menu->addAction(tr(S9xGBBootPolicyName(i)));
        a->setCheckable(true);
        bios_group->addAction(a);
        bios_policy_actions[i] = a;
        connect(a, &QAction::triggered, [this, i] { setGBBootPolicy(i); });
    }
    bios_policy_actions[S9X_GBBOOT_GBC]->setToolTip(
        tr("Colorizes Game Boy Color carts. Mono-only carts are experimental — "
           "DMG-compatibility colorization is not implemented yet."));
    bios_policy_actions[S9X_GBBOOT_SGBC]->setToolTip(
        tr("A Super Game Boy 2 with a Game Boy Color inside: real Color graphics in the "
           "SGB border, with the cart's SGB sound and border packets. No such hardware "
           "exists; the SGB2 BIOS is patched in memory for it."));
    bios_menu_action = emulation_menu->addMenu(bios_menu);
    bios_menu_action->setVisible(false);
    connect(emulation_menu, &QMenu::aboutToShow, this, &EmuMainWindow::refreshBiosMenu);

    // Koei Voicer-kun audio CD: only offered while a supported game is loaded.
    voicekun_menu = new QMenu(tr("Voicer-&kun"));
    voicekun_attach_action = voicekun_menu->addAction(tr("&Attach Audio CD..."));
    connect(voicekun_attach_action, &QAction::triggered, this, &EmuMainWindow::voicekunAttach);
    voicekun_detach_action = voicekun_menu->addAction(tr("&Eject Audio CD"));
    connect(voicekun_detach_action, &QAction::triggered, this, &EmuMainWindow::voicekunDetach);
    voicekun_menu_action = emulation_menu->addMenu(voicekun_menu);
    voicekun_menu_action->setVisible(false);
    connect(emulation_menu, &QMenu::aboutToShow, this, &EmuMainWindow::refreshVoicekunMenu);

    menuBar()->addMenu(emulation_menu);

    // Sound Menu, mirroring win32's Sound menu (Channels popup + Mute).
    auto sound_menu = new QMenu(tr("&Sound"));

    auto channels_menu = new QMenu(tr("&Channels"));
    std::array<QAction *, 8> channel_actions{};
    for (int i = 0; i < 8; i++)
    {
        auto action = channels_menu->addAction(tr("Channel &%1").arg(i + 1));
        action->setCheckable(true);
        action->setChecked(true);
        connect(action, &QAction::triggered, [&, i](bool checked) {
            uint8_t mask = app->getSoundChannelMask();
            if (checked)
                mask |= 1 << i;
            else
                mask &= ~(1 << i);
            app->setSoundChannelMask(mask);
        });
        channel_actions[i] = action;
    }
    channels_menu->addSeparator();
    auto enable_all_channels_item = channels_menu->addAction(tr("Enable All"));
    connect(enable_all_channels_item, &QAction::triggered, [&] {
        app->setSoundChannelMask(255);
    });
    core_actions.push_back(sound_menu->addMenu(channels_menu));

    sound_menu->addSeparator();

    // Same setting as the Sound panel's "Mute all sound" checkbox.
    auto mute_item = sound_menu->addAction(tr("&Mute"));
    mute_item->setCheckable(true);
    connect(mute_item, &QAction::triggered, [&](bool checked) {
        app->config->mute_audio = checked;
        app->updateSettings();
    });

    sound_menu->addSeparator();

    auto sound_settings_item = sound_menu->addAction(QIcon(iconset + "sound.svg"), tr("&Settings..."));
    connect(sound_settings_item, &QAction::triggered, [&] {
        if (!g_emu_settings_window)
            g_emu_settings_window = new EmuSettingsWindow(this, app);
        g_emu_settings_window->show(2); // the Sound panel
    });

    connect(sound_menu, &QMenu::aboutToShow, this, [this, channel_actions, mute_item] {
        const uint8_t mask = app->getSoundChannelMask();
        // Channels 1-4 drive both SPC voices 1-4 and the GB APU's CH1-CH4.
        // In BIOS-less GB mode the SPC isn't running, so 5-8 control
        // nothing — grey them there, as on win32.
        const bool gb_only = Settings.SuperGameBoy && !Settings.SGB_BIOSModeActive;
        for (int i = 0; i < 8; i++)
        {
            channel_actions[i]->setChecked(mask & (1 << i));
            if (i >= 4)
                channel_actions[i]->setEnabled(!gb_only);
        }
        mute_item->setChecked(app->config->mute_audio);
    });

    menuBar()->addMenu(sound_menu);

    // View Menu
    auto view_menu = new QMenu(tr("&View"));

    // Set Size Menu
    auto set_size_menu = new QMenu(tr("&Set Size"));
    for (size_t i = 1; i <= 10; i++)
    {
        auto string = (i == 10) ? tr("1&0x") : tr("&%1x").arg(i);
        auto item = set_size_menu->addAction(string);
        connect(item, &QAction::triggered, this, [&, i](bool checked) {
            resizeToMultiple(i);
        });
    }
    view_menu->addMenu(set_size_menu);

    view_menu->addSeparator();

    auto fullscreen_item = new QAction(QIcon(iconset + "fullscreen.svg"), tr("&Fullscreen"));
    view_menu->addAction(fullscreen_item);
    connect(fullscreen_item, &QAction::triggered, [&](bool checked) {
        toggleFullscreen();
    });

    view_menu->addSeparator();

    auto color_correction_item = view_menu->addAction(tr("&Color Correction..."));
    connect(color_correction_item, &QAction::triggered, [&] {
        ColorCorrectionDialog dialog(app, this);
        dialog.exec();
    });

    menuBar()->addMenu(view_menu);

    // Options Menu
    auto options_menu = new QMenu(tr("&Options"));



    std::array<QString, 7> setting_panels = { tr("&General..."),
                                              tr("&Display..."),
                                              tr("&Sound..."),
                                              tr("&Emulation..."),
                                              tr("&Controllers..."),
                                              tr("Shortcu&ts..."),
                                              tr("&Files...") };
    const char *setting_icons[] = { "settings.svg",
                                    "display.svg",
                                    "sound.svg",
                                    "emulation.svg",
                                    "joypad.svg",
                                    "keyboard.svg",
                                    "folders.svg" };

    for (int i = 0; i < setting_panels.size(); i++)
    {
        auto action = options_menu->addAction(QIcon(iconset + setting_icons[i]), setting_panels[i]);
        QObject::connect(action, &QAction::triggered, [&, i] {
            if (!g_emu_settings_window)
                g_emu_settings_window = new EmuSettingsWindow(this, app);
            g_emu_settings_window->show(i);
        });
    }

    options_menu->addSeparator();
    shader_settings_item = new QAction(QIcon(iconset + "shader.svg"), tr("S&hader Settings..."));
    QObject::connect(shader_settings_item, &QAction::triggered, [&] {
        if (canvas)
            canvas->showParametersDialog();
    });
    options_menu->addAction(shader_settings_item);
    updateShaderSettingsItem();

    options_menu->addSeparator();

    // win32's Input->Enable Rumble (Shake): pass LRG rumble-cart motor
    // effects to the port-1 gamepad.
    auto rumble_item = options_menu->addAction(tr("Enable &Rumble (Shake)"));
    rumble_item->setCheckable(true);
    rumble_item->setChecked(app->config->enable_rumble);
    QObject::connect(rumble_item, &QAction::triggered, [&](bool checked) {
        app->config->enable_rumble = checked;
    });

    menuBar()->addMenu(options_menu);

#ifdef RETROACHIEVEMENTS_SUPPORT
    auto ra_menu = new QMenu(tr("&RetroAchievements"));

    ra_enabled_action = ra_menu->addAction(tr("&Enabled"));
    ra_enabled_action->setCheckable(true);
    ra_enabled_action->setChecked(app->config->ra_enabled);
    connect(ra_enabled_action, &QAction::triggered, [&](bool checked) {
        app->config->ra_enabled = checked;
        app->config->saveFile(EmuConfig::findConfigFile());
        RA_SetEnabled(checked);
        if (checked)
        {
            RA_Qt_RegisterCallbacks(app);
            RA_Init();
            RA_AttemptLogin(app->config->ra_username.c_str(), app->config->ra_api_token.c_str());
            if (app->isCoreActive())
                RA_OnLoadROM();
        }
        else
        {
            RA_Shutdown();
        }
    });

    ra_login_action = ra_menu->addAction(tr("&Login..."));
    connect(ra_login_action, &QAction::triggered, [&] {
        RA_Qt_RegisterCallbacks(app);
        RA_Init();
        if (RA_IsLoggedIn())
        {
            RA_Logout();
            ra_login_action->setText(tr("&Login..."));
        }
        else
        {
            RA_ShowLoginDialog();
        }
    });

    ra_hardcore_action = ra_menu->addAction(tr("&Hardcore Mode"));
    ra_hardcore_action->setCheckable(true);
    ra_hardcore_action->setChecked(app->config->ra_hardcore_mode);
    connect(ra_hardcore_action, &QAction::triggered, [&](bool checked) {
        app->config->ra_hardcore_mode = checked;
        RA_SetHardcoreEnabled(checked);
    });

    auto ra_ua_action = ra_menu->addAction(QString("User Agent: SuperSnes9x/%1.%2").arg(VERSION).arg(RA_MINOR_VERSION));
    ra_ua_action->setCheckable(true);
    ra_ua_action->setChecked(true);
    ra_ua_action->setEnabled(true);

    ra_menu->addSeparator();

    ra_achievements_action = ra_menu->addAction(tr("&Achievement List..."));
    ra_achievements_action->setEnabled(false);
    connect(ra_achievements_action, &QAction::triggered, [&] {
        RA_ShowAchievementList();
    });

    ra_view_profile_action = ra_menu->addAction(tr("&View Profile"));
    ra_view_profile_action->setEnabled(false);
    connect(ra_view_profile_action, &QAction::triggered, [&] {
        rc_client_t *client = RA_GetClient();
        const rc_client_user_t *user = client ? rc_client_get_user_info(client) : nullptr;
        if (user && user->username && user->username[0])
            QDesktopServices::openUrl(QUrl(
                QString("https://retroachievements.org/user/%1").arg(user->username)));
    });

    connect(ra_menu, &QMenu::aboutToShow, [this] {
        bool enabled = app->config->ra_enabled;
        ra_enabled_action->setChecked(enabled);
        ra_login_action->setEnabled(enabled);
        ra_hardcore_action->setEnabled(enabled);
        ra_achievements_action->setEnabled(enabled && app->isCoreActive() && RA_IsLoggedIn());
        ra_view_profile_action->setEnabled(enabled && RA_IsLoggedIn());
    });

    menuBar()->addMenu(ra_menu);
#endif

#ifdef KAILLERA_SUPPORT
    auto netplay_menu = new QMenu(tr("&Netplay"));

    auto kaillera_connect_action = netplay_menu->addAction(tr("Kaillera &Netplay..."));
    connect(kaillera_connect_action, &QAction::triggered, [&] {
        Kaillera_Qt_RegisterCallbacks(app);
        Kaillera_Qt_ShowConnectDialog();
    });

    kaillera_host_action = netplay_menu->addAction(tr("&Host Kaillera Server..."));
    kaillera_host_action->setCheckable(true);
    kaillera_host_action->setChecked(KailleraServerIsRunning());
    connect(kaillera_host_action, &QAction::triggered, [&] {
        Kaillera_Qt_RegisterCallbacks(app);
        Kaillera_Qt_ShowHostDialog();
        kaillera_host_action->setChecked(KailleraServerIsRunning());
    });

    netplay_menu->addSeparator();

    kaillera_end_action = netplay_menu->addAction(tr("&End Kaillera Game"));
    kaillera_end_action->setEnabled(false);
    connect(kaillera_end_action, &QAction::triggered, [&] {
        if (KailleraClientGetState() == KCLIENT_PLAYING ||
            KailleraClientGetState() == KCLIENT_GAME_STARTING)
        {
            KailleraClientEndGame();
            // on_game_ended callback will reopen the dialog
        }
        kaillera_end_action->setEnabled(false);
    });

    menuBar()->addMenu(netplay_menu);
#endif

    setCoreActionsEnabled(false);

    if (app->config->main_window_width != 0 && app->config->main_window_height != 0)
        resize(app->config->main_window_width, app->config->main_window_height);

    setCentralWidget(new DefaultBackground(this));
}

void EmuMainWindow::resizeToMultiple(int multiple)
{
    double hidpi_height = 224 / devicePixelRatioF();
    resize((hidpi_height * multiple) * app->config->aspect_ratio_numerator / app->config->aspect_ratio_denominator, (hidpi_height * multiple) + menuBar()->height());
}

void EmuMainWindow::setBypassCompositor(bool bypass)
{
    // _NET_WM_BYPASS_COMPOSITOR is an X11 EWMH hint; macOS and Windows have
    // no equivalent, and the Quartz compositor cannot be bypassed at all.
#if !defined(_WIN32) && !defined(__APPLE__)
    if (QGuiApplication::platformName() == "xcb")
    {
        uint32_t value = bypass;
        auto iface = app->qtapp->nativeInterface<QNativeInterface::QX11Application>();
        auto display = iface->display();
        auto xid = winId();
        Atom net_wm_bypass_compositor = XInternAtom(display, "_NET_WM_BYPASS_COMPOSITOR", False);
        XChangeProperty(display, xid, net_wm_bypass_compositor, 6, 32, PropModeReplace, (unsigned char *)&value, 1);
    }
#endif
}

/* win32's "Save/Load with Preview": pick a slot from a thumbnail grid. */
void EmuMainWindow::statePreviewDialog(bool save)
{
    if (!app->isCoreActive())
        return;

    app->pause();

    StatePreviewDialog dialog(app, this, save);
    int slot = dialog.exec() ? dialog.selection() : -1;

    if (slot >= 0)
    {
        if (save)
            app->saveState(slot);
        else
            app->loadState(slot);
    }

    app->unpause();
}

void EmuMainWindow::chooseState(bool save)
{
    app->pause();

    QFileDialog dialog(this, tr("Choose a State File"));

    dialog.setDirectory(QString::fromStdString(app->getStateFolder()));
    dialog.setNameFilters({ tr("Save States (*.sst *.oops *.undo *.0?? *.1?? *.2?? *.3?? *.4?? *.5?? *.6?? *.7?? *.8?? *.9*)"), tr("All Files (*)") });

    if (!save)
        dialog.setFileMode(QFileDialog::ExistingFile);
    else
    {
        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
    }

    if (!dialog.exec() || dialog.selectedFiles().empty())
    {
        app->unpause();
        return;
    }

    auto filename = dialog.selectedFiles()[0];

    if (!save)
        app->loadState(filename.toStdString());
    else
        app->saveState(filename.toStdString());

    app->unpause();
}

void EmuMainWindow::openFile()
{
    app->pause();
    QFileDialog dialog(this, tr("Open a ROM File"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(QString::fromStdString(app->config->last_rom_folder));
    // .gb/.gbc route into the SGB subsystem in CMemory::LoadROM, and .sgb (plus
    // any GB dump under a foreign extension) is caught by the Nintendo-logo
    // content sniff, so Game Boy carts belong in the dialog alongside SNES ones.
    dialog.setNameFilters({ tr("ROM Files (*.sfc *.smc *.swc *.fig *.gd3 *.bs *.st *.bin *.gb *.gbc *.sgb *.msu *.msu1 *.zip *.gz)"),
                            tr("Super Nintendo ROM Files (*.sfc *.smc *.swc *.fig *.gd3 *.bs *.st *.bin)"),
                            tr("Game Boy ROM Files (*.gb *.gbc *.sgb)"),
                            tr("All Files (*)") });

    if (!dialog.exec() || dialog.selectedFiles().empty())
    {
        app->unpause();
        return;
    }

    auto filename = dialog.selectedFiles()[0];
    app->config->last_rom_folder = dialog.directory().canonicalPath().toStdString();

    openFile(filename.toStdString());
    app->unpause();
}

// An MSU-1 pack handed over as a plain .zip can't work: LoadZip() picks the
// largest entry, which is a multi-megabyte .pcm track rather than the cartridge,
// and S9xMSU1OpenFile() only reaches into an archive named <rom>.msu1. Renaming
// the archive fixes both at once, so offer that instead of loading garbage.
std::string EmuMainWindow::promptRenameMSU1Pack(const std::string &filename)
{
    QFileInfo info(QString::fromStdString(filename));

    if (info.suffix().compare("zip", Qt::CaseInsensitive) != 0)
        return filename;

    if (!S9xMSU1ZipHasMSU1Data(filename.c_str()))
        return filename;

    QString renamed = info.dir().filePath(info.completeBaseName() + ".msu1");

    if (QFileInfo::exists(renamed))
    {
        QMessageBox::warning(this, tr("MSU-1 Pack Detected"),
            tr("\"%1\" holds MSU-1 audio data, but \"%2\" already exists, so it "
               "can't be renamed. Load that file instead, or unpack the archive "
               "and load the ROM from the folder.")
                .arg(info.fileName(), QFileInfo(renamed).fileName()));
        return filename;
    }

    auto answer = QMessageBox::question(this, tr("MSU-1 Pack Detected"),
        tr("\"%1\" holds MSU-1 audio data. Snes9x can only stream MSU-1 tracks "
           "out of an archive named \".msu1\".\n\n"
           "Rename it to \"%2\" and load it?")
            .arg(info.fileName(), QFileInfo(renamed).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (answer != QMessageBox::Yes)
        return filename;

    if (!QFile::rename(QString::fromStdString(filename), renamed))
    {
        QMessageBox::warning(this, tr("MSU-1 Pack Detected"),
            tr("Couldn't rename \"%1\". Check that the folder is writable.")
                .arg(info.fileName()));
        return filename;
    }

    // the .zip is gone now, so don't leave it behind in the recent list
    auto &ru = app->config->recently_used;
    ru.erase(std::remove(ru.begin(), ru.end(), filename), ru.end());

    return renamed.toStdString();
}

bool EmuMainWindow::openFile(const std::string &original_filename)
{
    auto filename = promptRenameMSU1Pack(original_filename);

    if (app->openFile(filename))
    {
        auto &ru = app->config->recently_used;
        auto it = std::ranges::find(ru, filename);
        if (it != ru.end())
            ru.erase(it);
        ru.insert(ru.begin(), filename);
        populateRecentlyUsed();
        setCoreActionsEnabled(true);
        if (!isFullScreen() && app->config->fullscreen_on_open)
            toggleFullscreen();

        if (!canvas)
            if (!createCanvas())
                return false;
        updateShaderSettingsItem();

        QApplication::sync();
        app->startGame();
        mouse_timer.start();
        return true;
    }
    return false;
}


void EmuMainWindow::populateRecentlyUsed()
{
    recent_menu->clear();

    if (app->config->recently_used.empty())
    {
        auto action = recent_menu->addAction(tr("No recent files"));
        action->setDisabled(true);
        return;
    }

    while (app->config->recently_used.size() > 10)
        app->config->recently_used.pop_back();

    for (int i = 0; i < app->config->recently_used.size(); i++)
    {
        auto &string = app->config->recently_used[i];
        auto action = recent_menu->addAction(QString("&%1: %2")
            .arg(i)
            .arg(QDir::toNativeSeparators(QString::fromStdString(string))));
        connect(action, &QAction::triggered, [&, string] {
            openFile(string);
        });
    }

    recent_menu->addSeparator();
    auto action = recent_menu->addAction(tr("Clear Recent Files"));
    connect(action, &QAction::triggered, [&] {
        app->config->recently_used.clear();
        populateRecentlyUsed();
    });
}

#undef KeyPress
#undef KeyRelease
bool EmuMainWindow::event(QEvent *event)
{
    switch (event->type())
    {
    case QEvent::Close:
        app->suspendThread();
        if (isFullScreen())
        {
            toggleFullscreen();
        }
        QGuiApplication::processEvents();
        QGuiApplication::sync();
        app->stopThread();
        if (canvas)
            canvas->deinit();
        QGuiApplication::sync();
        event->accept();
        break;
    case QEvent::Resize:
        if (!isFullScreen() && !isMaximized())
        {
            app->config->main_window_width = ((QResizeEvent *)event)->size().width();
            app->config->main_window_height = ((QResizeEvent *)event)->size().height();
        }
        break;
    case QEvent::WindowActivate:
        if (focus_pause)
        {
            focus_pause = false;
            app->unpause();
        }
        break;
    case QEvent::WindowDeactivate:
        if (mouse_grabbed)
            toggleMouseGrab();
        if (app->config->pause_emulation_when_unfocused && !focus_pause
#ifdef KAILLERA_SUPPORT
            && !KailleraClientIsPlaying()
#endif
        )
        {
            focus_pause = true;
            app->pause();
        }
        break;
    case QEvent::WindowStateChange:
    {
        auto scevent = (QWindowStateChangeEvent *)event;
        if (!(scevent->oldState() & Qt::WindowMinimized) && windowState() & Qt::WindowMinimized)
        {
            minimized_pause = true;
            app->pause();
        }
        else if (minimized_pause && !(windowState() & Qt::WindowMinimized))
        {
            minimized_pause = false;
            app->unpause();
        }

        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    {
        if (!mouse_grabbed)
            break;
        auto mouse_event = (QMouseEvent *)event;
        app->reportMouseButton(mouse_event->button(), event->type() == QEvent::MouseButtonPress);
        break;
    }
    case QEvent::MouseMove:
        if (mouse_grabbed)
        {
            auto center = mapToGlobal(rect().center());
            auto pos = QCursor::pos();
            auto delta = pos - center;
            if (delta.x() == 0 && delta.y() == 0)
                break;
            app->reportPointer(delta.x(), delta.y());
            QCursor::setPos(center);
        }
        if (!cursor_visible)
        {
            if (canvas && !mouse_grabbed)
                canvas->setCursor(QCursor(Qt::ArrowCursor));
            cursor_visible = true;
            mouse_timer.start();
        }
        break;
    default:
        break;
    }

    return QMainWindow::event(event);
}

void EmuMainWindow::toggleFullscreen()
{
    if (isFullScreen())
    {
        if (app->config->adjust_for_vrr)
        {
            app->config->setVRRConfig(false);
            app->updateSettings();
        }
        setBypassCompositor(false);
        showNormal();
        menuBar()->setVisible(true);
    }
    else
    {
        if (app->config->adjust_for_vrr)
        {
            app->config->setVRRConfig(true);
            app->updateSettings();
        }
        QCursor::setPos(mapToGlobal(rect().center()));
        showFullScreen();
        menuBar()->setVisible(false);
        setBypassCompositor(true);
    }
}

bool EmuMainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == canvas)
    {
        if (event->type() == QEvent::Resize)
        {
            app->emu_thread->runOnThread([&] {
                canvas->resizeEvent((QResizeEvent *)event);
            }, true);
            event->accept();
            return true;
        }
        else if (event->type() == QEvent::Paint)
        {
            app->emu_thread->runOnThread([&] {
                canvas->paintEvent((QPaintEvent *)event);
            }, true);
            event->accept();
            return true;
        }
    }

    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
        return false;

    if (watched != this && watched != canvas && !app->binding_callback)
        return false;

    auto key_event = (QKeyEvent *)event;

    if (mouse_grabbed && key_event->key() == Qt::Key_Escape && event->type() == QEvent::KeyPress)
    {
        toggleMouseGrab();
        return true;
    }

    if (isFullScreen() && key_event->key() == Qt::Key_Escape && event->type() == QEvent::KeyPress)
    {
        toggleFullscreen();
        return true;
    }

    auto binding = EmuBinding::keyboard(key_event->key(),
                                        key_event->modifiers().testFlag(Qt::ShiftModifier),
                                        key_event->modifiers().testFlag(Qt::AltModifier),
                                        key_event->modifiers().testFlag(Qt::ControlModifier),
                                        key_event->modifiers().testFlag(Qt::MetaModifier));

    if ((app->isBound(binding) || app->binding_callback) && !key_event->isAutoRepeat())
    {
        app->reportBinding(binding, event->type() == QEvent::KeyPress);
        event->accept();
        return true;
    }

    return false;
}

std::vector<std::string> EmuMainWindow::getDisplayDeviceList()
{
    if (!canvas)
        return { "Default" };
    return canvas->getDeviceList();
}

void EmuMainWindow::pauseContinue()
{
    if (manual_pause)
    {
        manual_pause = false;
        app->unpause();
    }
    else
    {
        manual_pause = true;
        app->pause();
        canvas->paintEvent(nullptr);
    }
}

bool EmuMainWindow::isActivelyDrawing()
{
    return (!app->isPaused() && app->isCoreActive());
}

void EmuMainWindow::output(uint8_t *buffer, int width, int height, QImage::Format format, int bytes_per_line, double frame_rate)
{
    if (canvas)
        canvas->output(buffer, width, height, format, bytes_per_line, frame_rate);
}

void EmuMainWindow::recreateUIAssets()
{
    app->emu_thread->runOnThread([&] {
        if (canvas)
            canvas->recreateUIAssets();
    }, true);
}

void EmuMainWindow::shaderChanged()
{
    app->emu_thread->runOnThread([&] {
        if (canvas)
            canvas->shaderChanged();
    });
    updateShaderSettingsItem();
}

void EmuMainWindow::updateShaderSettingsItem()
{
    // Shader Settings edits the parameters of a loaded shader preset, so it is
    // meaningless until a game is running (no canvas yet) with a preset
    // configured on a driver that can use one. The preset may still fail to
    // load; clicking then reports that instead of showing parameters.
    bool shader_configured = app->config->use_shader &&
                             !app->config->shader.empty() &&
                             app->config->display_driver != "qt";
    shader_settings_item->setEnabled(canvas != nullptr && shader_configured);
}

void EmuMainWindow::gameChanging()
{
    if (cheats_dialog)
        cheats_dialog->close();
}

void EmuMainWindow::toggleMouseGrab()
{
    mouse_grabbed = !mouse_grabbed;

    if (mouse_grabbed)
    {
        canvas->setCursor(QCursor(Qt::BlankCursor));
        QCursor::setPos(mapToGlobal(rect().center()));
    }
    else
    {
        canvas->setCursor(QCursor(Qt::ArrowCursor));
    }
}