#pragma once
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QTimer>
#include <memory>
#include <vector>

#include "lua/lua_engine.h"

class EmuApplication;
class EmuMainWindow;
class QCompleter;
class QLabel;
class QLineEdit;
class QMenu;
class QSplitter;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QDockWidget;

/* File > Script > Lua: an editor for Lua scripts that also runs them. The
 * engine lives in the core (lua/); this window talks to it on the emulation
 * thread and polls its event queue for output and script state. */

class LuaHighlighter : public QSyntaxHighlighter
{
  public:
    explicit LuaHighlighter(QTextDocument *document);

  protected:
    void highlightBlock(const QString &text) override;

  private:
    QTextCharFormat formats[6];
};

class LuaCodeEditor : public QPlainTextEdit
{
    Q_OBJECT

  public:
    // A breakpoint rides a cursor at its line's start, which follows the text as it's edited.
    struct Breakpoint
    {
        QTextCursor anchor;
        bool enabled = true;
        QString condition, hit, log;
    };

    explicit LuaCodeEditor(QWidget *parent = nullptr);
    void setErrorLine(int line); // 0 clears it
    void setDebugLines(int stopped, int frame); // the debugger's stop, the selected frame; 0 clears
    void goToLine(int line);
    void showLine(int line); // scrolls it into view, leaving the cursor where it is
    void toggleComment();
    void zoom(int steps); // 0 resets to the system font
    void setPointSize(int size);
    int lineNumberAreaWidth();
    void paintLineNumbers(QPaintEvent *event);
    void lineNumberAreaClicked(QMouseEvent *event);
    void lineNumberAreaMenu(QContextMenuEvent *event);

    std::vector<Breakpoint> breakpoints;
    int breakpointLine(const Breakpoint &b) const { return b.anchor.blockNumber() + 1; }
    Breakpoint *breakpointAt(int line);
    void toggleBreakpoint(int line);
    void mergeBreakpoints(); // edits can fold two onto one line: keep the first
    void refreshMarkers(); // after a breakpoint changed from outside

  Q_SIGNALS:
    void hint(const QString &text);
    void breakpointsChanged();
    void breakpointMenu(int line, const QPoint &global);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void updateExtraSelections();
    void insertCompletion(const QString &completion);
    QString wordBeforeCursor() const; // identifier, dots included
    void showCallTip();
    void indentSelection(bool outdent);
    int markerWidth() const;
    int lineAt(int y);

    QWidget *line_numbers;
    QCompleter *completer;
    int error_line = 0;
    int stopped_line = 0, frame_line = 0;
};

class LuaScriptWindow : public QMainWindow
{
    Q_OBJECT

  public:
    LuaScriptWindow(EmuMainWindow *main_window, EmuApplication *app);
    ~LuaScriptWindow() override;

    // Opens (or switches to) a script file; run starts it.
    void openScript(const QString &path, bool run);
    // Asks about unsaved scripts; false when the user cancelled.
    bool confirmClose();
    // Remembers the open files for next time.
    void saveSession();

    /* The debugger. A stopped script holds the emulation thread inside its
     * hook; these run on the UI thread, queued from the engine's callback. */
    void debuggerStopped(int script);
    void debuggerResumed();
    // Before the emulator closes: no more stops, and a stopped script goes on.
    void releaseDebugger();

  protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct Document
    {
        LuaCodeEditor *editor = nullptr;
        QString path;
        QString untitled_name;
        int script = 0; // engine id while it runs
        bool paused = false;
    };

    void createActions();
    void createReference();
    Document *addDocument(const QString &text, const QString &path, const QString &name);
    Document *currentDocument();
    Document *documentFor(QWidget *editor);
    Document *documentForScript(int id);
    QString displayName(const Document *doc) const;
    void updateTab(Document *doc);
    void updateActions();
    bool save(Document *doc, bool save_as);
    bool closeDocument(Document *doc);
    void addRecent(const QString &path);
    void populateRecent();
    void populateExamples();

    void run(Document *doc);
    void stop(Document *doc);
    void togglePause(Document *doc);
    void stopAll();
    void poll();
    void appendConsole(const QString &text, const QColor &color);
    void consoleDoubleClicked(const QPoint &pos);
    void filterReference(const QString &text);

    // Debugger
    void createDebugPanels();
    void debugCommand(S9xLuaDebugAction action);
    void pollDebug();
    void onStopped(const S9xLuaDebugEvent &e);
    void onVariables(const S9xLuaDebugEvent &e);
    void onEvaluated(const S9xLuaDebugEvent &e);
    void clearDebugViews();
    void selectFrame(int index);
    Document *frameDocument(const S9xLuaDebugFrame &f);
    void fillVariables();
    void fillWatches();
    void fillCallStack();
    void clearTree(QTreeWidget *tree);
    void requestChildren(QTreeWidgetItem *item);
    void addWatch(const QString &expression);
    void sendBreakpoints(Document *doc);
    void breakpointsChanged(Document *doc);
    void refreshBreakpointList();
    LuaCodeEditor::Breakpoint *breakpointForRow(int row, Document **owner);
    LuaCodeEditor::Breakpoint *selectedBreakpoint(Document **owner);
    void fillBreakpointFields();
    void applyBreakpointFields();
    void editBreakpoint(Document *doc, int line);
    void removeAllBreakpoints();
    void showBreakpointMenu(Document *doc, int line, const QPoint &global);

    EmuMainWindow *main_window;
    EmuApplication *app;
    QTabWidget *tabs;
    QSplitter *splitter;
    QTabWidget *panel_tabs;
    QPlainTextEdit *console;
    QTreeWidget *variables = nullptr, *watch_tree = nullptr, *callstack = nullptr, *breakpoint_list = nullptr;
    QLineEdit *watch_edit = nullptr, *bp_condition = nullptr, *bp_hit = nullptr, *bp_log = nullptr;
    QDockWidget *reference_dock;
    QTreeWidget *reference;
    QLineEdit *find_field;
    QLabel *status_label;
    QLabel *position_label;
    QMenu *recent_menu;
    QMenu *examples_menu;
    QAction *run_action, *pause_action, *stop_action, *stop_all_action;
    QAction *continue_action, *break_action, *step_over_action, *step_into_action, *step_out_action;
    QAction *break_on_error_action;
    QTimer poll_timer;
    std::vector<std::unique_ptr<Document>> documents;
    std::vector<std::pair<int, QString>> script_names;
    int untitled_count = 0;

    int stopped = 0; // the script stopped in the debugger
    QString stop_text;
    std::vector<S9xLuaDebugFrame> frames;
    int frame = 0; // the selected one
    std::vector<std::pair<int, QTreeWidgetItem *>> pending; // variable requests, answered by ref
    QStringList watches;
    int watch_generation = 0;
    std::vector<std::pair<Document *, size_t>> bp_rows; // the breakpoints list
    bool filling_breakpoints = false;
    bool holding = false; // other windows are off while a script is stopped
    std::vector<QPointer<QWidget>> held_windows;
    bool released = false; // the emulator is closing: no more stops
};
