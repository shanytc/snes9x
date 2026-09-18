#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#include "EmuMainWindow.hpp"
#include "EmuPoTranslator.hpp"
#include "SDLInputManager.hpp"
#include "display.h"

#ifdef RETROACHIEVEMENTS_SUPPORT
#include "RAIntegrationQt.hpp"
#include "retroachievements.h"
#endif

#include <clocale>
#include <cstring>
#include <qnamespace.h>
#include <QStyle>
#include <QStyleHints>

// Qt 6.2.3 through 6.3.1 drop QPalette::setBrush()'s resolve bit when the
// brush is unchanged (qtbase 56bd1b76d2, undone by 9334e06bdf for 6.3.2 and
// 6.4: QTBUG-98762). The built-in KDE platform theme fills a default
// black-and-white QPalette from kdeglobals, so any colour that is exactly
// black or white stays "unset" and QApplication takes it from Fusion's light
// palette instead: dark colour schemes with pure white text (Nobara's, for
// one) come up with black menubar text on a black bar. The Ubuntu 22.04
// AppImage ships Qt 6.2.4. Reading the theme palette back is QPA API.
#if !defined(_WIN32) && !defined(__APPLE__) && QT_VERSION < QT_VERSION_CHECK(6, 3, 2)
#define REAPPLY_THEME_PALETTE
#include <qpa/qplatformtheme.h>
#include <private/qguiapplication_p.h>
#endif

#ifndef _WIN32
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <QSocketNotifier>
#endif

#ifdef REAPPLY_THEME_PALETTE
// Copy the theme's colours over the resolved application palette. setBrush()
// only flags the roles that differ, so the roles the theme got through stay
// as they were and a healthy palette is left alone.
static void reapplyPlatformThemePalette()
{
    const QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme();
    const QPalette *themePalette = theme ? theme->palette() : nullptr;
    if (!themePalette)
        return;

    // The roles QKdeTheme fills in from kdeglobals.
    static const QPalette::ColorRole roles[] = {
        QPalette::Window, QPalette::WindowText, QPalette::Base, QPalette::AlternateBase,
        QPalette::Text, QPalette::Button, QPalette::ButtonText, QPalette::Highlight,
        QPalette::HighlightedText, QPalette::Link, QPalette::LinkVisited,
        QPalette::ToolTipBase, QPalette::ToolTipText
    };
    static const QPalette::ColorGroup groups[] = {
        QPalette::Active, QPalette::Inactive, QPalette::Disabled
    };

    QPalette palette = QApplication::palette();
    for (auto role : roles)
        for (auto group : groups)
            palette.setBrush(group, role, themePalette->brush(group, role));

    if (palette != QApplication::palette())
        QApplication::setPalette(palette);
}
#endif

#ifndef _WIN32
// QApplication::quit() is not async-signal-safe, and a quit() issued while
// exec() is not yet running is dropped outright. Startup pumps a nested event
// loop, so a queued quit can land in that window too and be lost the same way:
// SIGTERM 0.3s after launch left the window up and deaf to the signal every
// time, and only a second one, once exec() was running, got through.
//
// The handler does nothing but write the signal number to a pipe, which is
// async-signal-safe. The QSocketNotifier that drains it is not created until
// enableQuitNotifier(), immediately before exec(), so it cannot fire into the
// window where a quit would be dropped: a signal taken during startup waits in
// the pipe until the event loop is up to act on it. Letting startup finish also
// keeps teardown running against a fully built emulator -- skipping exec()
// outright instead crashed in stopThread().
static int quit_pipe[2] = { -1, -1 };

static void quitSignalHandler(int sig)
{
    int saved_errno = errno;
    unsigned char num = (unsigned char)sig;
    // write() is async-signal-safe; a full pipe just means a quit is queued.
    ssize_t written = write(quit_pipe[1], &num, 1);
    (void)written;
    errno = saved_errno;
}

static void installQuitSignalHandlers()
{
    if (pipe(quit_pipe) != 0)
        return;

    for (int fd : quit_pipe)
    {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }

    struct sigaction sig_callback{};
    sig_callback.sa_handler = quitSignalHandler;
    sigemptyset(&sig_callback.sa_mask);
    sig_callback.sa_flags = SA_RESTART;
    for (int s : { SIGQUIT, SIGINT, SIGTERM, SIGHUP })
        sigaction(s, &sig_callback, nullptr);
}

static void enableQuitNotifier()
{
    if (quit_pipe[0] == -1)
        return;

    auto *notifier = new QSocketNotifier(quit_pipe[0], QSocketNotifier::Read, qApp);
    QObject::connect(notifier, &QSocketNotifier::activated, qApp, [] {
        unsigned char num;
        while (read(quit_pipe[0], &num, 1) == 1)
            ;
        QApplication::quit();
    });
}
#endif

#ifdef _WIN32
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, const char *lpCmdLine, int nShowCmd)
{
    int argc = 0;
    LPWSTR *argvw = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Convert wide args to UTF-8
    char **argv = new char *[argc];
    for (int i = 0; i < argc; i++)
    {
        int size = WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, nullptr, 0, nullptr, nullptr);
        argv[i] = new char[size];
        WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, argv[i], size, nullptr, nullptr);
    }
    LocalFree(argvw);

    setlocale(LC_ALL, ".utf8");
#else
int main(int argc, char *argv[])
{
#endif
    EmuApplication emu;
    emu.qtapp = std::make_unique<QApplication>(argc, argv);

    // Upstream reads --dark off a QCommandLineParser; this port hands the
    // command line to the core's own S9xParseArgs, so scan for it here.
    bool force_dark = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--dark") || !strcmp(argv[i], "-dark"))
            force_dark = true;

    QGuiApplication::setDesktopFileName("super-snes9x-qt");

    if (QApplication::platformName() == "windows" || force_dark)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark || force_dark)
        {
#else
        if (force_dark) {
#endif
            QApplication::setStyle("fusion");

            const QColor darkGray(53, 53, 53);
            const QColor gray(128, 128, 128);
            const QColor black(25, 25, 25);
            const QColor blue(198, 238, 255);
            const QColor blue2(0, 88, 208);

            QPalette darkPalette;
            darkPalette.setColor(QPalette::Window, darkGray);
            darkPalette.setColor(QPalette::WindowText, Qt::white);
            darkPalette.setColor(QPalette::Base, black);
            darkPalette.setColor(QPalette::AlternateBase, darkGray);
            darkPalette.setColor(QPalette::ToolTipBase, blue2);
            darkPalette.setColor(QPalette::ToolTipText, Qt::white);
            darkPalette.setColor(QPalette::Text, Qt::white);
            darkPalette.setColor(QPalette::Button, darkGray);
            darkPalette.setColor(QPalette::ButtonText, Qt::white);
            darkPalette.setColor(QPalette::Link, blue);
            darkPalette.setColor(QPalette::Highlight, blue2);
            darkPalette.setColor(QPalette::HighlightedText, Qt::white);
            darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

            darkPalette.setColor(QPalette::Active, QPalette::Button, darkGray);
            darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
            darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
            darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
            darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);
            QApplication::setPalette(darkPalette);
        }
        else
        {
            QApplication::setStyle("windowsvista");
        }
    }
#ifdef REAPPLY_THEME_PALETTE
    reapplyPlatformThemePalette();
#endif

#ifndef _WIN32
    installQuitSignalHandlers();
#endif

    emu.startThread();

    emu.config = std::make_unique<EmuConfig>();
    emu.config->setDefaults();
    emu.config->loadFile(EmuConfig::findConfigFile());

    EmuPoTranslator translator;
    if (translator.loadLanguage(QString::fromStdString(emu.config->language)))
        emu.qtapp->installTranslator(&translator);

    emu.input_manager = std::make_unique<SDLInputManager>();
    emu.window = std::make_unique<EmuMainWindow>(emu);
    emu.window->show();

    emu.updateBindings();
    emu.startInputTimer();

    char *rom_filename = S9xParseArgs(argv, argc);
    if (rom_filename)
        emu.window->openFile(rom_filename);

#ifdef _WIN32
    for (int i = 0; i < argc; i++)
        delete[] argv[i];
    delete[] argv;
#endif

#ifdef RETROACHIEVEMENTS_SUPPORT
    if (emu.config->ra_enabled)
    {
        RA_Qt_RegisterCallbacks(&emu);
        RA_Init();
        RA_SetEnabled(true);
        RA_SetHardcoreEnabled(emu.config->ra_hardcore_mode);
        RA_AttemptLogin(emu.config->ra_username.c_str(), emu.config->ra_api_token.c_str());
    }
#endif

#ifndef _WIN32
    // Not at install time: startup pumps a nested event loop, and a quit
    // dispatched into that window would be dropped before exec() could take it.
    enableQuitNotifier();
#endif

    emu.qtapp->exec();

    emu.stopThread();
    emu.config->saveFile(EmuConfig::findConfigFile());

    return 0;
}
