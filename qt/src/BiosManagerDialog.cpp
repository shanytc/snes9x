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

#include "snes9x.h"
#include "memmap.h"

BiosManagerDialog::BiosManagerDialog(QWidget *parent, EmuApplication *app)
    : QDialog(parent), app(app)
{
    setWindowTitle(tr("BIOS Manager"));

    auto outer = new QVBoxLayout(this);

    auto intro = new QLabel(tr("Point each entry at its BIOS file. Anything left blank "
                               "falls back to the usual search by filename in the BIOS folder."));
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto grid = new QGridLayout();
    grid->setColumnStretch(1, 1);
    outer->addLayout(grid);

    for (int slot = 0; slot < S9X_NUM_BIOS_SLOTS; slot++)
    {
        const auto *info = S9xGetBiosSlotInfo(slot);

        auto edit = new QLineEdit(QString::fromUtf8(S9xGetBiosPath(slot)));
        edit->setPlaceholderText(QString::fromUtf8(info->filename));
        edit->setMinimumWidth(340);
        auto status = new QLabel();

        auto select = new QPushButton(tr("Select..."));
        auto clear = new QPushButton(QStringLiteral("✕"));
        clear->setToolTip(tr("Clear"));
        clear->setFixedWidth(clear->fontMetrics().height() * 2);

        const int r = slot;
        grid->addWidget(new QLabel(QString::fromUtf8(info->label)), r, 0);
        grid->addWidget(edit, r, 1);
        grid->addWidget(select, r, 2);
        grid->addWidget(clear, r, 3);
        grid->addWidget(status, r, 4);

        connect(select, &QPushButton::clicked, this, [this, slot] { browse(slot); });
        connect(clear, &QPushButton::clicked, this, [this, slot] {
            rows[slot].edit->clear();
            refreshRow(slot);
        });
        connect(edit, &QLineEdit::textChanged, this, [this, slot] { refreshRow(slot); });

        rows.push_back({ edit, status });
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
        tr("BIOS files (*.zip *.bin *.sfc *.gb *.gbc *.BIN);;All files (*)"));

    if (!file.isEmpty())
        rows[slot].edit->setText(file);
}

void BiosManagerDialog::refreshRow(int slot)
{
    // Validate against the live text, not the stored path, so typing shows up.
    const QString text = rows[slot].edit->text();
    QLabel *status = rows[slot].status;

    if (text.isEmpty())
    {
        status->clear();
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
    const bool ok = S9xBiosPathUsable(slot);
    S9xSetBiosPath(slot, keep.c_str());

    status->setText(ok ? tr("OK") : tr("unexpected size"));
    status->setStyleSheet(ok ? "color: #27ae60;" : "color: #d68910;");
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
