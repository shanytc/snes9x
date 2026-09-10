#include <algorithm>
#include <QTimer>
#include <QMenu>
#include <QMenuBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QtEvents>
#include <QGuiApplication>
#include <QApplication>
#include <QActionGroup>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif

#include "AudioWaveformWindow.hpp"
#include "CheatsDialog.hpp"
#include "ColorCorrectionDialog.hpp"
#include "MovieDialogs.hpp"
#include "StatePreviewDialog.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include "common/desktop/xdg_app_icon.hpp"
#endif
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
#include "EmuPoTranslator.hpp"
#include "EmuSettingsWindow.hpp"
#include "memmap.h"
#include "display.h"
#include "msu1.h"
#include "voicekun.h"
#include "movie.h"
#include "snapshot.h"
#include "fscompat.h"

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

void EmuMainWindow::refreshBiosMenu()
{
    if (!bios_menu_action)
        return;

    const bool gb_loaded = (Settings.GBRomPath[0] != '\0');
    bios_menu_action->setVisible(gb_loaded);
    if (!gb_loaded)
        return;

    const bool sgb1_avail = S9xSGBBIOSAvailable(1, Settings.GBRomPath);
    const bool sgb2_avail = S9xSGBBIOSAvailable(2, Settings.GBRomPath);
    bios_sgb1_action->setEnabled(sgb1_avail);
    bios_sgb2_action->setEnabled(sgb2_avail);

    uint8_t active = 0;
    if (Settings.SGB_BIOSModeActive)
        active = (Settings.GameBoyRunMode == 2) ? 2 : 1;
    bios_none_action->setChecked(active == 0);
    bios_sgb1_action->setChecked(active == 1);
    bios_sgb2_action->setChecked(active == 2);
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

// File->Choose Icon: the four bundled logos, 1-4 like win32's Window:Icon.
static const int logo_sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
static const char *launcher_icon_name = "snes9x"; // Icon= of super-snes9x-qt.desktop

static int clampLogoIndex(int n)
{
    return (n < 1 || n > 4) ? 1 : n;
}

QIcon EmuMainWindow::logoIcon(int index)
{
    QIcon icon;
    for (int size : logo_sizes)
        icon.addFile(QString(":/icons/logos/logo%1_%2.png").arg(clampLogoIndex(index)).arg(size),
                     QSize(size, size));
    return icon;
}

void EmuMainWindow::applyWindowIcon()
{
    int index = clampLogoIndex(app->config->window_icon);
    // Icon 1 is the stock look, so an icon theme that ships its own snes9x
    // icon still wins there, as it always has.
    if (index == 1)
        setWindowIcon(QIcon::fromTheme("snes9x", QIcon(":/icons/snes9x.svg")));
    else
        setWindowIcon(logoIcon(index));
}

void EmuMainWindow::chooseWindowIcon(int index)
{
    index = clampLogoIndex(index);
    if (index == clampLogoIndex(app->config->window_icon))
        return;
    app->config->window_icon = index;
    applyWindowIcon();
    app->config->saveFile(EmuConfig::findConfigFile());
    if (app->config->write_icon_to_launcher)
        syncLauncherIcon();
}

void EmuMainWindow::setWriteIconToLauncher(bool enabled)
{
    app->config->write_icon_to_launcher = enabled;
    app->config->saveFile(EmuConfig::findConfigFile());
    syncLauncherIcon();
}

// The Linux stand-in for win32 rewriting the .exe icon: put the chosen logo
// into the user's icon theme (or take it out again when the option is off) so
// the launcher, dock and task bar follow the window.
void EmuMainWindow::syncLauncherIcon()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    // What the shipped super-snes9x-qt.desktop says, for the entry written
    // when the program is not installed.
    static const XdgAppIcon::DesktopEntry entry = {
        "super-snes9x-qt", "Super Snes9x", "A Super Nintendo emulator", "Game;Emulator;",
        "application/vnd.nintendo.snes.rom;application/x-snes-rom;application/x-gameboy-rom;"
        "application/x-gameboy-color-rom;"
    };
    std::string error;
    bool ok;
    if (!app->config->write_icon_to_launcher)
    {
        ok = XdgAppIcon::Restore(launcher_icon_name, entry, error);
    }
    else
    {
        int index = clampLogoIndex(app->config->window_icon);
        std::vector<XdgAppIcon::Image> images;
        for (int size : logo_sizes)
        {
            QFile file(QString(":/icons/logos/logo%1_%2.png").arg(index).arg(size));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QByteArray png = file.readAll();
            images.push_back({ size, std::vector<uint8_t>(png.begin(), png.end()) });
        }
        ok = XdgAppIcon::Install(launcher_icon_name, images, entry, error);
    }
    if (!ok)
        QMessageBox::warning(this, tr("Choose Icon"),
                             tr("Couldn't update the launcher icon.\n\n%1").arg(QString::fromStdString(error)));
#endif
}

void EmuMainWindow::createWidgets()
{
    setWindowTitle(QString("SuperSnes9x %1").arg(VERSION_DISPLAY));
    applyWindowIcon();

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

    // File->Save Other submenu: the dialog-less exports from the win32 File menu.
    auto save_other_menu = new QMenu(tr("Save Ot&her"), file_menu);

    auto save_spc_item = save_other_menu->addAction(tr("Save &SPC Data"));
    connect(save_spc_item, &QAction::triggered, [&] {
        app->saveSPC();
    });
    core_actions.push_back(save_spc_item);

    auto save_screenshot_item = save_other_menu->addAction(tr("Save S&creenshot"));
    connect(save_screenshot_item, &QAction::triggered, [&] {
        app->takeScreenshot();
    });
    core_actions.push_back(save_screenshot_item);

    auto save_sram_item = save_other_menu->addAction(tr("Save S-&RAM Data"));
    connect(save_sram_item, &QAction::triggered, [&] {
        app->saveSRAM();
    });
    core_actions.push_back(save_sram_item);

    auto save_mempack_item = save_other_menu->addAction(tr("Save &Memory Pack"));
    connect(save_mempack_item, &QAction::triggered, [&] {
        app->saveMemoryPack();
    });
    core_actions.push_back(save_mempack_item);
    // Only BS-X and Sufami-style multicarts carry a memory pack, so re-check
    // each time the submenu opens (the game may have changed since).
    connect(save_other_menu, &QMenu::aboutToShow, this, [this, save_mempack_item] {
        save_mempack_item->setEnabled(app->isCoreActive() && app->hasMemoryPack());
    });

    file_menu->addMenu(save_other_menu);
    file_menu->addSeparator();

    // win32's movie items: play back or record an input movie (.smv).
    auto movie_play_item = file_menu->addAction(tr("Movie &Play..."));
    connect(movie_play_item, &QAction::triggered, [&] {
        playMovieDialog();
    });
    core_actions.push_back(movie_play_item);

    auto movie_record_item = file_menu->addAction(tr("Movie &Record..."));
    connect(movie_record_item, &QAction::triggered, [&] {
        recordMovieDialog();
    });
    core_actions.push_back(movie_record_item);

    movie_stop_action = file_menu->addAction(tr("Movie &Stop"));
    connect(movie_stop_action, &QAction::triggered, [&] {
        app->stopMovie();
    });
    core_actions.push_back(movie_stop_action);

    file_menu->addSeparator();

    // One item that starts or stops, relabelled like win32's.
    avi_recording_action = file_menu->addAction(tr("Start &AVI Recording..."));
    connect(avi_recording_action, &QAction::triggered, [&] {
        toggleAVIRecording();
    });
    core_actions.push_back(avi_recording_action);

    connect(file_menu, &QMenu::aboutToShow, this, [this] {
        movie_stop_action->setEnabled(app->isCoreActive() && app->isMovieActive());
        avi_recording_action->setText(app->isAVIRecording() ? tr("Stop &AVI Recording")
                                                            : tr("Start &AVI Recording..."));
    });

    file_menu->addSeparator();

    // win32's File->Choose Icon: one of four bundled logos for the window and
    // task bar. There is no .exe icon to rewrite on Linux, so the launcher
    // item writes the logo into the user's icon theme instead.
    auto icon_menu = new QMenu(tr("Choose &Icon"), file_menu);
    auto icon_group = new QActionGroup(icon_menu);
    std::vector<QAction *> icon_actions;
    for (int i = 1; i <= 4; i++)
    {
        auto action = icon_menu->addAction(logoIcon(i), tr("Icon &%1").arg(i));
        action->setCheckable(true);
        action->setActionGroup(icon_group);
        connect(action, &QAction::triggered, this, [this, i] { chooseWindowIcon(i); });
        icon_actions.push_back(action);
    }
    QAction *launcher_action = nullptr;
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    icon_menu->addSeparator();
    launcher_action = icon_menu->addAction(tr("Apply to &Launcher Icon"));
    launcher_action->setCheckable(true);
    connect(launcher_action, &QAction::triggered, this, [this](bool checked) {
        setWriteIconToLauncher(checked);
    });
#endif
    connect(icon_menu, &QMenu::aboutToShow, this, [this, icon_actions, launcher_action] {
        icon_actions[clampLogoIndex(app->config->window_icon) - 1]->setChecked(true);
        if (launcher_action)
            launcher_action->setChecked(app->config->write_icon_to_launcher);
    });
    file_menu->addMenu(icon_menu);
    file_menu->addSeparator();

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
        app->powerCycle();
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

    bios_menu = new QMenu(tr("&BIOS"));
    auto bios_group = new QActionGroup(this);
    bios_group->setExclusive(true);
    bios_none_action = bios_menu->addAction(tr("&No BIOS"));
    bios_sgb1_action = bios_menu->addAction(tr("Super Game Boy (&SGB1)"));
    bios_sgb2_action = bios_menu->addAction(tr("Super Game Boy 2 (SGB&2)"));
    for (auto a : { bios_none_action, bios_sgb1_action, bios_sgb2_action })
    {
        a->setCheckable(true);
        bios_group->addAction(a);
    }
    auto reload_with_pref = [this](uint8_t pref) {
        Settings.SGB_BIOSPreference = pref;
        app->config->sgb_bios_preference = pref; // persist the choice to the config file
        if (Settings.GBRomPath[0])
            openFile(std::string(Settings.GBRomPath));
    };
    connect(bios_none_action, &QAction::triggered, [reload_with_pref] { reload_with_pref(0); });
    connect(bios_sgb1_action, &QAction::triggered, [reload_with_pref] { reload_with_pref(1); });
    connect(bios_sgb2_action, &QAction::triggered, [reload_with_pref] { reload_with_pref(2); });
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

    // win32's Input menu: the configuration dialogs, rumble, and the device
    // list for the two controller ports.
    auto input_menu = new QMenu(tr("&Input"));

    // The Controllers and Shortcuts settings panels (indices into the
    // Options menu's setting_panels list below).
    auto input_configuration_item = input_menu->addAction(QIcon(iconset + "joypad.svg"), tr("&Input Configuration..."));
    QObject::connect(input_configuration_item, &QAction::triggered, [&] {
        if (!g_emu_settings_window)
            g_emu_settings_window = new EmuSettingsWindow(this, app);
        g_emu_settings_window->show(4);
    });
    auto customize_hotkeys_item = input_menu->addAction(QIcon(iconset + "keyboard.svg"), tr("&Customize Hotkeys..."));
    QObject::connect(customize_hotkeys_item, &QAction::triggered, [&] {
        if (!g_emu_settings_window)
            g_emu_settings_window = new EmuSettingsWindow(this, app);
        g_emu_settings_window->show(5);
    });

    input_menu->addSeparator();

    // win32's Input->Enable Rumble (Shake): pass LRG rumble-cart motor
    // effects to the port-1 gamepad.
    auto rumble_item = input_menu->addAction(tr("Enable &Rumble (Shake)"));
    rumble_item->setCheckable(true);
    rumble_item->setChecked(app->config->enable_rumble);
    QObject::connect(rumble_item, &QAction::triggered, [&](bool checked) {
        app->config->enable_rumble = checked;
    });

    input_menu->addSeparator();

    // What is plugged into the two controller ports. Items a ROM's NSRT
    // header rules out are greyed.
    port_configuration_actions.assign(EmuConfig::eNumPortConfigurations, nullptr);
    auto add_device_item = [&](QMenu *menu, int configuration, const QString &text) {
        auto action = menu->addAction(text);
        action->setCheckable(true);
        QObject::connect(action, &QAction::triggered, [&, configuration] {
            app->setPortConfiguration(configuration);
            updatePortConfigurationMenu();
        });
        port_configuration_actions[configuration] = action;
    };

    add_device_item(input_menu, EmuConfig::eJoypads, tr("Use SNES &Joypad(s)"));
    add_device_item(input_menu, EmuConfig::eMouse, tr("Use SNES &Mouse"));

    auto superscope_menu = input_menu->addMenu(tr("Use Super &Scope"));
    add_device_item(superscope_menu, EmuConfig::eSuperScope, tr("&Enable"));
    superscope_crosshair_action = superscope_menu->addAction(tr("Show &Crosshair"));
    superscope_crosshair_action->setCheckable(true);
    QObject::connect(superscope_crosshair_action, &QAction::triggered, [&](bool checked) {
        app->setSuperScopeCrosshairVisible(checked);
    });

    add_device_item(input_menu, EmuConfig::eMultitap5, tr("Use Super Multi&tap (5-player)"));
    add_device_item(input_menu, EmuConfig::eJustifier, tr("Use Konami &Justifier"));
    add_device_item(input_menu, EmuConfig::eMouseSwapped, tr("Use Mouse in &alternate port"));
    add_device_item(input_menu, EmuConfig::eMultitap8, tr("Use Multitaps (&8-player)"));
    add_device_item(input_menu, EmuConfig::eDualJustifiers, tr("Use &Dual Justifiers"));
    add_device_item(input_menu, EmuConfig::eMacsRifle, tr("Use M.A.C.S. &Rifle"));

    QObject::connect(input_menu, &QMenu::aboutToShow, [&] {
        updatePortConfigurationMenu();
    });
    updatePortConfigurationMenu();

    menuBar()->addMenu(input_menu);

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
        app->enableAllSoundChannels();
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

    // win32's Show Audio Waveform: the track viewer of the audio rings.
    auto waveform_item = sound_menu->addAction(tr("Show Audio &Waveform"));
    waveform_item->setCheckable(true);
    connect(waveform_item, &QAction::triggered, [&] {
        toggleAudioWaveform();
    });

    sound_menu->addSeparator();

    auto sound_settings_item = sound_menu->addAction(QIcon(iconset + "sound.svg"), tr("&Settings..."));
    connect(sound_settings_item, &QAction::triggered, [&] {
        if (!g_emu_settings_window)
            g_emu_settings_window = new EmuSettingsWindow(this, app);
        g_emu_settings_window->show(2); // the Sound panel
    });

    connect(sound_menu, &QMenu::aboutToShow, this, [this, channel_actions, mute_item, waveform_item] {
        // Checkmarks show the effective state — the user mask composed with
        // any waveform-viewer solo — so soloing V3 leaves only Channel 3 checked.
        uint8_t spc_mask, gb_mask;
        audiowave::effective_masks(&spc_mask, &gb_mask);
        // Channels 1-4 drive both SPC voices 1-4 and the GB APU's CH1-CH4.
        // In BIOS-less GB mode the SPC isn't running, so 5-8 control
        // nothing — grey them there, as on win32.
        const bool gb_only = Settings.SuperGameBoy && !Settings.SGB_BIOSModeActive;
        const uint8_t low_bits = gb_only ? gb_mask : spc_mask;
        for (int i = 0; i < 8; i++)
        {
            channel_actions[i]->setChecked((i < 4 ? low_bits : spc_mask) & (1 << i));
            if (i >= 4)
                channel_actions[i]->setEnabled(!gb_only);
        }
        mute_item->setChecked(app->config->mute_audio);
        waveform_item->setChecked(audio_waveform_window != nullptr);
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

void EmuMainWindow::playMovieDialog()
{
    if (!app->isCoreActive())
        return;
#ifdef RETROACHIEVEMENTS_SUPPORT
    if (!RA_WarnDisableHardcore("Movie playback"))
        return;
#endif

    app->pause();

    PlayMovieDialog dialog(app, this);
    if (dialog.exec() && !dialog.path().empty())
    {
        app->config->movie_default_read_only = dialog.readOnly();
        int result = app->playMovie(dialog.path(), dialog.readOnly());
        if (result != SUCCESS)
            QMessageBox::warning(this, tr("Play Movie"), movieErrorString(result, false));
    }

    app->unpause();
}

void EmuMainWindow::recordMovieDialog()
{
    if (!app->isCoreActive())
        return;
#ifdef RETROACHIEVEMENTS_SUPPORT
    if (!RA_WarnDisableHardcore("Movie recording"))
        return;
#endif

    app->pause();

    // Written out first so the dialog can tell whether there is a battery
    // save that "Clear SRAM" would remove, as win32 does.
    bool sram_exists = app->movieSRAMExists();

    RecordMovieDialog dialog(app, this, sram_exists);
    if (dialog.exec() && !dialog.path().empty())
    {
        int result = app->recordMovie(dialog.path(), dialog.controllersMask(),
                                      dialog.fromReset(), dialog.clearSRAM(), dialog.metadata());
        if (result != SUCCESS)
            QMessageBox::warning(this, tr("Record Movie"), movieErrorString(result, false));
    }

    app->unpause();
}

void EmuMainWindow::toggleAVIRecording()
{
    if (!app->isCoreActive())
        return;

    if (app->isAVIRecording())
    {
        app->stopAVIRecording();
        return;
    }

    app->pause();

    QFileDialog dialog(this, tr("Record AVI"));
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("avi");
    dialog.setDirectory(QString::fromStdString(S9xGetDirectory(SCREENSHOT_DIR)));
    dialog.selectFile(QString::fromStdString(S9xBasename(S9xGetFilename(".avi", SCREENSHOT_DIR))));
    dialog.setNameFilters({ tr("AVI Files (*.avi)"), tr("All Files (*)") });

    if (dialog.exec() && !dialog.selectedFiles().empty())
    {
        std::string error;
        if (!app->startAVIRecording(dialog.selectedFiles()[0].toStdString(), error))
            QMessageBox::warning(this, tr("Record AVI"), QString::fromStdString(error));
    }

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
        handleFocusChange(true);
        break;
    case QEvent::WindowDeactivate:
        if (mouse_grabbed)
            toggleMouseGrab();
        // Focus moving to the audio waveform viewer stays within the
        // emulator. Qt has already made it the active window at this point.
        if (!(audio_waveform_window && QApplication::activeWindow() == audio_waveform_window.data()))
            handleFocusChange(false);
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
        if (!mouse_grabbed && !gunAimsAtPointer())
            break;
        auto mouse_event = (QMouseEvent *)event;
        int button = 0;
        switch (mouse_event->button())
        {
        case Qt::LeftButton:
            button = 1;
            break;
        case Qt::RightButton:
            button = 2;
            break;
        case Qt::MiddleButton:
            button = 3;
            break;
        default:
            break;
        }
        if (button)
            app->reportMouseButton(button, event->type() == QEvent::MouseButtonPress);
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
        else if (gunAimsAtPointer())
        {
            reportGunAim(((QMouseEvent *)event)->globalPosition().toPoint());
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

void EmuMainWindow::toggleAudioWaveform()
{
    if (audio_waveform_window)
    {
        audio_waveform_window->close();
        return;
    }
    audio_waveform_window = new AudioWaveformWindow(this, app);
    audio_waveform_window->show();
}

void EmuMainWindow::handleFocusChange(bool active)
{
    if (active)
    {
        if (focus_pause)
        {
            focus_pause = false;
            app->unpause();
        }
        return;
    }

    if (app->config->pause_emulation_when_unfocused && !focus_pause
#ifdef KAILLERA_SUPPORT
        && !KailleraClientIsPlaying()
#endif
    )
    {
        focus_pause = true;
        app->pause();
    }
}

bool EmuMainWindow::gunAimsAtPointer()
{
    return canvas && app->isCoreActive() &&
           EmuConfig::portConfigurationUsesGun(app->config->port_configuration);
}

void EmuMainWindow::reportGunAim(const QPoint &global_pos)
{
    // Map the pointer through the letterboxed image onto the SNES screen,
    // the way the GTK port aims an ungrabbed gun. Outside the image counts
    // as off-screen, which the core reads as such.
    auto ratio = canvas->devicePixelRatio();
    auto image = canvas->applyAspect(QRect(0, 0, canvas->width() * ratio, canvas->height() * ratio));
    if (image.width() <= 0 || image.height() <= 0)
        return;

    auto pos = canvas->mapFromGlobal(global_pos) * ratio;
    int height = app->config->show_overscan ? 239 : 224;
    int x = (pos.x() - image.x()) * 256 / image.width();
    int y = (pos.y() - image.y()) * height / image.height();
    app->reportPointerAbsolute(x, y);
}

void EmuMainWindow::updatePortConfigurationMenu()
{
    for (int i = 0; i < (int)port_configuration_actions.size(); i++)
    {
        auto action = port_configuration_actions[i];
        if (!action)
            continue;
        action->setChecked(app->config->port_configuration == i);
        action->setEnabled(app->isPortConfigurationValid(i));
    }

    if (superscope_crosshair_action)
    {
        superscope_crosshair_action->setChecked(app->config->superscope_crosshair_visible);
        superscope_crosshair_action->setEnabled(app->config->port_configuration == EmuConfig::eSuperScope);
    }
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