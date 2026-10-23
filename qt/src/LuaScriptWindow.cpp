#include "LuaScriptWindow.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#include "EmuMainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCompleter>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStringListModel>
#include <QTabWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "lua/lua_engine.h"

// Status lines lead with a symbol; QChar keeps the source ASCII.
static QString marked(int symbol, const QString &text)
{
    return QString(QChar(symbol)) + " " + text;
}

static bool darkPalette()
{
    return QApplication::palette().color(QPalette::Base).lightness() < 128;
}

//------------------------------------------------------------------------------
// Highlighter

LuaHighlighter::LuaHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    bool dark = darkPalette();
    formats[S9X_LUA_TOKEN_KEYWORD].setForeground(dark ? QColor(0x7a, 0xa6, 0xff) : QColor(0x00, 0x33, 0xb3));
    formats[S9X_LUA_TOKEN_KEYWORD].setFontWeight(QFont::Bold);
    formats[S9X_LUA_TOKEN_BUILTIN].setForeground(dark ? QColor(0x4e, 0xd6, 0xc8) : QColor(0x00, 0x80, 0x80));
    formats[S9X_LUA_TOKEN_NUMBER].setForeground(dark ? QColor(0xf0, 0xa0, 0x50) : QColor(0xb0, 0x50, 0x00));
    formats[S9X_LUA_TOKEN_STRING].setForeground(dark ? QColor(0x98, 0xd0, 0x70) : QColor(0x1a, 0x80, 0x1a));
    formats[S9X_LUA_TOKEN_COMMENT].setForeground(dark ? QColor(0x90, 0x90, 0x90) : QColor(0x70, 0x70, 0x70));
    formats[S9X_LUA_TOKEN_COMMENT].setFontItalic(true);
}

void LuaHighlighter::highlightBlock(const QString &text)
{
    // The tokenizer speaks UTF-8 byte offsets; map them back to QChars.
    QByteArray utf8 = text.toUtf8();
    std::vector<int> to_qchar(utf8.size() + 1);
    int qchar = 0;
    for (int byte = 0; byte < utf8.size();)
    {
        unsigned char c = utf8[byte];
        int length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        for (int k = 0; k < length && byte + k < utf8.size(); k++)
            to_qchar[byte + k] = qchar;
        byte += length;
        qchar += length == 4 ? 2 : 1;
    }
    to_qchar[utf8.size()] = qchar;

    std::vector<S9xLuaToken> tokens;
    int state = std::max(previousBlockState(), 0);
    state = S9xLuaTokenize(utf8.constData(), utf8.size(), state, tokens);
    for (auto &token : tokens)
    {
        int start = to_qchar[token.start];
        int end = to_qchar[std::min<int>(token.start + token.length, utf8.size())];
        setFormat(start, end - start, formats[token.kind]);
    }
    setCurrentBlockState(state);
}

//------------------------------------------------------------------------------
// Code editor

namespace {

class LineNumberArea : public QWidget
{
  public:
    explicit LineNumberArea(LuaCodeEditor *editor)
        : QWidget(editor), editor(editor)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(editor->lineNumberAreaWidth(), 0);
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        editor->paintLineNumbers(event);
    }

  private:
    LuaCodeEditor *editor;
};

const int indent_width = 2; // the LuaRocks style guide's two spaces

QStringList completionWords()
{
    QStringList words;
    for (auto &name : S9xLuaApiNames())
        words.push_back(QString::fromStdString(name));
    return words;
}

} // namespace

LuaCodeEditor::LuaCodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(font);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * indent_width);
    new LuaHighlighter(document());

    line_numbers = new LineNumberArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            line_numbers->scroll(0, dy);
        else
            line_numbers->update(0, rect.y(), line_numbers->width(), rect.height());
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &LuaCodeEditor::updateExtraSelections);
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    updateExtraSelections();

    completer = new QCompleter(new QStringListModel(completionWords(), this), this);
    completer->setWidget(this);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseSensitive);
    completer->setModelSorting(QCompleter::CaseSensitivelySortedModel);
    connect(completer, QOverload<const QString &>::of(&QCompleter::activated), this, &LuaCodeEditor::insertCompletion);
    connect(completer, QOverload<const QString &>::of(&QCompleter::highlighted), this, [this](const QString &name) {
        if (auto doc = S9xLuaFindApiDoc(name.toStdString()))
            emit hint(QString("%1%2 — %3").arg(doc->name, doc->args, doc->summary));
    });
}

int LuaCodeEditor::lineNumberAreaWidth()
{
    int digits = QString::number(std::max(1, blockCount())).length();
    return 8 + fontMetrics().horizontalAdvance('9') * std::max(digits, 3);
}

void LuaCodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    QRect r = contentsRect();
    line_numbers->setGeometry(QRect(r.left(), r.top(), lineNumberAreaWidth(), r.height()));
}

void LuaCodeEditor::paintLineNumbers(QPaintEvent *event)
{
    QPainter painter(line_numbers);
    QColor background = palette().color(QPalette::Window);
    painter.fillRect(event->rect(), background);

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber() + 1;
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    int current = textCursor().blockNumber() + 1;

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            QColor color = palette().color(QPalette::PlaceholderText);
            if (number == error_line)
                color = QColor(0xe0, 0x40, 0x40);
            else if (number == current)
                color = palette().color(QPalette::Text);
            painter.setPen(color);
            painter.drawText(0, top, line_numbers->width() - 4, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        number++;
    }
}

void LuaCodeEditor::updateExtraSelections()
{
    QList<QTextEdit::ExtraSelection> selections;

    QTextEdit::ExtraSelection current;
    QColor line = palette().color(QPalette::Highlight);
    line.setAlpha(darkPalette() ? 40 : 25);
    current.format.setBackground(line);
    current.format.setProperty(QTextFormat::FullWidthSelection, true);
    current.cursor = textCursor();
    current.cursor.clearSelection();
    selections.append(current);

    if (error_line > 0 && error_line <= blockCount())
    {
        QTextEdit::ExtraSelection error;
        error.format.setBackground(QColor(0xe0, 0x30, 0x30, 70));
        error.format.setProperty(QTextFormat::FullWidthSelection, true);
        error.cursor = QTextCursor(document()->findBlockByNumber(error_line - 1));
        selections.append(error);
    }

    setExtraSelections(selections);
    line_numbers->update();
}

void LuaCodeEditor::setErrorLine(int line)
{
    if (error_line == line)
        return;
    error_line = line;
    updateExtraSelections();
}

void LuaCodeEditor::goToLine(int line)
{
    QTextBlock block = document()->findBlockByNumber(std::max(line, 1) - 1);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

void LuaCodeEditor::zoom(int steps)
{
    if (steps == 0)
        setPointSize(QFontDatabase::systemFont(QFontDatabase::FixedFont).pointSize());
    else
        setPointSize(std::clamp(font().pointSize() + steps, 6, 40));
}

void LuaCodeEditor::setPointSize(int size)
{
    QFont f = font();
    f.setPointSize(size);
    setFont(f);
    setTabStopDistance(QFontMetricsF(f).horizontalAdvance(' ') * indent_width);
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void LuaCodeEditor::toggleComment()
{
    QTextCursor cursor = textCursor();
    QTextBlock first = document()->findBlock(std::min(cursor.selectionStart(), cursor.selectionEnd()));
    QTextBlock last = document()->findBlock(std::max(cursor.selectionStart(), cursor.selectionEnd()));
    if (cursor.hasSelection() && last != first && cursor.selectionEnd() == last.position())
        last = last.previous();

    bool all_commented = true;
    int indent = INT_MAX;
    for (QTextBlock b = first; b.isValid(); b = b.next())
    {
        QString text = b.text();
        if (!text.trimmed().isEmpty())
        {
            all_commented = all_commented && text.trimmed().startsWith("--");
            indent = std::min<int>(indent, text.length() - QString(text).remove(QRegularExpression("^\\s+")).length());
        }
        if (b == last)
            break;
    }
    if (indent == INT_MAX)
        return;

    cursor.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next())
    {
        QString text = b.text();
        if (!text.trimmed().isEmpty())
        {
            QTextCursor c(b);
            if (all_commented)
            {
                int at = text.indexOf("--");
                int length = text.mid(at + 2, 1) == " " ? 3 : 2;
                c.setPosition(b.position() + at);
                c.setPosition(b.position() + at + length, QTextCursor::KeepAnchor);
                c.removeSelectedText();
            }
            else
            {
                c.setPosition(b.position() + indent);
                c.insertText("-- ");
            }
        }
        if (b == last)
            break;
    }
    cursor.endEditBlock();
}

void LuaCodeEditor::indentSelection(bool outdent)
{
    QTextCursor cursor = textCursor();
    QTextBlock first = document()->findBlock(cursor.selectionStart());
    QTextBlock last = document()->findBlock(cursor.selectionEnd());
    if (cursor.hasSelection() && last != first && cursor.selectionEnd() == last.position())
        last = last.previous();

    cursor.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next())
    {
        QTextCursor c(b);
        if (outdent)
        {
            int spaces = 0;
            while (spaces < indent_width && spaces < b.text().length() && b.text()[spaces] == ' ')
                spaces++;
            if (b.text().startsWith('\t'))
                spaces = 1;
            c.setPosition(b.position() + spaces, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        else if (!b.text().isEmpty())
            c.insertText(QString(indent_width, ' '));
        if (b == last)
            break;
    }
    cursor.endEditBlock();
}

QString LuaCodeEditor::wordBeforeCursor() const
{
    QTextCursor cursor = textCursor();
    QString line = cursor.block().text().left(cursor.positionInBlock());
    int start = line.length();
    while (start > 0 && (line[start - 1].isLetterOrNumber() || line[start - 1] == '_' || line[start - 1] == '.'))
        start--;
    return line.mid(start);
}

void LuaCodeEditor::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, wordBeforeCursor().length());
    cursor.insertText(completion);
    setTextCursor(cursor);
}

void LuaCodeEditor::showCallTip()
{
    QString name = wordBeforeCursor();
    auto doc = S9xLuaFindApiDoc(name.toStdString());
    if (!doc)
        return;
    QString tip = QString("<b>%1</b>%2<br>%3")
                      .arg(QString(doc->name).toHtmlEscaped(), QString(doc->args).toHtmlEscaped(),
                           QString(doc->summary).toHtmlEscaped());
    QToolTip::showText(mapToGlobal(cursorRect().bottomLeft() + QPoint(lineNumberAreaWidth(), 4)), tip, this);
    emit hint(QString("%1%2 — %3").arg(doc->name, doc->args, doc->summary));
}

void LuaCodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (completer->popup()->isVisible())
    {
        switch (event->key())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }

    bool force_completion = event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier);

    if (event->key() == Qt::Key_Tab && !(event->modifiers() & ~Qt::KeypadModifier))
    {
        if (textCursor().hasSelection())
            indentSelection(false);
        else
        {
            int column = textCursor().positionInBlock();
            insertPlainText(QString(indent_width - column % indent_width, ' '));
        }
        return;
    }
    if (event->key() == Qt::Key_Backtab)
    {
        indentSelection(true);
        return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !(event->modifiers() & ~Qt::KeypadModifier))
    {
        // Keep the line's indent, one level deeper after a block opener.
        QString line = textCursor().block().text().left(textCursor().positionInBlock());
        QString indent = line.left(line.length() - QString(line).remove(QRegularExpression("^\\s+")).length());
        QString code = line.trimmed();
        static const QRegularExpression opener(
            "(\\bthen|\\bdo|\\belse|\\brepeat|[{(]|\\bfunction\\b[^)]*\\)|^function\\b.*\\))$");
        if (opener.match(code).hasMatch() && !code.startsWith("--"))
            indent += QString(indent_width, ' ');
        textCursor().insertText("\n" + indent);
        ensureCursorVisible();
        return;
    }

    if (!force_completion)
        QPlainTextEdit::keyPressEvent(event);

    if (event->text() == "(")
    {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Left);
        QTextCursor saved = textCursor();
        setTextCursor(cursor);
        showCallTip();
        setTextCursor(saved);
    }

    QString prefix = wordBeforeCursor();
    QChar typed = event->text().isEmpty() ? QChar() : event->text().back();
    bool word_key = typed.isLetterOrNumber() || typed == '_' || typed == '.';
    if (!force_completion && (!word_key || prefix.length() < 2))
    {
        completer->popup()->hide();
        return;
    }

    if (prefix != completer->completionPrefix())
    {
        completer->setCompletionPrefix(prefix);
        completer->popup()->setCurrentIndex(completer->completionModel()->index(0, 0));
    }
    if (completer->completionCount() == 0 ||
        (completer->completionCount() == 1 && completer->currentCompletion() == prefix))
    {
        completer->popup()->hide();
        return;
    }

    QRect rect = cursorRect();
    rect.translate(lineNumberAreaWidth(), 0);
    rect.setWidth(completer->popup()->sizeHintForColumn(0) + completer->popup()->verticalScrollBar()->sizeHint().width());
    completer->complete(rect);
}

//------------------------------------------------------------------------------
// Window

LuaScriptWindow::LuaScriptWindow(EmuMainWindow *main_window_, EmuApplication *app_)
    : QMainWindow(main_window_, Qt::Window), main_window(main_window_), app(app_)
{
    setWindowTitle(tr("Lua Scripting"));
    setAcceptDrops(true);

    tabs = new QTabWidget;
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->setDocumentMode(true);
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closeDocument(documentFor(tabs->widget(index)));
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        updateActions();
        if (auto doc = currentDocument())
        {
            auto c = doc->editor->textCursor();
            position_label->setText(tr("Line %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
        }
    });

    console = new QPlainTextEdit;
    console->setReadOnly(true);
    console->setMaximumBlockCount(5000);
    console->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    console->setPlaceholderText(tr("Script output appears here. Double-click an error to jump to its line."));
    console->viewport()->installEventFilter(this);

    auto console_panel = new QWidget;
    auto console_layout = new QVBoxLayout(console_panel);
    console_layout->setContentsMargins(0, 0, 0, 0);
    console_layout->setSpacing(2);
    auto console_header = new QHBoxLayout;
    console_header->setContentsMargins(6, 2, 2, 0);
    console_header->addWidget(new QLabel(tr("Console")));
    console_header->addStretch();
    auto clear_button = new QToolButton;
    clear_button->setText(tr("Clear"));
    clear_button->setAutoRaise(true);
    connect(clear_button, &QToolButton::clicked, console, &QPlainTextEdit::clear);
    console_header->addWidget(clear_button);
    console_layout->addLayout(console_header);
    console_layout->addWidget(console);

    auto splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(tabs);
    splitter->addWidget(console_panel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    status_label = new QLabel;
    position_label = new QLabel;
    statusBar()->addWidget(status_label, 1);
    statusBar()->addPermanentWidget(position_label);

    createReference();
    createActions();

    connect(&poll_timer, &QTimer::timeout, this, &LuaScriptWindow::poll);
    poll_timer.start(40);

    auto &config = app->config;
    resize(config->lua_window_width > 0 ? config->lua_window_width : 900,
           config->lua_window_height > 0 ? config->lua_window_height : 650);
    reference_dock->setVisible(config->lua_show_reference);

    for (auto &path : config->lua_open_scripts)
        if (QFileInfo::exists(QString::fromStdString(path)))
            openScript(QString::fromStdString(path), false);

    // Scripts started elsewhere (the command line) get a tab too.
    for (auto &info : S9xLuaScripts())
    {
        if (documentForScript(info.id))
            continue;
        Document *doc = nullptr;
        if (!info.path.empty())
        {
            openScript(QString::fromStdString(info.path), false);
            doc = currentDocument();
        }
        if (doc && !doc->script)
        {
            doc->script = info.id;
            doc->paused = info.state == S9X_LUA_PAUSED;
            script_names.push_back({ info.id, QString::fromStdString(info.name) });
            updateTab(doc);
        }
    }

    if (documents.empty())
        addDocument(S9xLuaNewScriptTemplate(), QString(), QString());
    updateActions();
}

LuaScriptWindow::~LuaScriptWindow()
{
    poll_timer.stop();
}

void LuaScriptWindow::createReference()
{
    reference_dock = new QDockWidget(tr("API Reference"), this);
    reference_dock->setObjectName("lua_reference");
    auto panel = new QWidget;
    auto layout = new QVBoxLayout(panel);
    layout->setContentsMargins(4, 4, 4, 4);

    auto filter = new QLineEdit;
    filter->setPlaceholderText(tr("Search functions"));
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, this, &LuaScriptWindow::filterReference);
    layout->addWidget(filter);

    reference = new QTreeWidget;
    reference->setHeaderHidden(true);
    reference->setRootIsDecorated(true);
    reference->setWordWrap(true);
    reference->setToolTip(tr("Double-click to insert into the script"));
    layout->addWidget(reference);

    size_t count;
    const S9xLuaApiDoc *docs = S9xLuaApiDocs(&count);
    QTreeWidgetItem *module_item = nullptr;
    QString module;
    for (size_t i = 0; i < count; i++)
    {
        QString name = docs[i].name;
        QString this_module = name.section('.', 0, 0);
        if (this_module != module)
        {
            module = this_module;
            module_item = new QTreeWidgetItem(reference, QStringList(module));
            QFont bold = module_item->font(0);
            bold.setBold(true);
            module_item->setFont(0, bold);
        }
        auto item = new QTreeWidgetItem(module_item, QStringList(name.section('.', 1) + docs[i].args));
        item->setData(0, Qt::UserRole, name);
        item->setToolTip(0, QString("<b>%1%2</b><br>%3")
                                .arg(name.toHtmlEscaped(), QString(docs[i].args).toHtmlEscaped(),
                                     QString(docs[i].summary).toHtmlEscaped()));
    }

    connect(reference, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        QString name = item->data(0, Qt::UserRole).toString();
        Document *doc = currentDocument();
        if (name.isEmpty() || !doc)
            return;
        doc->editor->insertPlainText(name);
        doc->editor->setFocus();
    });
    connect(reference, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        auto doc = S9xLuaFindApiDoc(item->data(0, Qt::UserRole).toString().toStdString());
        if (doc)
            status_label->setText(QString("%1%2 — %3").arg(doc->name, doc->args, doc->summary));
    });

    reference_dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, reference_dock);
    resizeDocks({ reference_dock }, { 280 }, Qt::Horizontal);
}

void LuaScriptWindow::filterReference(const QString &text)
{
    for (int i = 0; i < reference->topLevelItemCount(); i++)
    {
        auto module = reference->topLevelItem(i);
        bool any = false;
        for (int j = 0; j < module->childCount(); j++)
        {
            auto item = module->child(j);
            auto doc = S9xLuaFindApiDoc(item->data(0, Qt::UserRole).toString().toStdString());
            bool match = text.isEmpty() || item->data(0, Qt::UserRole).toString().contains(text, Qt::CaseInsensitive) ||
                         (doc && QString(doc->summary).contains(text, Qt::CaseInsensitive));
            item->setHidden(!match);
            any = any || match;
        }
        module->setHidden(!any);
        module->setExpanded(!text.isEmpty() && any);
    }
}

void LuaScriptWindow::createActions()
{
    QString icons = app->iconPrefix();
    auto editor_action = [this](void (QPlainTextEdit::*slot)()) {
        return [this, slot] {
            if (auto doc = currentDocument())
                (doc->editor->*slot)();
        };
    };

    // File
    auto file_menu = menuBar()->addMenu(tr("&File"));
    auto new_action = file_menu->addAction(QIcon(icons + "new.svg"), tr("&New Script"), this, [this] {
        addDocument(S9xLuaNewScriptTemplate(), QString(), QString());
    });
    new_action->setShortcut(QKeySequence::New);
    auto open_action = file_menu->addAction(QIcon(icons + "open.svg"), tr("&Open..."), this, [this] {
        QString dir = QString::fromStdString(app->config->lua_last_folder);
        QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open Lua Script"), dir,
                                                          tr("Lua scripts (*.lua);;All files (*)"));
        for (auto &path : paths)
            openScript(path, false);
    });
    open_action->setShortcut(QKeySequence::Open);
    recent_menu = file_menu->addMenu(tr("Open &Recent"));
    connect(recent_menu, &QMenu::aboutToShow, this, &LuaScriptWindow::populateRecent);
    file_menu->addSeparator();
    auto save_action = file_menu->addAction(QIcon(icons + "save.svg"), tr("&Save"), this, [this] {
        if (auto doc = currentDocument())
            save(doc, false);
    });
    save_action->setShortcut(QKeySequence::Save);
    auto save_as_action = file_menu->addAction(tr("Save &As..."), this, [this] {
        if (auto doc = currentDocument())
            save(doc, true);
    });
    save_as_action->setShortcut(QKeySequence::SaveAs);
    file_menu->addSeparator();
    auto close_action = file_menu->addAction(tr("&Close Script"), this, [this] {
        if (auto doc = currentDocument())
            closeDocument(doc);
    });
    close_action->setShortcut(QKeySequence::Close);
    file_menu->addAction(tr("Close &Window"), this, &QWidget::close);

    // Edit
    auto edit_menu = menuBar()->addMenu(tr("&Edit"));
    edit_menu->addAction(tr("&Undo"), this, editor_action(&QPlainTextEdit::undo))->setShortcut(QKeySequence::Undo);
    edit_menu->addAction(tr("&Redo"), this, editor_action(&QPlainTextEdit::redo))->setShortcut(QKeySequence::Redo);
    edit_menu->addSeparator();
    edit_menu->addAction(tr("Cu&t"), this, editor_action(&QPlainTextEdit::cut))->setShortcut(QKeySequence::Cut);
    edit_menu->addAction(tr("&Copy"), this, editor_action(&QPlainTextEdit::copy))->setShortcut(QKeySequence::Copy);
    edit_menu->addAction(tr("&Paste"), this, editor_action(&QPlainTextEdit::paste))->setShortcut(QKeySequence::Paste);
    edit_menu->addAction(tr("Select &All"), this, editor_action(&QPlainTextEdit::selectAll))->setShortcut(QKeySequence::SelectAll);
    edit_menu->addSeparator();
    edit_menu->addAction(tr("Toggle &Comment"), this, [this] {
        if (auto doc = currentDocument())
            doc->editor->toggleComment();
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    edit_menu->addAction(tr("&Find..."), this, [this] {
        find_field->setFocus();
        find_field->selectAll();
    })->setShortcut(QKeySequence::Find);
    auto find_next = [this](bool backward) {
        Document *doc = currentDocument();
        if (!doc || find_field->text().isEmpty())
            return;
        auto flags = backward ? QTextDocument::FindBackward : QTextDocument::FindFlags();
        if (!doc->editor->find(find_field->text(), flags))
        {
            // Wrap around once.
            QTextCursor cursor = doc->editor->textCursor();
            cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            doc->editor->setTextCursor(cursor);
            if (!doc->editor->find(find_field->text(), flags))
                status_label->setText(tr("\"%1\" not found").arg(find_field->text()));
        }
    };
    edit_menu->addAction(tr("Find &Next"), this, [find_next] { find_next(false); })->setShortcut(QKeySequence::FindNext);
    edit_menu->addAction(tr("Find Pre&vious"), this, [find_next] { find_next(true); })->setShortcut(QKeySequence::FindPrevious);
    edit_menu->addAction(tr("&Go to Line..."), this, [this] {
        Document *doc = currentDocument();
        if (!doc)
            return;
        bool ok = false;
        int line = QInputDialog::getInt(this, tr("Go to Line"), tr("Line:"), doc->editor->textCursor().blockNumber() + 1,
                                        1, doc->editor->blockCount(), 1, &ok);
        if (ok)
            doc->editor->goToLine(line);
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));

    // Script
    auto script_menu = menuBar()->addMenu(tr("&Script"));
    run_action = script_menu->addAction(QIcon(icons + "play.svg"), tr("&Run"), this, [this] {
        if (auto doc = currentDocument())
            run(doc);
    });
    run_action->setShortcut(QKeySequence(Qt::Key_F5));
    pause_action = script_menu->addAction(QIcon(icons + "pause.svg"), tr("&Pause"), this, [this] {
        if (auto doc = currentDocument())
            togglePause(doc);
    });
    pause_action->setShortcut(QKeySequence(Qt::Key_F6));
    stop_action = script_menu->addAction(QIcon(icons + "stop.svg"), tr("&Stop"), this, [this] {
        if (auto doc = currentDocument())
            stop(doc);
    });
    stop_action->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    script_menu->addSeparator();
    stop_all_action = script_menu->addAction(tr("Stop &All Scripts"), this, &LuaScriptWindow::stopAll);

    // View
    auto view_menu = menuBar()->addMenu(tr("&View"));
    auto reference_action = reference_dock->toggleViewAction();
    reference_action->setShortcut(QKeySequence(Qt::Key_F1));
    view_menu->addAction(reference_action);
    view_menu->addAction(tr("C&lear Console"), console, &QPlainTextEdit::clear)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    view_menu->addSeparator();
    auto zoom = [this](int steps) {
        for (auto &doc : documents)
            doc->editor->zoom(steps);
        if (auto doc = currentDocument())
            app->config->lua_font_size = doc->editor->font().pointSize();
    };
    view_menu->addAction(tr("Zoom &In"), this, [zoom] { zoom(1); })->setShortcut(QKeySequence::ZoomIn);
    view_menu->addAction(tr("Zoom &Out"), this, [zoom] { zoom(-1); })->setShortcut(QKeySequence::ZoomOut);
    view_menu->addAction(tr("&Reset Zoom"), this, [this, zoom] {
        zoom(0);
        app->config->lua_font_size = 0;
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));

    // Help
    auto help_menu = menuBar()->addMenu(tr("&Help"));
    examples_menu = help_menu->addMenu(tr("&Examples"));
    populateExamples();
    help_menu->addSeparator();
    help_menu->addAction(tr("Snes9x &Scripting Guide"), this, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/shanytc/snes9x/blob/master/docs/lua-scripting.md"));
    });
    help_menu->addAction(tr("Lua 5.4 &Manual"), this, [] {
        QDesktopServices::openUrl(QUrl("https://www.lua.org/manual/5.4/"));
    });
    help_menu->addAction(tr("&Programming in Lua"), this, [] {
        QDesktopServices::openUrl(QUrl("https://www.lua.org/pil/contents.html"));
    });

    // Toolbar
    auto toolbar = addToolBar(tr("Script"));
    toolbar->setObjectName("lua_toolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(new_action);
    toolbar->addAction(open_action);
    toolbar->addAction(save_action);
    toolbar->addSeparator();
    toolbar->addAction(run_action);
    toolbar->addAction(pause_action);
    toolbar->addAction(stop_action);
    toolbar->addSeparator();
    find_field = new QLineEdit;
    find_field->setPlaceholderText(tr("Find"));
    find_field->setClearButtonEnabled(true);
    find_field->setMaximumWidth(220);
    connect(find_field, &QLineEdit::returnPressed, this, [find_next] { find_next(false); });
    auto spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addWidget(find_field);
}

void LuaScriptWindow::populateRecent()
{
    recent_menu->clear();
    auto &recent = app->config->lua_recent_scripts;
    for (size_t i = 0; i < recent.size(); i++)
    {
        QString path = QString::fromStdString(recent[i]);
        recent_menu->addAction(QString("&%1: %2").arg(i).arg(QDir::toNativeSeparators(path)), this, [this, path] {
            openScript(path, false);
        });
    }
    if (recent.empty())
        recent_menu->addAction(tr("(none)"))->setEnabled(false);
    recent_menu->addSeparator();
    recent_menu->addAction(tr("Clear Recent Scripts"), this, [this] {
        app->config->lua_recent_scripts.clear();
    });
}

void LuaScriptWindow::populateExamples()
{
    size_t count;
    const S9xLuaExample *examples = S9xLuaExamples(&count);
    for (size_t i = 0; i < count; i++)
    {
        const S9xLuaExample &example = examples[i];
        examples_menu->addAction(tr(example.title), this, [this, &example] {
            addDocument(example.code, QString(), example.filename);
        });
    }
}

void LuaScriptWindow::addRecent(const QString &path)
{
    auto &recent = app->config->lua_recent_scripts;
    std::string p = QDir::cleanPath(path).toStdString();
    recent.erase(std::remove(recent.begin(), recent.end(), p), recent.end());
    recent.insert(recent.begin(), p);
    if (recent.size() > 10)
        recent.resize(10);
    app->config->lua_last_folder = QFileInfo(path).absolutePath().toStdString();
}

LuaScriptWindow::Document *LuaScriptWindow::addDocument(const QString &text, const QString &path, const QString &name)
{
    auto doc = std::make_unique<Document>();
    doc->path = path;
    doc->untitled_name = name.isEmpty() ? QString("untitled-%1.lua").arg(++untitled_count) : name;
    doc->editor = new LuaCodeEditor;
    if (app->config->lua_font_size > 0)
        doc->editor->setPointSize(app->config->lua_font_size);
    doc->editor->setPlainText(text);
    doc->editor->document()->setModified(false);

    Document *raw = doc.get();
    connect(raw->editor->document(), &QTextDocument::modificationChanged, this, [this, raw] { updateTab(raw); });
    connect(raw->editor, &QPlainTextEdit::textChanged, this, [raw] { raw->editor->setErrorLine(0); });
    connect(raw->editor, &QPlainTextEdit::cursorPositionChanged, this, [this, raw] {
        auto c = raw->editor->textCursor();
        position_label->setText(tr("Line %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
    });
    connect(raw->editor, &LuaCodeEditor::hint, this, [this](const QString &text) { status_label->setText(text); });

    documents.push_back(std::move(doc));
    tabs->addTab(raw->editor, displayName(raw));
    tabs->setCurrentWidget(raw->editor);
    updateTab(raw);
    raw->editor->setFocus();
    return raw;
}

void LuaScriptWindow::openScript(const QString &path, bool run_it)
{
    QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (auto &doc : documents)
    {
        if (!doc->path.isEmpty() && QFileInfo(doc->path) == QFileInfo(clean))
        {
            tabs->setCurrentWidget(doc->editor);
            if (run_it)
                run(doc.get());
            return;
        }
    }

    QFile file(clean);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Open Lua Script"), tr("Couldn't open %1.").arg(QDir::toNativeSeparators(clean)));
        return;
    }
    QString text = QString::fromUtf8(file.readAll());

    // A lone untouched new script makes way for the opened one.
    if (documents.size() == 1 && documents[0]->path.isEmpty() && !documents[0]->script &&
        !documents[0]->editor->document()->isModified() &&
        documents[0]->editor->toPlainText() == S9xLuaNewScriptTemplate())
    {
        auto editor = documents[0]->editor;
        documents.clear();
        tabs->removeTab(tabs->indexOf(editor));
        editor->deleteLater();
    }

    Document *doc = addDocument(text, clean, QString());
    addRecent(clean);
    if (run_it)
        run(doc);
}

LuaScriptWindow::Document *LuaScriptWindow::currentDocument()
{
    return documentFor(tabs->currentWidget());
}

LuaScriptWindow::Document *LuaScriptWindow::documentFor(QWidget *editor)
{
    for (auto &doc : documents)
        if (doc->editor == editor)
            return doc.get();
    return nullptr;
}

LuaScriptWindow::Document *LuaScriptWindow::documentForScript(int id)
{
    if (id == 0)
        return nullptr;
    for (auto &doc : documents)
        if (doc->script == id)
            return doc.get();
    return nullptr;
}

QString LuaScriptWindow::displayName(const Document *doc) const
{
    return doc->path.isEmpty() ? doc->untitled_name : QFileInfo(doc->path).fileName();
}

void LuaScriptWindow::updateTab(Document *doc)
{
    int index = tabs->indexOf(doc->editor);
    if (index < 0)
        return;
    // A text mark rather than an icon: it shows without Qt's SVG plugin.
    QString title = displayName(doc);
    if (doc->editor->document()->isModified())
        title += "*";
    if (doc->script)
        title = QString(QChar(doc->paused ? 0x2016 : 0x25b6)) + " " + title;
    tabs->setTabText(index, title);
    tabs->setTabToolTip(index, doc->path.isEmpty() ? displayName(doc) : QDir::toNativeSeparators(doc->path));
    updateActions();
}

void LuaScriptWindow::updateActions()
{
    Document *doc = currentDocument();
    bool running = doc && doc->script;
    run_action->setEnabled(doc != nullptr);
    run_action->setText(running ? tr("&Restart") : tr("&Run"));
    run_action->setIcon(QIcon(app->iconPrefix() + (running ? "refresh.svg" : "play.svg")));
    pause_action->setEnabled(running);
    pause_action->setText(doc && doc->paused ? tr("Res&ume") : tr("&Pause"));
    stop_action->setEnabled(running);
    stop_all_action->setEnabled(!script_names.empty());

    int count = (int)script_names.size();
    if (count == 0)
        status_label->setText(tr("No scripts running"));
    else if (doc && doc->script)
        status_label->setText(doc->paused ? tr("Paused") : tr("Running") +
                              (count > 1 ? tr(" — %1 scripts running").arg(count) : QString()));
    else
        status_label->setText(count == 1 ? tr("1 script running") : tr("%1 scripts running").arg(count));
}

bool LuaScriptWindow::save(Document *doc, bool save_as)
{
    QString path = doc->path;
    if (path.isEmpty() || save_as)
    {
        QString dir = path.isEmpty() ? QString::fromStdString(app->config->lua_last_folder) + "/" + doc->untitled_name : path;
        path = QFileDialog::getSaveFileName(this, tr("Save Lua Script"), dir, tr("Lua scripts (*.lua);;All files (*)"));
        if (path.isEmpty())
            return false;
        if (QFileInfo(path).suffix().isEmpty())
            path += ".lua";
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(doc->editor->toPlainText().toUtf8()) < 0)
    {
        QMessageBox::warning(this, tr("Save Lua Script"), tr("Couldn't write %1.").arg(QDir::toNativeSeparators(path)));
        return false;
    }

    doc->path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    doc->editor->document()->setModified(false);
    addRecent(doc->path);
    updateTab(doc);
    status_label->setText(tr("Saved %1").arg(QDir::toNativeSeparators(doc->path)));
    return true;
}

bool LuaScriptWindow::closeDocument(Document *doc)
{
    if (!doc)
        return false;

    if (doc->editor->document()->isModified())
    {
        tabs->setCurrentWidget(doc->editor);
        auto answer = QMessageBox::question(this, tr("Lua Scripting"),
                                            tr("Save changes to %1?").arg(displayName(doc)),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                            QMessageBox::Save);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save(doc, false)))
            return false;
    }

    if (doc->script)
        stop(doc);

    auto editor = doc->editor;
    tabs->removeTab(tabs->indexOf(editor));
    documents.erase(std::remove_if(documents.begin(), documents.end(),
                                   [doc](const std::unique_ptr<Document> &d) { return d.get() == doc; }),
                    documents.end());
    editor->deleteLater();
    updateActions();
    return true;
}

bool LuaScriptWindow::confirmClose()
{
    for (auto &doc : documents)
    {
        if (!doc->editor->document()->isModified())
            continue;
        show();
        raise();
        tabs->setCurrentWidget(doc->editor);
        auto answer = QMessageBox::question(this, tr("Lua Scripting"),
                                            tr("Save changes to %1?").arg(displayName(doc.get())),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                            QMessageBox::Save);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save(doc.get(), false)))
            return false;
    }
    return true;
}

void LuaScriptWindow::saveSession()
{
    auto &config = app->config;
    config->lua_open_scripts.clear();
    for (int i = 0; i < tabs->count(); i++)
        if (auto doc = documentFor(tabs->widget(i)); doc && !doc->path.isEmpty())
            config->lua_open_scripts.push_back(doc->path.toStdString());
    config->lua_show_reference = reference_dock->isVisible() || (!isVisible() && config->lua_show_reference);
}

void LuaScriptWindow::run(Document *doc)
{
    if (doc->script)
        stop(doc);

    int id = S9xLuaNewScriptId();
    doc->script = id;
    doc->paused = false;
    doc->editor->setErrorLine(0);

    std::string code = doc->editor->toPlainText().toStdString();
    std::string name = displayName(doc).toStdString();
    std::string path = doc->path.toStdString();
    app->emu_thread->runOnThread([id, code, name, path] {
        S9xLuaRun(id, code, name, path);
    });
    updateTab(doc);
}

void LuaScriptWindow::stop(Document *doc)
{
    int id = doc->script;
    if (!id)
        return;
    // The request reaches a script stuck in a loop; the call ends it.
    S9xLuaRequestStop(id);
    app->emu_thread->runOnThread([id] { S9xLuaStop(id); });
    doc->script = 0;
    doc->paused = false;
    updateTab(doc);
}

void LuaScriptWindow::togglePause(Document *doc)
{
    int id = doc->script;
    if (!id)
        return;
    bool paused = !doc->paused;
    app->emu_thread->runOnThread([id, paused] { S9xLuaPause(id, paused); });
}

void LuaScriptWindow::stopAll()
{
    S9xLuaRequestStop(0);
    app->emu_thread->runOnThread([] { S9xLuaStop(0); });
    for (auto &doc : documents)
    {
        doc->script = 0;
        doc->paused = false;
        updateTab(doc.get());
    }
}

void LuaScriptWindow::appendConsole(const QString &text, const QColor &color)
{
    QTextCharFormat format;
    if (color.isValid())
        format.setForeground(color);
    QScrollBar *bar = console->verticalScrollBar();
    bool at_bottom = bar->value() >= bar->maximum() - 2;

    QTextCursor cursor(console->document());
    cursor.movePosition(QTextCursor::End);
    if (!console->document()->isEmpty())
        cursor.insertBlock();
    cursor.insertText(text, format);

    if (at_bottom)
        bar->setValue(bar->maximum());
}

void LuaScriptWindow::poll()
{
    std::vector<S9xLuaEvent> events;
    if (!S9xLuaTakeEvents(events))
        return;

    bool dark = darkPalette();
    QColor info = dark ? QColor(0x7a, 0xb8, 0xff) : QColor(0x1a, 0x5f, 0xb4);
    QColor warning = dark ? QColor(0xff, 0xc0, 0x50) : QColor(0xa0, 0x60, 0x00);
    QColor error = dark ? QColor(0xff, 0x70, 0x70) : QColor(0xc0, 0x10, 0x10);
    QColor quiet = dark ? QColor(0x90, 0x90, 0x90) : QColor(0x70, 0x70, 0x70);

    auto name_of = [this](int id) {
        for (auto &entry : script_names)
            if (entry.first == id)
                return entry.second;
        return QString();
    };

    for (auto &e : events)
    {
        Document *doc = documentForScript(e.script);
        QString text = QString::fromStdString(e.text);
        QString prefix = script_names.size() > 1 ? "[" + name_of(e.script) + "] " : QString();

        switch (e.kind)
        {
        case S9X_LUA_EVENT_STARTED:
            if (name_of(e.script).isEmpty())
                script_names.push_back({ e.script, text });
            appendConsole(marked(0x25b6, tr("%1 started").arg(text)), quiet);
            break;
        case S9X_LUA_EVENT_PRINT:
            appendConsole(prefix + text, QColor());
            break;
        case S9X_LUA_EVENT_INFO:
            appendConsole(prefix + text, info);
            break;
        case S9X_LUA_EVENT_WARNING:
            appendConsole(prefix + text, warning);
            break;
        case S9X_LUA_EVENT_ERROR:
            appendConsole(prefix + text, error);
            break;
        case S9X_LUA_EVENT_PAUSED:
        case S9X_LUA_EVENT_RESUMED:
            if (doc)
            {
                doc->paused = e.kind == S9X_LUA_EVENT_PAUSED;
                updateTab(doc);
            }
            appendConsole(e.kind == S9X_LUA_EVENT_PAUSED ? marked(0x2016, tr("%1 paused").arg(text))
                                                            : marked(0x25b6, tr("%1 resumed").arg(text)), quiet);
            break;
        case S9X_LUA_EVENT_ENDED:
        {
            QString name = name_of(e.script);
            script_names.erase(std::remove_if(script_names.begin(), script_names.end(),
                                              [&](const std::pair<int, QString> &p) { return p.first == e.script; }),
                               script_names.end());
            if (doc)
            {
                doc->script = 0;
                doc->paused = false;
                if (e.line > 0)
                    doc->editor->setErrorLine(e.line);
                updateTab(doc);
            }
            if (e.text == "failed")
                appendConsole(marked(0x25a0, tr("%1 stopped with an error").arg(name)), error);
            else if (e.text == "finished")
                appendConsole(marked(0x25a0, tr("%1 finished").arg(name)), quiet);
            else
                appendConsole(marked(0x25a0, tr("%1 stopped").arg(name)), quiet);
            break;
        }
        case S9X_LUA_EVENT_CLEAR:
            console->clear();
            break;
        }
    }
    updateActions();
}

void LuaScriptWindow::consoleDoubleClicked(const QPoint &pos)
{
    QString line = console->cursorForPosition(pos).block().text();
    static const QRegularExpression location("([^\\s:\"'\\[\\]]+\\.lua):(\\d+):");
    auto match = location.match(line);
    if (!match.hasMatch())
        return;
    QString file = match.captured(1);
    int number = match.captured(2).toInt();
    for (auto &doc : documents)
    {
        if (displayName(doc.get()) == file)
        {
            tabs->setCurrentWidget(doc->editor);
            doc->editor->goToLine(number);
            return;
        }
    }
}

bool LuaScriptWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == console->viewport() && event->type() == QEvent::MouseButtonDblClick)
    {
        consoleDoubleClicked(static_cast<QMouseEvent *>(event)->pos());
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

bool LuaScriptWindow::event(QEvent *event)
{
    // As the PPU viewers: part of the main window for "pause when unfocused".
    switch (event->type())
    {
    case QEvent::WindowActivate:
        main_window->handleFocusChange(true);
        break;
    case QEvent::WindowDeactivate:
        if (isVisible() && QApplication::activeWindow() != main_window)
            main_window->handleFocusChange(false);
        break;
    default:
        break;
    }
    return QMainWindow::event(event);
}

void LuaScriptWindow::closeEvent(QCloseEvent *event)
{
    // Closing only hides the editor; scripts keep running.
    if (!isMaximized() && !isFullScreen())
    {
        app->config->lua_window_width = width();
        app->config->lua_window_height = height();
    }
    app->config->lua_show_reference = reference_dock->isVisible();
    event->accept();
}

void LuaScriptWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void LuaScriptWindow::dropEvent(QDropEvent *event)
{
    for (auto &url : event->mimeData()->urls())
        if (url.isLocalFile())
            openScript(url.toLocalFile(), false);
    event->acceptProposedAction();
}
