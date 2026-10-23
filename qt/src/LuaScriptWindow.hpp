#pragma once
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTimer>
#include <memory>
#include <vector>

class EmuApplication;
class EmuMainWindow;
class QCompleter;
class QLabel;
class QLineEdit;
class QMenu;
class QTabWidget;
class QTreeWidget;
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
    explicit LuaCodeEditor(QWidget *parent = nullptr);
    void setErrorLine(int line); // 0 clears it
    void goToLine(int line);
    void toggleComment();
    void zoom(int steps); // 0 resets to the system font
    void setPointSize(int size);
    int lineNumberAreaWidth();
    void paintLineNumbers(QPaintEvent *event);

  Q_SIGNALS:
    void hint(const QString &text);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void updateExtraSelections();
    void insertCompletion(const QString &completion);
    QString wordBeforeCursor() const; // identifier, dots included
    void showCallTip();
    void indentSelection(bool outdent);

    QWidget *line_numbers;
    QCompleter *completer;
    int error_line = 0;
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

    EmuMainWindow *main_window;
    EmuApplication *app;
    QTabWidget *tabs;
    QPlainTextEdit *console;
    QDockWidget *reference_dock;
    QTreeWidget *reference;
    QLineEdit *find_field;
    QLabel *status_label;
    QLabel *position_label;
    QMenu *recent_menu;
    QMenu *examples_menu;
    QAction *run_action, *pause_action, *stop_action, *stop_all_action;
    QTimer poll_timer;
    std::vector<std::unique_ptr<Document>> documents;
    std::vector<std::pair<int, QString>> script_names;
    int untitled_count = 0;
};
