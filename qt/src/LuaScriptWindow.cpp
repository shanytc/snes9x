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
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
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

#include "common/audio/s9x_sound_driver.hpp"
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

    // A click sets or clears a breakpoint; the context menu has the rest.
    void mousePressEvent(QMouseEvent *event) override
    {
        editor->lineNumberAreaClicked(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editor->lineNumberAreaMenu(event);
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

int LuaCodeEditor::markerWidth() const
{
    return fontMetrics().height() + 2;
}

int LuaCodeEditor::lineNumberAreaWidth()
{
    int digits = QString::number(std::max(1, blockCount())).length();
    return markerWidth() + 8 + fontMetrics().horizontalAdvance('9') * std::max(digits, 3);
}

// The line under a y in the line number area, 1-based; 0 below the last.
int LuaCodeEditor::lineAt(int y)
{
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid())
    {
        int bottom = top + qRound(blockBoundingRect(block).height());
        if (y >= top && y < bottom)
            return block.blockNumber() + 1;
        block = block.next();
        top = bottom;
    }
    return 0;
}

LuaCodeEditor::Breakpoint *LuaCodeEditor::breakpointAt(int line)
{
    for (auto &b : breakpoints)
        if (breakpointLine(b) == line)
            return &b;
    return nullptr;
}

void LuaCodeEditor::toggleBreakpoint(int line)
{
    for (size_t i = 0; i < breakpoints.size(); i++)
    {
        if (breakpointLine(breakpoints[i]) == line)
        {
            breakpoints.erase(breakpoints.begin() + i);
            line_numbers->update();
            emit breakpointsChanged();
            return;
        }
    }
    QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    Breakpoint b;
    b.anchor = QTextCursor(block);
    breakpoints.push_back(b);
    line_numbers->update();
    emit breakpointsChanged();
}

void LuaCodeEditor::mergeBreakpoints()
{
    std::vector<int> lines;
    for (size_t i = 0; i < breakpoints.size();)
    {
        int line = breakpointLine(breakpoints[i]);
        if (std::find(lines.begin(), lines.end(), line) != lines.end())
        {
            breakpoints.erase(breakpoints.begin() + i);
            continue;
        }
        lines.push_back(line);
        i++;
    }
}

void LuaCodeEditor::lineNumberAreaClicked(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (int line = lineAt(event->pos().y()))
        toggleBreakpoint(line);
}

void LuaCodeEditor::lineNumberAreaMenu(QContextMenuEvent *event)
{
    if (int line = lineAt(event->pos().y()))
        emit breakpointMenu(line, event->globalPos());
}

void LuaCodeEditor::refreshMarkers()
{
    line_numbers->update();
}

void LuaCodeEditor::setDebugLines(int stopped, int frame)
{
    if (stopped_line == stopped && frame_line == frame)
        return;
    stopped_line = stopped;
    frame_line = frame;
    updateExtraSelections();
}

void LuaCodeEditor::showLine(int line)
{
    QScrollBar *bar = verticalScrollBar();
    int visible = std::max(1, viewport()->height() / std::max(1, fontMetrics().height()));
    int target = line - 1;
    if (target < bar->value() || target >= bar->value() + visible - 1)
        bar->setValue(target - visible / 3);
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
    int size = fontMetrics().height() - 4;
    painter.setRenderHint(QPainter::Antialiasing);

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            QRectF box(3, top + (fontMetrics().height() - size) / 2.0, size, size);
            if (const Breakpoint *b = breakpointAt(number))
            {
                // Red, orange with a condition or hit count, a blue diamond for a log message.
                QColor color = !b->log.isEmpty() ? QColor(0x24, 0x70, 0xc8)
                             : (!b->condition.isEmpty() || !b->hit.isEmpty()) ? QColor(0xe2, 0x84, 0x00)
                             : QColor(0xc8, 0x24, 0x24);
                painter.setPen(QPen(color, 1.5));
                painter.setBrush(b->enabled ? QBrush(color) : Qt::NoBrush);
                if (!b->log.isEmpty())
                {
                    QPointF c = box.center();
                    QPointF points[4] = { { c.x(), box.top() }, { box.right(), c.y() }, { c.x(), box.bottom() }, { box.left(), c.y() } };
                    painter.drawPolygon(points, 4);
                }
                else
                    painter.drawEllipse(box);
            }
            if (number == stopped_line || number == frame_line)
            {
                // An arrow: yellow where the script stopped, green for the selected frame.
                qreal cy = box.center().y(), mid = box.left() + box.width() / 2, h = box.height();
                QPointF arrow[7] = { { box.left(), cy - h / 5 }, { mid, cy - h / 5 }, { mid, box.top() }, { box.right(), cy },
                                     { mid, box.bottom() }, { mid, cy + h / 5 }, { box.left(), cy + h / 5 } };
                painter.setPen(QPen(QColor(0x60, 0x50, 0x00), 1));
                painter.setBrush(number == stopped_line ? QColor(0xff, 0xd6, 0x00) : QColor(0x60, 0xb8, 0x60));
                painter.drawPolygon(arrow, 7);
            }

            QColor color = palette().color(QPalette::PlaceholderText);
            if (number == error_line)
                color = QColor(0xe0, 0x40, 0x40);
            else if (number == current)
                color = palette().color(QPalette::Text);
            painter.setPen(color);
            painter.drawText(markerWidth(), top, line_numbers->width() - markerWidth() - 4, fontMetrics().height(),
                             Qt::AlignRight, QString::number(number));
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

    auto whole_line = [&](int line, const QColor &color) {
        if (line <= 0 || line > blockCount())
            return;
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(color);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = QTextCursor(document()->findBlockByNumber(line - 1));
        selections.append(selection);
    };
    whole_line(error_line, QColor(0xe0, 0x30, 0x30, 70));
    whole_line(frame_line, QColor(0x60, 0xc0, 0x60, 70));
    whole_line(stopped_line, QColor(0xff, 0xd8, 0x30, 110));

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
    console_header->addStretch();
    auto clear_button = new QToolButton;
    clear_button->setText(tr("Clear"));
    clear_button->setAutoRaise(true);
    connect(clear_button, &QToolButton::clicked, console, &QPlainTextEdit::clear);
    console_header->addWidget(clear_button);
    console_layout->addLayout(console_header);
    console_layout->addWidget(console);

    // Under the editor: the console and the debugger's panels.
    panel_tabs = new QTabWidget;
    panel_tabs->setDocumentMode(true);
    panel_tabs->addTab(console_panel, tr("Console"));
    createDebugPanels();

    splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(tabs);
    splitter->addWidget(panel_tabs);
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
    if (config->lua_panel_height > 0)
        splitter->setSizes({ std::max(200, height() - config->lua_panel_height), config->lua_panel_height });

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
        // While a script is stopped in the debugger, F5 lets it go on.
        if (stopped)
            debugCommand(S9X_LUA_DEBUG_CONTINUE);
        else if (auto doc = currentDocument())
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

    // Debug
    auto debug_menu = menuBar()->addMenu(tr("&Debug"));
    continue_action = debug_menu->addAction(QIcon(icons + "play.svg"), tr("&Continue\tF5"), this, [this] {
        debugCommand(S9X_LUA_DEBUG_CONTINUE);
    });
    break_action = debug_menu->addAction(tr("&Break"), this, [this] {
        Document *doc = currentDocument();
        if (stopped || !doc || !doc->script)
            return;
        S9xLuaDebugCommand(doc->script, S9X_LUA_DEBUG_BREAK);
        status_label->setText(tr("Stopping at the script's next line"));
    });
    continue_action->setIconText(tr("Continue"));
    break_action->setShortcut(QKeySequence(Qt::Key_F8));
    step_over_action = debug_menu->addAction(tr("Step &Over"), this, [this] { debugCommand(S9X_LUA_DEBUG_STEP_OVER); });
    step_over_action->setShortcut(QKeySequence(Qt::Key_F10));
    step_into_action = debug_menu->addAction(tr("Step &Into"), this, [this] { debugCommand(S9X_LUA_DEBUG_STEP_INTO); });
    step_into_action->setShortcut(QKeySequence(Qt::Key_F11));
    step_out_action = debug_menu->addAction(tr("Step O&ut"), this, [this] { debugCommand(S9X_LUA_DEBUG_STEP_OUT); });
    step_out_action->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F11));
    debug_menu->addSeparator();
    debug_menu->addAction(tr("&Toggle Breakpoint"), this, [this] {
        if (auto doc = currentDocument())
            doc->editor->toggleBreakpoint(doc->editor->textCursor().blockNumber() + 1);
    })->setShortcut(QKeySequence(Qt::Key_F9));
    debug_menu->addAction(tr("&Edit Breakpoint..."), this, [this] {
        if (auto doc = currentDocument())
            editBreakpoint(doc, doc->editor->textCursor().blockNumber() + 1);
    });
    debug_menu->addAction(tr("&Remove All Breakpoints"), this, &LuaScriptWindow::removeAllBreakpoints)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F9));
    debug_menu->addAction(tr("Add &Watch"), this, [this] {
        Document *doc = currentDocument();
        if (!doc)
            return;
        // The selection, or the name under the cursor.
        QTextCursor cursor = doc->editor->textCursor();
        QString text = cursor.selectedText().trimmed();
        if (text.isEmpty())
        {
            QString line = cursor.block().text();
            int first = cursor.positionInBlock(), last = first;
            auto ident = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '.'; };
            while (first > 0 && ident(line[first - 1]))
                first--;
            while (last < line.length() && ident(line[last]))
                last++;
            text = line.mid(first, last - first);
        }
        addWatch(text);
        panel_tabs->setCurrentWidget(watch_tree->parentWidget());
    })->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F9));
    debug_menu->addSeparator();
    break_on_error_action = debug_menu->addAction(tr("Break on &Errors"), this, [this](bool on) {
        app->config->lua_break_on_error = on;
        S9xLuaDebugSetBreakOnError(on);
    });
    break_on_error_action->setCheckable(true);
    break_on_error_action->setChecked(app->config->lua_break_on_error);

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
    toolbar->addAction(continue_action);
    toolbar->addAction(step_over_action);
    toolbar->addAction(step_into_action);
    toolbar->addAction(step_out_action);
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
    connect(raw->editor, &LuaCodeEditor::breakpointsChanged, this, [this, raw] { breakpointsChanged(raw); });
    connect(raw->editor, &LuaCodeEditor::breakpointMenu, this, [this, raw](int line, const QPoint &global) {
        showBreakpointMenu(raw, line, global);
    });
    connect(raw->editor, &QPlainTextEdit::textChanged, this, [this, raw] {
        if (raw->editor->breakpoints.empty())
            return;
        raw->editor->mergeBreakpoints();
        refreshBreakpointList();
    });

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
        refreshBreakpointList();
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
    for (QAction *action : { continue_action, step_over_action, step_into_action, step_out_action })
        action->setEnabled(stopped != 0);
    break_action->setEnabled(!stopped && running);

    int count = (int)script_names.size();
    if (stopped)
        status_label->setText(stop_text);
    else if (count == 0)
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
    refreshBreakpointList();
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
    config->lua_panel_height = splitter->sizes().value(1);
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
    sendBreakpoints(doc); // they wait for the script by its id
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
    pollDebug();
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
    // Closing only hides the editor; scripts keep running, a stopped one too.
    debugCommand(S9X_LUA_DEBUG_CONTINUE);
    app->config->lua_panel_height = splitter->sizes().value(1);
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

//------------------------------------------------------------------------------
// Debugger

namespace {

using PendingList = std::vector<std::pair<int, QTreeWidgetItem *>>;

// A variable row; a ref means it has fields, fetched when it's expanded.
QTreeWidgetItem *variableItem(const QString &text, int ref)
{
    auto item = new QTreeWidgetItem(QStringList(text));
    item->setData(0, Qt::UserRole, ref);
    item->setToolTip(0, text);
    item->setChildIndicatorPolicy(ref ? QTreeWidgetItem::ShowIndicator : QTreeWidgetItem::DontShowIndicator);
    return item;
}

bool within(QTreeWidgetItem *item, QTreeWidgetItem *ancestor)
{
    for (; item; item = item->parent())
        if (item == ancestor)
            return true;
    return false;
}

// Deletes an item's children, dropping the requests that would fill them.
void deleteChildren(PendingList &pending, QTreeWidgetItem *item)
{
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [item](const std::pair<int, QTreeWidgetItem *> &p) {
                                     return p.second != item && within(p.second, item);
                                 }),
                  pending.end());
    qDeleteAll(item->takeChildren());
}

// Where the script is: the innermost frame with a line.
int topFrame(const std::vector<S9xLuaDebugFrame> &frames)
{
    for (size_t i = 0; i < frames.size(); i++)
        if (frames[i].line > 0)
            return (int)i;
    return 0;
}

} // namespace

void LuaScriptWindow::createDebugPanels()
{
    auto tree = [](const QStringList &headers) {
        auto t = new QTreeWidget;
        t->setColumnCount(headers.size());
        t->setHeaderLabels(headers);
        t->setHeaderHidden(headers.size() == 1);
        t->setUniformRowHeights(true);
        return t;
    };
    auto remove_action = [](QTreeWidget *t, const QString &text) {
        auto action = new QAction(text, t);
        action->setShortcut(QKeySequence::Delete);
        action->setShortcutContext(Qt::WidgetShortcut);
        t->setContextMenuPolicy(Qt::ActionsContextMenu);
        t->addAction(action);
        return action;
    };

    variables = tree({ tr("Variable") });
    panel_tabs->addTab(variables, tr("Variables"));

    auto watch_panel = new QWidget;
    auto watch_layout = new QVBoxLayout(watch_panel);
    watch_layout->setContentsMargins(0, 2, 0, 0);
    watch_layout->setSpacing(2);
    watch_edit = new QLineEdit;
    watch_edit->setPlaceholderText(tr("Add a watch: type an expression and press Enter"));
    watch_edit->setClearButtonEnabled(true);
    connect(watch_edit, &QLineEdit::returnPressed, this, [this] {
        addWatch(watch_edit->text());
        watch_edit->clear();
    });
    watch_tree = tree({ tr("Watch") });
    connect(remove_action(watch_tree, tr("Remove Watch")), &QAction::triggered, this, [this] {
        QTreeWidgetItem *item = watch_tree->currentItem();
        int index = item && !item->parent() ? watch_tree->indexOfTopLevelItem(item) : -1;
        if (index < 0 || index >= watches.size())
            return;
        watches.removeAt(index);
        fillWatches();
    });
    watch_layout->addWidget(watch_edit);
    watch_layout->addWidget(watch_tree);
    panel_tabs->addTab(watch_panel, tr("Watch"));

    for (QTreeWidget *t : { variables, watch_tree })
    {
        connect(t, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
            if (item->childCount() == 0)
                requestChildren(item);
        });
        // Long values are cut off in the tree; the status bar has room.
        connect(t, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
            if (item)
                status_label->setText(item->text(0));
        });
    }

    callstack = tree({ tr("Function"), tr("Location") });
    callstack->setRootIsDecorated(false);
    callstack->setColumnWidth(0, 260);
    connect(callstack, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        int index = item ? callstack->indexOfTopLevelItem(item) : -1;
        if (index >= 0 && index != frame)
            selectFrame(index);
    });
    panel_tabs->addTab(callstack, tr("Call Stack"));

    auto bp_panel = new QWidget;
    auto bp_layout = new QVBoxLayout(bp_panel);
    bp_layout->setContentsMargins(0, 0, 0, 2);
    bp_layout->setSpacing(2);
    breakpoint_list = tree({ tr("Location"), tr("Condition"), tr("Hit Count"), tr("Log Message") });
    breakpoint_list->setRootIsDecorated(false);
    breakpoint_list->setColumnWidth(0, 170);
    breakpoint_list->setColumnWidth(1, 170);
    breakpoint_list->setColumnWidth(2, 80);
    connect(breakpoint_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        Document *doc = nullptr;
        LuaCodeEditor::Breakpoint *b = nullptr;
        if (filling_breakpoints || column != 0 ||
            !(b = breakpointForRow(breakpoint_list->indexOfTopLevelItem(item), &doc)))
            return;
        bool enabled = item->checkState(0) == Qt::Checked;
        if (b->enabled == enabled)
            return;
        b->enabled = enabled;
        sendBreakpoints(doc);
        doc->editor->refreshMarkers();
    });
    connect(breakpoint_list, &QTreeWidget::currentItemChanged, this, [this] {
        if (!filling_breakpoints)
            fillBreakpointFields();
    });
    connect(breakpoint_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        Document *doc = nullptr;
        if (auto b = breakpointForRow(breakpoint_list->indexOfTopLevelItem(item), &doc))
        {
            tabs->setCurrentWidget(doc->editor);
            doc->editor->goToLine(doc->editor->breakpointLine(*b));
        }
    });
    connect(remove_action(breakpoint_list, tr("Remove Breakpoint")), &QAction::triggered, this, [this] {
        Document *doc = nullptr;
        if (auto b = selectedBreakpoint(&doc))
            doc->editor->toggleBreakpoint(doc->editor->breakpointLine(*b));
    });
    auto remove_all = new QAction(tr("Remove All Breakpoints"), breakpoint_list);
    connect(remove_all, &QAction::triggered, this, &LuaScriptWindow::removeAllBreakpoints);
    breakpoint_list->addAction(remove_all);
    bp_layout->addWidget(breakpoint_list);

    auto fields = new QHBoxLayout;
    fields->setContentsMargins(4, 0, 4, 0);
    const char *labels[3] = { QT_TR_NOOP("Condition:"), QT_TR_NOOP("Hit count:"), QT_TR_NOOP("Log message:") };
    const char *cues[3] = { "lives < 3", "5, >= 5 or % 5", "x is {x}" };
    QLineEdit **edits[3] = { &bp_condition, &bp_hit, &bp_log };
    int stretch[3] = { 3, 1, 4 };
    for (int i = 0; i < 3; i++)
    {
        auto edit = *edits[i] = new QLineEdit;
        edit->setPlaceholderText(cues[i]);
        edit->setEnabled(false);
        // Enter or leaving the field applies it.
        connect(edit, &QLineEdit::editingFinished, this, &LuaScriptWindow::applyBreakpointFields);
        fields->addWidget(new QLabel(tr(labels[i])));
        fields->addWidget(edit, stretch[i]);
    }
    bp_layout->addLayout(fields);
    panel_tabs->addTab(bp_panel, tr("Breakpoints"));
}

void LuaScriptWindow::debugCommand(S9xLuaDebugAction action)
{
    if (int script = S9xLuaDebugStoppedScript())
        S9xLuaDebugCommand(script, action);
}

void LuaScriptWindow::debuggerStopped(int script)
{
    if (released)
    {
        S9xLuaDebugCommand(script, S9X_LUA_DEBUG_STOP);
        return;
    }
    if (S9xLuaDebugStoppedScript() != script)
        return; // it went on already
    if (!holding)
    {
        // The script holds the emulation thread mid-frame: the other windows wait with it.
        holding = true;
        auto ours = [this](QWidget *w) { // this window, its floating dock, its popups
            for (; w; w = w->parentWidget())
                if (w == this)
                    return true;
            return false;
        };
        for (QWidget *w : QApplication::topLevelWidgets())
        {
            if (!ours(w) && w->isVisible() && w->isEnabled())
            {
                w->setEnabled(false);
                held_windows.push_back(w);
            }
        }
        setEnabled(true); // disabling the main window took its children along
        if (app->sound_driver)
            app->sound_driver->stop();
    }
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
    poll();
}

void LuaScriptWindow::debuggerResumed()
{
    if (!holding || S9xLuaDebugStoppedScript())
        return; // a step that stopped again keeps the hold
    holding = false;
    for (auto &w : held_windows)
        if (w)
            w->setEnabled(true);
    held_windows.clear();
    if (app->sound_driver && app->isCoreActive() && !app->isPaused())
        app->sound_driver->start();
    poll();
}

void LuaScriptWindow::releaseDebugger()
{
    released = true;
    S9xLuaDebugSetBreakOnError(false);
    for (auto &doc : documents)
        if (doc->script)
            S9xLuaDebugSetBreakpoints(doc->script, {});
    debugCommand(S9X_LUA_DEBUG_CONTINUE);
}

void LuaScriptWindow::pollDebug()
{
    std::vector<S9xLuaDebugEvent> events;
    if (!S9xLuaDebugTakeEvents(events))
        return;
    bool resumed = false;
    for (auto &e : events)
    {
        switch (e.kind)
        {
        case S9X_LUA_DEBUG_STOPPED:
            resumed = false;
            onStopped(e);
            break;
        case S9X_LUA_DEBUG_RESUMED:
            stopped = 0;
            resumed = true;
            break;
        case S9X_LUA_DEBUG_VARIABLES:
            onVariables(e);
            break;
        case S9X_LUA_DEBUG_EVALUATED:
            onEvaluated(e);
            break;
        }
    }
    // A step stops again at once: only clear the views if it didn't.
    if (resumed && !stopped)
        clearDebugViews();
}

void LuaScriptWindow::onStopped(const S9xLuaDebugEvent &e)
{
    stopped = e.script;
    frames = e.frames;
    int top = topFrame(frames);
    if (e.reason == "error")
        stop_text = tr("Stopped on an error: %1").arg(QString::fromStdString(e.message));
    else
    {
        QString why = e.reason == "breakpoint" ? tr("breakpoint") : e.reason == "step" ? tr("step") : tr("break");
        stop_text = tr("Paused at line %1 (%2)").arg(frames.empty() ? 0 : frames[top].line).arg(why);
    }
    fillCallStack();
    if (frames.empty())
    {
        fillVariables();
        fillWatches();
    }
    else
        selectFrame(top);
    if (panel_tabs->currentIndex() == 0)
        panel_tabs->setCurrentWidget(variables);
    updateActions();
}

void LuaScriptWindow::onVariables(const S9xLuaDebugEvent &e)
{
    for (size_t i = 0; i < pending.size();)
    {
        if (pending[i].first != e.ref)
        {
            i++;
            continue;
        }
        QTreeWidgetItem *item = pending[i].second;
        pending.erase(pending.begin() + i);
        deleteChildren(pending, item);
        for (auto &v : e.variables)
            item->addChild(variableItem(QString::fromStdString(v.name + " = " + v.value), v.ref));
        if (e.variables.empty())
            item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
        else
            item->setExpanded(true);
        i = 0; // the list changed under the loop
    }
}

void LuaScriptWindow::onEvaluated(const S9xLuaDebugEvent &e)
{
    int generation = e.request / 1000, index = e.request % 1000;
    if (generation != watch_generation || index >= watches.size() || e.variables.empty())
        return;
    QTreeWidgetItem *item = watch_tree->topLevelItem(index);
    if (!item)
        return;
    const S9xLuaDebugVariable &v = e.variables[0];
    QString text = watches[index] + " = " + (e.failed ? tr("error: ") : QString()) + QString::fromStdString(v.value);
    int ref = e.failed ? 0 : v.ref;
    deleteChildren(pending, item);
    item->setText(0, text);
    item->setToolTip(0, text);
    item->setData(0, Qt::UserRole, ref);
    item->setChildIndicatorPolicy(ref ? QTreeWidgetItem::ShowIndicator : QTreeWidgetItem::DontShowIndicator);
}

void LuaScriptWindow::clearDebugViews()
{
    frames.clear();
    pending.clear();
    for (auto &doc : documents)
        doc->editor->setDebugLines(0, 0);
    {
        QSignalBlocker blocker(callstack);
        callstack->clear();
    }
    clearTree(variables);
    fillWatches();
    updateActions();
}

void LuaScriptWindow::selectFrame(int index)
{
    if (index < 0 || index >= (int)frames.size())
        return;
    frame = index;

    int top = topFrame(frames);
    Document *stop_doc = frameDocument(frames[top]);
    Document *shown = frameDocument(frames[index]);
    for (auto &doc : documents)
        doc->editor->setDebugLines(doc.get() == stop_doc ? frames[top].line : 0,
                                   doc.get() == shown && index != top ? frames[index].line : 0);
    if (shown)
    {
        tabs->setCurrentWidget(shown->editor);
        shown->editor->showLine(frames[index].line);
    }

    {
        QSignalBlocker blocker(callstack);
        callstack->setCurrentItem(callstack->topLevelItem(index));
    }
    fillVariables();
    fillWatches();
}

// The document a frame's code is in, opening its file if it has to.
LuaScriptWindow::Document *LuaScriptWindow::frameDocument(const S9xLuaDebugFrame &f)
{
    if (f.line <= 0)
        return nullptr;
    if (f.own)
        return documentForScript(stopped);
    if (f.path.empty())
        return nullptr;
    QFileInfo info(QString::fromStdString(f.path));
    auto find = [&]() -> Document * {
        for (auto &doc : documents)
            if (!doc->path.isEmpty() && QFileInfo(doc->path) == info)
                return doc.get();
        return nullptr;
    };
    if (Document *doc = find())
        return doc;
    if (!info.isFile())
        return nullptr;
    openScript(info.absoluteFilePath(), false);
    return find();
}

void LuaScriptWindow::fillVariables()
{
    clearTree(variables);
    if (!stopped || frame >= (int)frames.size())
        return;
    const char *scopes[3] = { QT_TR_NOOP("Locals"), QT_TR_NOOP("Upvalues"), QT_TR_NOOP("Globals") };
    for (int scope = 0; scope < 3; scope++)
    {
        auto item = variableItem(tr(scopes[scope]), S9xLuaDebugScopeRef(frame, scope));
        variables->addTopLevelItem(item);
        if (scope == 0)
            requestChildren(item);
    }
}

// Every watch, evaluated again in the selected frame.
void LuaScriptWindow::fillWatches()
{
    clearTree(watch_tree);
    watch_generation++;
    for (int i = 0; i < watches.size(); i++)
    {
        watch_tree->addTopLevelItem(variableItem(watches[i], 0));
        if (stopped)
            S9xLuaDebugEvaluate(stopped, frame, watches[i].toStdString(), watch_generation * 1000 + i);
    }
}

void LuaScriptWindow::fillCallStack()
{
    QSignalBlocker blocker(callstack);
    callstack->clear();
    for (auto &f : frames)
    {
        std::string location = f.line ? f.source + ":" + std::to_string(f.line) : f.source.empty() ? "[C]" : f.source;
        callstack->addTopLevelItem(
            new QTreeWidgetItem(QStringList{ QString::fromStdString(f.function), QString::fromStdString(location) }));
    }
}

void LuaScriptWindow::clearTree(QTreeWidget *tree)
{
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [tree](const std::pair<int, QTreeWidgetItem *> &p) { return p.second->treeWidget() == tree; }),
                  pending.end());
    tree->clear();
}

// A variable request whose answer fills the item's children.
void LuaScriptWindow::requestChildren(QTreeWidgetItem *item)
{
    int ref = item->data(0, Qt::UserRole).toInt();
    if (!stopped || !ref)
        return;
    pending.push_back({ ref, item });
    S9xLuaDebugRequestVariables(stopped, ref);
}

void LuaScriptWindow::addWatch(const QString &expression)
{
    QString text = expression.trimmed();
    if (text.isEmpty() || watches.size() >= 999)
        return;
    watches.push_back(text);
    fillWatches();
}

// To the running script; its code is what was run, so lines are as they are now.
void LuaScriptWindow::sendBreakpoints(Document *doc)
{
    if (!doc->script)
        return;
    std::vector<S9xLuaBreakpoint> list;
    for (auto &b : doc->editor->breakpoints)
    {
        S9xLuaBreakpoint bp;
        bp.line = doc->editor->breakpointLine(b);
        bp.enabled = b.enabled;
        bp.condition = b.condition.toStdString();
        bp.hit_condition = b.hit.toStdString();
        bp.log_message = b.log.toStdString();
        list.push_back(bp);
    }
    S9xLuaDebugSetBreakpoints(doc->script, list);
}

void LuaScriptWindow::breakpointsChanged(Document *doc)
{
    doc->editor->mergeBreakpoints();
    sendBreakpoints(doc);
    refreshBreakpointList();
    doc->editor->refreshMarkers();
}

void LuaScriptWindow::refreshBreakpointList()
{
    if (!breakpoint_list)
        return;
    filling_breakpoints = true;
    QTreeWidgetItem *current = breakpoint_list->currentItem();
    int selected = current ? breakpoint_list->indexOfTopLevelItem(current) : -1;
    breakpoint_list->clear();
    bp_rows.clear();
    for (auto &doc : documents)
    {
        auto &list = doc->editor->breakpoints;
        std::vector<std::pair<int, size_t>> order;
        for (size_t i = 0; i < list.size(); i++)
            order.push_back({ doc->editor->breakpointLine(list[i]), i });
        std::sort(order.begin(), order.end());
        for (auto &entry : order)
        {
            const LuaCodeEditor::Breakpoint &b = list[entry.second];
            bp_rows.push_back({ doc.get(), entry.second });
            auto item = new QTreeWidgetItem(QStringList{ QString("%1:%2").arg(displayName(doc.get())).arg(entry.first),
                                                         b.condition, b.hit, b.log });
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, b.enabled ? Qt::Checked : Qt::Unchecked);
            breakpoint_list->addTopLevelItem(item);
        }
    }
    if (selected >= 0 && selected < breakpoint_list->topLevelItemCount())
        breakpoint_list->setCurrentItem(breakpoint_list->topLevelItem(selected));
    filling_breakpoints = false;
    fillBreakpointFields();
}

LuaCodeEditor::Breakpoint *LuaScriptWindow::breakpointForRow(int row, Document **owner)
{
    if (row < 0 || row >= (int)bp_rows.size())
        return nullptr;
    Document *doc = bp_rows[row].first;
    size_t index = bp_rows[row].second;
    bool open = std::any_of(documents.begin(), documents.end(),
                            [doc](const std::unique_ptr<Document> &d) { return d.get() == doc; });
    if (!open || index >= doc->editor->breakpoints.size())
        return nullptr;
    if (owner)
        *owner = doc;
    return &doc->editor->breakpoints[index];
}

LuaCodeEditor::Breakpoint *LuaScriptWindow::selectedBreakpoint(Document **owner)
{
    QTreeWidgetItem *item = breakpoint_list->currentItem();
    return item ? breakpointForRow(breakpoint_list->indexOfTopLevelItem(item), owner) : nullptr;
}

void LuaScriptWindow::fillBreakpointFields()
{
    LuaCodeEditor::Breakpoint *b = selectedBreakpoint(nullptr);
    QLineEdit *edits[3] = { bp_condition, bp_hit, bp_log };
    QString values[3] = { b ? b->condition : QString(), b ? b->hit : QString(), b ? b->log : QString() };
    for (int i = 0; i < 3; i++)
    {
        edits[i]->setText(values[i]);
        edits[i]->setEnabled(b != nullptr);
    }
}

void LuaScriptWindow::applyBreakpointFields()
{
    Document *doc = nullptr;
    LuaCodeEditor::Breakpoint *b = selectedBreakpoint(&doc);
    if (!b)
        return;
    QString condition = bp_condition->text().trimmed(), hit = bp_hit->text().trimmed(), log = bp_log->text();
    if (condition == b->condition && hit == b->hit && log == b->log)
        return;
    b->condition = condition;
    b->hit = hit;
    b->log = log;
    sendBreakpoints(doc);

    // The row in place: a rebuild here could pull the list out from under a click.
    if (QTreeWidgetItem *item = breakpoint_list->currentItem())
    {
        filling_breakpoints = true;
        item->setText(1, condition);
        item->setText(2, hit);
        item->setText(3, log);
        filling_breakpoints = false;
    }
    doc->editor->refreshMarkers();
}

void LuaScriptWindow::editBreakpoint(Document *doc, int line)
{
    if (!doc->editor->breakpointAt(line))
        doc->editor->toggleBreakpoint(line);
    panel_tabs->setCurrentWidget(breakpoint_list->parentWidget());
    for (size_t row = 0; row < bp_rows.size(); row++)
    {
        if (bp_rows[row].first != doc ||
            doc->editor->breakpointLine(doc->editor->breakpoints[bp_rows[row].second]) != line)
            continue;
        QTreeWidgetItem *item = breakpoint_list->topLevelItem((int)row);
        breakpoint_list->setCurrentItem(item);
        breakpoint_list->scrollToItem(item);
        break;
    }
    bp_condition->setFocus();
    bp_condition->selectAll();
}

void LuaScriptWindow::removeAllBreakpoints()
{
    for (auto &doc : documents)
    {
        doc->editor->breakpoints.clear();
        sendBreakpoints(doc.get());
        doc->editor->refreshMarkers();
    }
    refreshBreakpointList();
}

void LuaScriptWindow::showBreakpointMenu(Document *doc, int line, const QPoint &global)
{
    LuaCodeEditor *editor = doc->editor;
    LuaCodeEditor::Breakpoint *b = editor->breakpointAt(line);
    QMenu menu(this);
    menu.addAction(b ? tr("Remove Breakpoint") : tr("Add Breakpoint"), this, [editor, line] {
        editor->toggleBreakpoint(line);
    });
    menu.addAction(b ? tr("Edit Breakpoint...") : tr("Add Conditional Breakpoint..."), this, [this, doc, line] {
        editBreakpoint(doc, line);
    });
    if (b)
    {
        menu.addAction(b->enabled ? tr("Disable Breakpoint") : tr("Enable Breakpoint"), this, [this, doc, line] {
            if (auto bp = doc->editor->breakpointAt(line))
            {
                bp->enabled = !bp->enabled;
                breakpointsChanged(doc);
            }
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Remove All Breakpoints"), this, &LuaScriptWindow::removeAllBreakpoints)->setEnabled(!bp_rows.empty());
    menu.exec(global);
}
