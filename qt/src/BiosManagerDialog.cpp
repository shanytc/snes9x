#include "BiosManagerDialog.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QMessageBox>
#include <QStyle>
#include <QToolButton>

#include "snes9x.h"
#include "memmap.h"

BiosManagerDialog::BiosManagerDialog(QWidget *parent, EmuApplication *app)
    : QDialog(parent), app(app)
{
    setWindowTitle(tr("BIOS Manager"));

    auto outer = new QVBoxLayout(this);

    auto intro = new QLabel(tr("Point each entry at its BIOS file. Nothing is searched for: "
                               "a blank entry means that BIOS is unavailable."));
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto grid = new QGridLayout();
    grid->setColumnStretch(0, 1);
    outer->addLayout(grid);

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        const auto *info = S9xGetBiosSlotInfo(slot);

        auto edit = new QLineEdit(QString::fromUtf8(S9xGetBiosPath(slot)));
        edit->setPlaceholderText(QString::fromUtf8(info->names[0]));
        edit->setMinimumWidth(340);
        auto status = new QLabel();

        auto select = new QPushButton(tr("Select..."));
        auto clear = new QPushButton(QStringLiteral("✕"));
        clear->setToolTip(tr("Clear"));
        clear->setFixedWidth(clear->fontMetrics().height() * 2);

        // The name on its own line behind its info icon, the path controls under it.
        const int r = slot * 2;
        auto name = new QWidget();
        auto name_row = new QHBoxLayout(name);
        name_row->setContentsMargins(0, slot ? 6 : 0, 0, 0);
        if (info->info)
        {
            auto about = new QToolButton();
            about->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
            about->setAutoRaise(true);
            about->setCursor(Qt::PointingHandCursor);
            about->setToolTip(QString::fromUtf8(info->info));
            connect(about, &QToolButton::clicked, this, [this, info] {
                QMessageBox::information(this, QString::fromUtf8(info->label), QString::fromUtf8(info->info));
            });
            name_row->addWidget(about);
        }
        name_row->addWidget(new QLabel(QString::fromUtf8(info->label)));
        name_row->addStretch();
        grid->addWidget(name, r, 0, 1, 4);
        grid->addWidget(edit, r + 1, 0);
        grid->addWidget(select, r + 1, 1);
        grid->addWidget(clear, r + 1, 2);
        grid->addWidget(status, r + 1, 3);

        connect(select, &QPushButton::clicked, this, [this, slot] { browse(slot); });
        connect(clear, &QPushButton::clicked, this, [this, slot] {
            rows[slot].edit->clear();
            refreshRow(slot);
        });
        connect(edit, &QLineEdit::textChanged, this, [this, slot] { refreshRow(slot); });

        rows.push_back({ edit, status, clear });
    }

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &BiosManagerDialog::applyAndClose);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
        refreshRow(slot);
}

void BiosManagerDialog::browse(int slot)
{
    const auto *info = S9xGetBiosSlotInfo(slot);
    QString start = rows[slot].edit->text();
    if (start.isEmpty())
        start = QString::fromStdString(S9xGetDirectory(BIOS_DIR));

    const QString file = QFileDialog::getOpenFileName(
        this, tr("Select %1 BIOS").arg(QString::fromUtf8(info->label)), start,
        tr("BIOS files (*.zip *.bin *.rom *.sfc *.gb *.gbc *.BIN);;All files (*)"));

    if (!file.isEmpty())
        rows[slot].edit->setText(file);
}

void BiosManagerDialog::refreshRow(int slot)
{
    refreshRowStatus(slot);
    // The full line on hover, for when a long path gets cut short.
    rows[slot].status->setToolTip(rows[slot].status->text());
}

void BiosManagerDialog::refreshRowStatus(int slot)
{
    // Validate against the live text, not the stored path, so typing shows up.
    const QString text = rows[slot].edit->text();
    QLabel *status = rows[slot].status;
    rows[slot].clear->setEnabled(!text.isEmpty());   // nothing to clear on a blank row

    if (text.isEmpty())
    {
        // Empty is fine for some slots and not others, so say which.
        const char *note = S9xGetBiosSlotInfo(slot)->note;
        status->setText(note ? tr(note) : QString());
        status->setStyleSheet("color: palette(mid);");
        return;
    }

    QFileInfo fi(text);
    if (!fi.isFile())
    {
        status->setText(tr("not found"));
        status->setStyleSheet("color: #c0392b;");
        return;
    }

    const char *saved = S9xGetBiosPath(slot);
    const std::string keep(saved ? saved : "");
    S9xSetBiosPath(slot, text.toUtf8().constData());
    std::string why;
    const S9xBiosPathStatus st = S9xCheckBiosPath(slot, &why);
    S9xSetBiosPath(slot, keep.c_str());

    // The reason is the useful half: which size the slot wanted, or what the
    // file turned out to be. Falls back to the status when there is none.
    const bool ok = (st == S9X_BIOS_PATH_OK);
    const QString detail = QString::fromStdString(why);
    status->setText(
          ok && why.empty()             ? tr("OK")
        : ok                            ? tr("OK") + "   " + detail
        : st == S9X_BIOS_PATH_MISSING   ? tr("not found")
        : !why.empty()                  ? detail
        : st == S9X_BIOS_PATH_BAD_IMAGE ? tr("wrong image")
        : tr("unexpected size"));
    status->setStyleSheet(ok ? "color: #27ae60;" : "color: #c0392b;");
}

void BiosManagerDialog::applyAndClose()
{
    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        const std::string path = rows[slot].edit->text().toStdString();
        S9xSetBiosPath(slot, path.c_str());
        app->config->bios_paths[slot] = path;
    }

    accept();
}
