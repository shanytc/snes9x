#include "BiosManagerDialog.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>
#include <map>

#include "snes9x.h"
#include "memmap.h"

namespace {

// Sidebar item data: a slot, kHeading + family for a heading, or kGroup + group for a group.
constexpr int kHeading = S9X_NUM_BIOS_SLOTS;
constexpr int kGroup   = kHeading + S9X_BIOS_NUM_FAMILIES;

const QColor kGood(0x1E, 0x8B, 0x3A), kBad(0xC0, 0x39, 0x2B);

// A chip's modes, in its box's order and the speed chart's, each with its colour there.
const struct { const char *name; int mode; QColor ink; } kModes[] = {
    { QT_TRANSLATE_NOOP("BiosManagerDialog", "Legacy (HLE)"), S9X_CHIP_HLE,      QColor(0x8E, 0x9A, 0xA6) },
    { QT_TRANSLATE_NOOP("BiosManagerDialog", "Native (LLE)"), S9X_CHIP_NATIVE,   QColor(0x2E, 0x9E, 0x4F) },
    { QT_TRANSLATE_NOOP("BiosManagerDialog", "Firmware"),     S9X_CHIP_FIRMWARE, QColor(0x2F, 0x7E, 0xD8) },
};

QString tr(const char *text)
{
    return QCoreApplication::translate("BiosManagerDialog", text);
}

// A slot's parent in the sidebar, as item data; -1 for straight under its family.
int parentOf(int slot)
{
    const S9xBiosNesting n = S9xGetBiosNesting(slot);
    if (n.parent < 0)
        return -1;
    return (n.parent >= S9X_BIOS_UNDER_GROUP) ? kGroup + n.parent - S9X_BIOS_UNDER_GROUP : n.parent;
}

// A card: a rounded panel in the base colour on the dialog's background.
class CardWidget : public QWidget
{
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().color(QPalette::Base));
        const qreal r = fontMetrics().height() * 0.5;
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), r, r);
    }
};

// The white-on-red cross before a status that's trouble.
QPixmap crossBadge(int size, qreal dpr)
{
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(kBad);
    p.drawRoundedRect(QRectF(0, 0, size, size), size / 2.0, size / 2.0);
    p.setPen(QPen(Qt::white, std::max(1.0, size / 7.0), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(size * 0.32, size * 0.32), QPointF(size * 0.68, size * 0.68));
    p.drawLine(QPointF(size * 0.68, size * 0.32), QPointF(size * 0.32, size * 0.68));
    return pm;
}

// The speed chart's button face: a small bar chart in the modes' colours.
QIcon benchIcon(int size)
{
    static const int kTall[3] = { 45, 65, 100 };   // percent of the icon, short to tall
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const int bw = std::max(2, size / 4), gap = std::max(1, (size - 3 * bw) / 4);
    for (int i = 0; i < 3; i++)
    {
        const int h = (size - 2) * kTall[i] / 100;
        p.fillRect(gap + i * (bw + gap), size - 1 - h, bw, h, kModes[i].ink);
    }
    return QIcon(pm);
}

// The speed chart: a group of bars per measured scene, one per mode on a scale from 0,
// with gridlines under them and each bar's value after it; `chip` shows the time in
// the chip rather than the frame time.
class BenchChart : public QWidget
{
  public:
    BenchChart(int slot, QWidget *parent) : QWidget(parent), slot(slot)
    {
        int bar, gap, group, pad;
        const int th = fontMetrics().height(), n = S9xBiosBenchScenes(slot);
        geometry(th, bar, gap, group, pad);
        setFixedHeight(pad + th + pad + n * (3 * bar + 2 * gap) + (n - 1) * group + 2 * gap + th + pad);
        setMinimumWidth(th * 34);
    }

    void setChip(bool on)
    {
        chip = on;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter          p(this);
        const QFontMetrics fm = fontMetrics();
        const int         th = fm.height();
        int               bar, gap, group, pad;
        geometry(th, bar, gap, group, pad);

        const QColor paper = palette().color(QPalette::Base), ink = palette().color(QPalette::Text);
        const QColor grid((paper.red() * 5 + ink.red()) / 6, (paper.green() * 5 + ink.green()) / 6,
                          (paper.blue() * 5 + ink.blue()) / 6);
        p.fillRect(rect(), paper);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        // The legend: each mode's swatch and name.
        int x = pad;
        for (const auto &m : kModes)
        {
            const int sw = th * 2 / 3;
            p.fillRect(x, pad + (th - sw) / 2, sw, sw, m.ink);
            x += sw + th / 3;
            p.setPen(ink);
            p.drawText(QRect(x, pad, th * 20, th), Qt::AlignLeft | Qt::AlignVCenter, tr(m.name));
            x += fm.horizontalAdvance(tr(m.name)) + th;
        }

        // Columns: the scenes' names, the bars, then room for the widest label.
        int                      count = 0, scenes = 0, namew = 0, labelw = 0;
        double                   most  = 0;
        const S9xBiosBenchScene *bench = S9xGetBiosBench(&count);
        for (int k = 0; k < count; k++)
        {
            if (bench[k].slot != slot)
                continue;
            scenes++;
            namew = std::max(namew, fm.horizontalAdvance(QString::fromUtf8(bench[k].scene)));
            for (int m = 0; m < 3; m++)
            {
                labelw = std::max(labelw, fm.horizontalAdvance(label(bench[k], m)));
                most   = std::max(most, chip ? (double) bench[k].ticks[m] : (double) bench[k].ms[m]);
            }
        }
        static const double kSteps[] = { 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000 };
        double step = kSteps[std::size(kSteps) - 1];
        for (double s : kSteps)
            if (most / s <= 5)
            {
                step = s;
                break;
            }
        int lines = (int) (most / step);
        if (lines * step < most || lines < 1)
            lines++;
        const double top = lines * step;

        const int x0   = pad + namew + th;
        const int x1   = width() - pad - th / 3 - labelw;
        const int y0   = pad + th + pad;
        const int y1   = y0 + scenes * (3 * bar + 2 * gap) + (scenes - 1) * group;
        auto      x_of = [&](double v) { return x0 + (int) ((x1 - x0) * v / top + 0.5); };

        // Gridlines and their numbers, then the zero line over them.
        for (int t = 0; t <= lines; t++)
        {
            const int gx = x_of(t * step);
            p.setPen(grid);
            p.drawLine(gx, y0 - gap, gx, y1 + gap);
            const QString num = (chip && t) ? QString::number(t * step) + "M" : QString::number(t * step);
            p.setPen(ink);
            p.drawText(QRect(gx - th * 3, y1 + 2 * gap, th * 6, th), Qt::AlignHCenter | Qt::AlignTop, num);
        }
        p.drawLine(x0, y0 - gap, x0, y1 + gap);

        // Each scene: its name level with its three bars, each bar's value after it.
        int y = y0;
        for (int k = 0; k < count; k++)
        {
            const S9xBiosBenchScene &b = bench[k];
            if (b.slot != slot)
                continue;
            p.setPen(ink);
            p.drawText(QRect(pad, y, namew, 3 * bar + 2 * gap), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::fromUtf8(b.scene));
            for (int m = 0; m < 3; m++)
            {
                const int right = std::max(x_of(chip ? (double) b.ticks[m] : (double) b.ms[m]), x0 + 2);
                p.fillRect(QRect(QPoint(x0 + 1, y), QPoint(right - 1, y + bar - 1)), kModes[m].ink);
                p.drawText(QRect(right + th / 3, y, labelw + th, bar), Qt::AlignLeft | Qt::AlignVCenter, label(b, m));
                y += bar + gap;
            }
            y += group - gap;
        }
    }

  private:
    static void geometry(int th, int &bar, int &gap, int &group, int &pad)
    {
        bar   = th;
        gap   = std::max(1, th / 6);
        group = th;
        pad   = th / 2 + 2;
    }

    // A bar's label: frame time with its change against the HLE, or the time in the chip.
    QString label(const S9xBiosBenchScene &b, int mode) const
    {
        if (chip)
            return b.ticks[mode] ? QString("%1M").arg(b.ticks[mode]) : QStringLiteral("<1M");
        QString text = QString::asprintf("%.3f ms", b.ms[mode]);
        if (mode)
            text += QString::asprintf("   %+.1f%%", (b.ms[mode] - b.ms[0]) * 100.0 / b.ms[0]);
        return text;
    }

    int  slot;
    bool chip = false;
};

// The speed chart's popup for one chip slot.
void showBenchChart(QWidget *parent, int slot)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QString::fromUtf8(S9xGetBiosSlotInfo(slot)->label) + " " + tr("Speed"));
    auto layout = new QVBoxLayout(&dialog);

    auto views = new QHBoxLayout();
    auto frame = new QRadioButton(tr("Frame time"));
    auto chip  = new QRadioButton(tr("Time in the chip"));
    frame->setChecked(true);
    views->addWidget(frame);
    views->addWidget(chip);
    views->addStretch(1);
    layout->addLayout(views);

    auto note = new QLabel();
    note->setWordWrap(true);
    layout->addWidget(note);
    auto chart = new BenchChart(slot, &dialog);
    layout->addWidget(chart);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto show = [chip, chart, note] {
        chart->setChip(chip->isChecked());
        note->setText(chip->isChecked()
                          ? tr("Time spent inside the chip over the same scenes, in millions of CPU clock ticks. Lower is faster. Measured on one PC.")
                          : tr("Milliseconds the emulator takes per frame while each game plays a scripted scene. Lower is faster. Measured on one PC."));
    };
    QObject::connect(chip, &QRadioButton::toggled, &dialog, show);
    show();
    dialog.exec();
}

} // namespace

BiosManagerDialog::BiosManagerDialog(QWidget *parent, EmuApplication *app)
    : QDialog(parent), app(app)
{
    setWindowTitle(tr("BIOS Manager"));
    const int th = fontMetrics().height();

    auto outer = new QVBoxLayout(this);
    auto intro = new QLabel(tr("Configure BIOS files for system emulation. Nothing is searched for: "
                               "an entry without a file runs its built-in, if it has one, or is unavailable."));
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto body = new QHBoxLayout();
    outer->addLayout(body, 1);

    // The sidebar: a heading per family, bold with its theme icon, over its entries.
    static const char *const kIcons[S9X_BIOS_NUM_FAMILIES][2] = {
        { "input-gaming", "applications-games" }, { "application-x-addon", "preferences-plugins" },
        { "cpu", "computer" }, { "phone", "input-gaming" } };
    sidebar = new QTreeWidget();
    sidebar->setHeaderHidden(true);
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
    {
        auto heading = new QTreeWidgetItem(sidebar, { tr(S9xGetBiosFamily(g)->name) });
        heading->setData(0, Qt::UserRole, kHeading + g);
        QFont bold = heading->font(0);
        bold.setBold(true);
        heading->setFont(0, bold);
        heading->setIcon(0, QIcon::fromTheme(kIcons[g][0], QIcon::fromTheme(kIcons[g][1])));

        std::map<int, QTreeWidgetItem *> under;   // the entries so far, by item data
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
        {
            const S9xBiosNesting n = S9xGetBiosNesting(*s);
            if (!n.listed)
                continue;
            const int        parent = parentOf(*s);
            QTreeWidgetItem *host   = heading;
            if (parent >= kGroup)
            {
                if (!under[parent])
                {
                    under[parent] = new QTreeWidgetItem(heading, { tr(S9xBiosGroupName(parent - kGroup)) });
                    under[parent]->setData(0, Qt::UserRole, parent);
                }
                host = under[parent];
            }
            else if (parent >= 0 && under[parent])
                host = under[parent];
            auto item = new QTreeWidgetItem(host, { n.name ? tr(n.name) : QString::fromUtf8(S9xGetBiosSlotInfo(*s)->label) });
            item->setData(0, Qt::UserRole, *s);
            under[*s] = item;
        }
    }
    // As wide as its widest entry with everything open, so opening one never resizes it.
    sidebar->expandAll();
    sidebar->header()->setStretchLastSection(false);
    sidebar->resizeColumnToContents(0);
    sidebar->setFixedWidth(sidebar->columnWidth(0) + sidebar->frameWidth() * 2 + th * 2);
    sidebar->header()->setStretchLastSection(true);
    sidebar->collapseAll();
    body->addWidget(sidebar);

    // The cards, one per BIOS in sidebar order; the pick shows its own.
    auto scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto list   = new QWidget();
    auto layout = new QVBoxLayout(list);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(th / 2);
    cards.resize(S9X_NUM_BIOS_SLOTS);
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
            addCard(*s, layout);
    layout->addStretch(1);
    scroll->setWidget(list);
    body->addWidget(scroll, 1);

    // Cancel, and Save Changes in the accent colour.
    auto buttons = new QDialogButtonBox();
    buttons->addButton(QDialogButtonBox::Cancel);
    auto save = buttons->addButton(tr("Save Changes"), QDialogButtonBox::AcceptRole);
    save->setDefault(true);
    save->setStyleSheet("QPushButton { background: palette(highlight); color: palette(highlighted-text);"
                        " border: none; border-radius: 4px; padding: 5px 16px; }"
                        " QPushButton:pressed { background: palette(dark); }");
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &BiosManagerDialog::applyAndClose);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
        refreshCard(slot);
    connect(sidebar, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { showPick(item); });
    sidebar->setCurrentItem(sidebar->topLevelItem(0));
    showPick(sidebar->currentItem());

    // Room for the biggest family's cards without scrolling, as on win32, as far as the screen allows.
    int family = 0;
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
    {
        int h = -layout->spacing();
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
            h += cards[*s].frame->sizeHint().height() + layout->spacing();
        family = std::max(family, h);
    }
    const QRect room = screen()->availableGeometry();
    resize(std::min(std::max(sizeHint().width(), th * 64), room.width() * 9 / 10),
           std::min(sizeHint().height() - scroll->sizeHint().height() + family + th, room.height() * 9 / 10));
}

void BiosManagerDialog::addCard(int slot, QVBoxLayout *list)
{
    const auto *info = S9xGetBiosSlotInfo(slot);
    const int   th   = fontMetrics().height();
    Card       &c    = cards[slot];

    auto frame = new CardWidget();
    auto grid  = new QGridLayout(frame);
    grid->setContentsMargins(th * 2 / 3, th / 2, th * 2 / 3, th / 2);
    grid->setHorizontalSpacing(th / 2);
    grid->setVerticalSpacing(th / 4);

    // The label and the icons after it, centred on the card: the dumps it takes, a chip's speed chart.
    c.label = new QLabel(QString::fromUtf8(info->label));
    grid->addWidget(c.label, 0, 0, 2, 1, Qt::AlignVCenter);
    auto icons = new QHBoxLayout();
    icons->setSpacing(th / 4);
    if (info->info)
    {
        auto about = new QToolButton();
        about->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
        about->setAutoRaise(true);
        about->setCursor(Qt::PointingHandCursor);
        const QString text = QString::fromUtf8(S9xBiosSlotInfoText(slot).c_str());
        about->setToolTip(text);
        connect(about, &QToolButton::clicked, this, [this, info, text] {
            QMessageBox::information(this, QString::fromUtf8(info->label), text);
        });
        icons->addWidget(about);
    }
    if (S9xBiosBenchScenes(slot))
    {
        auto speed = new QToolButton();
        speed->setIcon(benchIcon(16));
        speed->setAutoRaise(true);
        speed->setCursor(Qt::PointingHandCursor);
        speed->setToolTip(tr("How fast Legacy, Native and Firmware run"));
        connect(speed, &QToolButton::clicked, this, [this, slot] { showBenchChart(this, slot); });
        icons->addWidget(speed);
    }
    grid->addLayout(icons, 0, 1, 2, 1, Qt::AlignVCenter);

    // The status line, after a chip's mode box, over the path box, Browse and Clear.
    auto line = new QHBoxLayout();
    line->setSpacing(th / 3);
    if (S9xBiosSlotHasChipMode(slot))
    {
        c.mode = new QComboBox();
        for (const auto &m : kModes)
            c.mode->addItem(tr(m.name), m.mode);
        c.mode->setCurrentIndex(c.mode->findData(S9xChipModeInEffect(slot)));
        c.mode->setToolTip(tr("Legacy (HLE): the old high-level code, some games glitch\n"
                              "Native (LLE): the chip itself, exact, no file needed\n"
                              "Firmware: the chip running the dump picked here, exact but slower\n"
                              "Takes effect at the next load or hard reset"));
        connect(c.mode, &QComboBox::currentIndexChanged, this, [this, slot] { refreshCard(slot); });
        line->addWidget(c.mode);
    }
    c.mark = new QLabel();
    c.mark->setPixmap(crossBadge(th * 3 / 4, devicePixelRatioF()));
    line->addWidget(c.mark);
    c.status = new QLabel();
    c.status->setTextFormat(Qt::PlainText);
    c.status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    line->addWidget(c.status, 1);
    grid->addLayout(line, 0, 2, 1, 3);

    c.edit = new QLineEdit(QString::fromUtf8(S9xGetBiosPath(slot)));
    c.edit->setPlaceholderText(QString::fromUtf8(info->names[0]));
    c.edit->setMinimumWidth(th * 16);
    c.browse = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Browse"));
    c.clear  = new QPushButton(QIcon::fromTheme("edit-delete", style()->standardIcon(QStyle::SP_TrashIcon)), tr("Clear"));
    grid->addWidget(c.edit, 1, 2);
    grid->addWidget(c.browse, 1, 3);
    grid->addWidget(c.clear, 1, 4);
    grid->setColumnStretch(2, 1);

    connect(c.browse, &QPushButton::clicked, this, [this, slot] { browse(slot); });
    connect(c.clear, &QPushButton::clicked, this, [this, slot] { cards[slot].edit->clear(); });
    connect(c.edit, &QLineEdit::textChanged, this, [this, slot] { refreshCard(slot); });

    c.frame = frame;
    list->addWidget(frame);
}

// The pick's cards: a slot's own, a heading's whole family, or what a slot or group holds;
// their labels as wide as the widest of them, so their icons line up.
void BiosManagerDialog::showPick(QTreeWidgetItem *item)
{
    const int pick  = item ? item->data(0, Qt::UserRole).toInt() : -1;
    int       label = 0;
    for (int g = 0; g < S9X_BIOS_NUM_FAMILIES; g++)
        for (const int *s = S9xGetBiosFamily(g)->members; *s >= 0; s++)
        {
            const int  parent = parentOf(*s);
            const bool shown  = pick == *s || pick == kHeading + g || (parent >= 0 && pick == parent);
            cards[*s].frame->setVisible(shown);
            if (shown)
                label = std::max(label, cards[*s].label->sizeHint().width());
        }
    for (Card &c : cards)
        if (c.frame->isVisibleTo(this))
            c.label->setFixedWidth(label);
}

void BiosManagerDialog::browse(int slot)
{
    const auto *info = S9xGetBiosSlotInfo(slot);
    QString start = cards[slot].edit->text();
    if (start.isEmpty())
        start = QString::fromStdString(S9xGetDirectory(BIOS_DIR));

    const QString file = QFileDialog::getOpenFileName(
        this, tr("Select %1 BIOS").arg(QString::fromUtf8(info->label)), start,
        tr("BIOS files (*.zip *.bin *.rom *.sfc *.gb *.gbc *.BIN);;All files (*)"));

    if (!file.isEmpty())
        cards[slot].edit->setText(file);
}

int BiosManagerDialog::chipMode(int slot) const
{
    return cards[slot].mode ? cards[slot].mode->currentData().toInt() : -1;
}

void BiosManagerDialog::refreshCard(int slot)
{
    // Validate against the live text, not the stored path, so typing shows up.
    Card         &c       = cards[slot];
    const QString text    = c.edit->text();
    // A chip reads its file only in Firmware mode; otherwise the path waits greyed out.
    const int     mode    = chipMode(slot);
    const bool    file_on = mode < 0 || mode == S9X_CHIP_FIRMWARE;
    c.edit->setEnabled(file_on);
    c.browse->setEnabled(file_on);
    c.clear->setEnabled(file_on && !text.isEmpty());   // nothing to clear on a blank card

    enum { Plain, Note, Good, Bad } look = Plain;
    QString says;
    if (mode >= 0 && (!file_on || text.isEmpty()))
    {
        says = (mode == S9X_CHIP_HLE)      ? tr("Fastest (less accurate)")
             : (mode == S9X_CHIP_NATIVE)   ? tr("Fast (chip accurate)")
                                           : tr("Nothing selected: Slow (chip accurate)");
        look = file_on ? Bad : Plain;
    }
    else if (text.isEmpty())
    {
        // Empty is fine for some slots and not others, so say which.
        const char *note = S9xGetBiosSlotInfo(slot)->note;
        says = note ? tr(note) : QString();
        look = Note;
    }
    else if (!QFileInfo(text).isFile())
    {
        says = tr("not found");
        look = Bad;
    }
    else
    {
        const char       *saved = S9xGetBiosPath(slot);
        const std::string keep(saved ? saved : "");
        S9xSetBiosPath(slot, text.toUtf8().constData());
        std::string why;
        const S9xBiosPathStatus st = S9xCheckBiosPath(slot, &why);
        S9xSetBiosPath(slot, keep.c_str());

        // The reason is the useful half: which size the slot wanted, or what the
        // file turned out to be. Falls back to the status when there is none.
        const QString detail = QString::fromStdString(why);
        if (st == S9X_BIOS_PATH_OK)
        {
            says = (mode >= 0) ? tr("Slow (chip accurate)") : why.empty() ? tr("OK") : tr("OK") + " - " + detail;
            look = Good;
        }
        else
        {
            says = (st == S9X_BIOS_PATH_MISSING)   ? tr("not found")
                 : !why.empty()                    ? detail
                 : (st == S9X_BIOS_PATH_BAD_IMAGE) ? tr("wrong image")
                                                   : tr("unexpected size");
            look = Bad;
        }
    }

    // The full line on hover, for when a long path gets cut short.
    c.status->setText(says);
    c.status->setToolTip(says);
    c.mark->setVisible(look == Bad);
    QFont font = c.status->font();
    font.setItalic(look == Note);
    c.status->setFont(font);
    c.status->setStyleSheet(look == Good ? "color: #1e8b3a;" : look == Bad ? "color: #c0392b;"
                            : look == Note ? "color: palette(mid);" : "");
}

void BiosManagerDialog::applyAndClose()
{
    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        const std::string path = cards[slot].edit->text().toStdString();
        S9xSetBiosPath(slot, path.c_str());
        app->config->bios_paths[slot] = path;
        const int mode = chipMode(slot);
        if (mode >= 0)
        {
            S9xSetChipMode(slot, mode);
            app->config->chip_modes[slot] = mode;
        }
    }

    accept();
}
