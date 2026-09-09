/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "MovieDialogs.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

#include "snes9x.h"
#include "memmap.h"
#include "movie.h"
#include "snapshot.h"
#include "fscompat.h"

QString movieErrorString(int result, bool brief)
{
    switch (result)
    {
    case FILE_NOT_FOUND:
        return brief ? QObject::tr("File not found.")
                     : QObject::tr("The movie file was not found or could not be opened.");
    case WRONG_FORMAT:
        return brief ? QObject::tr("Unrecognized format.")
                     : QObject::tr("The movie file is corrupt or in the wrong format.");
    case WRONG_VERSION:
        return brief ? QObject::tr("Unsupported movie version.")
                     : QObject::tr("Unsupported movie version. You need a different version of SuperSnes9x to play this movie.");
    default:
        return QObject::tr("Could not open movie file.");
    }
}

/* <ROM name>.smv in the export folder, the default both dialogs start from. */
static QString defaultMoviePath()
{
    if (Memory.ROMFilename.empty())
        return {};
    return QString::fromStdString(S9xGetFilename(".smv", SCREENSHOT_DIR));
}

static QString romDescription(uint32_t crc32, const char *name)
{
    return QObject::tr("crc32=%1, name=%2")
        .arg(QString::number(crc32, 16).rightJustified(8, '0').toUpper())
        .arg(QString::fromLatin1(name).trimmed());
}

static QGroupBox *makeJoypadGroup(std::array<QCheckBox *, kMovieJoypads> &boxes, bool enabled)
{
    auto group = new QGroupBox(QObject::tr("Record Controllers"));
    auto grid = new QGridLayout(group);
    for (int i = 0; i < kMovieJoypads; i++)
    {
        boxes[i] = new QCheckBox(QObject::tr("Joypad %1").arg(i + 1));
        boxes[i]->setEnabled(enabled);
        grid->addWidget(boxes[i], i / 4, i % 4);
    }
    return group;
}

PlayMovieDialog::PlayMovieDialog(EmuApplication *app, QWidget *parent)
    : QDialog(parent), app(app)
{
    setWindowTitle(tr("Play Movie"));

    auto layout = new QVBoxLayout(this);

    auto path_row = new QHBoxLayout;
    path_row->addWidget(new QLabel(tr("Movie File:")));
    path_edit = new QLineEdit;
    path_edit->setMinimumWidth(320);
    path_row->addWidget(path_edit, 1);
    auto browse_button = new QPushButton(tr("&Browse..."));
    path_row->addWidget(browse_button);
    layout->addLayout(path_row);

    read_only_box = new QCheckBox(tr("Open Read-Only"));
    read_only_box->setChecked(app->config->movie_default_read_only);
    layout->addWidget(read_only_box);

    movie_rom_label = new QLabel;
    current_rom_label = new QLabel;
    layout->addWidget(movie_rom_label);
    layout->addWidget(current_rom_label);

    auto middle = new QHBoxLayout;

    auto info_grid = new QGridLayout;
    const std::pair<QString, QLabel **> rows[] = {
        { tr("Recording Date:"), &date_label },
        { tr("Length:"), &length_label },
        { tr("Frames:"), &frames_label },
        { tr("Re-record Count:"), &rerecord_label },
    };
    int row = 0;
    for (auto &[name, label] : rows)
    {
        auto title = new QLabel(name);
        title->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        *label = new QLabel;
        (*label)->setMinimumWidth((*label)->fontMetrics().horizontalAdvance("0000-00-00 00:00:00 "));
        info_grid->addWidget(title, row, 0);
        info_grid->addWidget(*label, row, 1);
        row++;
    }
    middle->addLayout(info_grid, 1);

    // How the movie was recorded: shown for information, as on win32.
    auto options_group = new QGroupBox(tr("Record Options"));
    auto options_layout = new QVBoxLayout(options_group);
    from_reset_radio = new QRadioButton(tr("Record from reset"));
    from_now_radio = new QRadioButton(tr("Record from now"));
    from_reset_radio->setEnabled(false);
    from_now_radio->setEnabled(false);
    options_layout->addWidget(from_reset_radio);
    options_layout->addWidget(from_now_radio);
    middle->addWidget(options_group);

    layout->addLayout(middle);

    layout->addWidget(makeJoypadGroup(joypad_boxes, false));

    auto info_row = new QHBoxLayout;
    info_title_label = new QLabel(tr("Author Info:"));
    info_title_label->setAlignment(Qt::AlignRight | Qt::AlignTop);
    info_label = new QLabel;
    info_label->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    info_label->setWordWrap(true);
    info_label->setMinimumHeight(info_label->fontMetrics().height() * 3);
    info_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_row->addWidget(info_title_label);
    info_row->addWidget(info_label, 1);
    layout->addLayout(info_row);

    auto bottom = new QHBoxLayout;
    warning_label = new QLabel;
    bottom->addWidget(warning_label, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_button = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    connect(browse_button, &QPushButton::clicked, this, &PlayMovieDialog::browse);
    connect(path_edit, &QLineEdit::textChanged, this, &PlayMovieDialog::refreshInfo);

    path_edit->setText(defaultMoviePath());
    refreshInfo();
}

std::string PlayMovieDialog::path() const
{
    return path_edit->text().toStdString();
}

bool PlayMovieDialog::readOnly() const
{
    return read_only_box->isChecked();
}

void PlayMovieDialog::browse()
{
    QFileInfo current(path_edit->text());
    QString start = current.dir().exists() ? current.dir().path()
                                           : QString::fromStdString(S9xGetDirectory(SCREENSHOT_DIR));

    QFileDialog dialog(this, tr("Open Movie"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(start);
    dialog.setNameFilters({ tr("SuperSnes9x Movie Files (*.smv)"), tr("All Files (*)") });
    if (dialog.exec() && !dialog.selectedFiles().empty())
        path_edit->setText(dialog.selectedFiles()[0]);
}

void PlayMovieDialog::refreshInfo()
{
    MovieInfo info{};
    int result = FILE_NOT_FOUND;
    auto path = path_edit->text().toStdString();
    if (!path.empty())
        result = S9xMovieGetInfo(path.c_str(), &info);

    const bool header_ok = result != FILE_NOT_FOUND;

    if (header_ok)
    {
        date_label->setText(QDateTime::fromSecsSinceEpoch(info.TimeCreated).toString(Qt::TextDate));

        uint32_t fps = Memory.ROMFramesPerSecond ? Memory.ROMFramesPerSecond : 60;
        uint32_t seconds = (info.LengthFrames + fps / 2) / fps;
        length_label->setText(QString("%1:%2:%3")
                                  .arg(seconds / 3600, 2, 10, QChar('0'))
                                  .arg((seconds / 60) % 60, 2, 10, QChar('0'))
                                  .arg(seconds % 60, 2, 10, QChar('0')));
        frames_label->setText(QString::number(info.LengthFrames));
        rerecord_label->setText(QString::number(info.RerecordCount));
    }
    else
    {
        date_label->clear();
        length_label->clear();
        frames_label->clear();
        rerecord_label->clear();
    }

    current_rom_label->setText(tr("Current ROM: %1").arg(romDescription(Memory.ROMCRC32, Memory.ROMName)));

    if (result == SUCCESS)
    {
        info_title_label->setText(tr("Author Info:"));
        info_label->setText(QString::fromWCharArray(info.Metadata).trimmed());

        // A movie that was saved read-only cannot be recorded into.
        read_only_box->setEnabled(!info.ReadOnly);
        if (info.ReadOnly)
            read_only_box->setChecked(true);

        for (int i = 0; i < kMovieJoypads; i++)
            joypad_boxes[i]->setChecked(info.ControllersMask & (1 << i));
        from_reset_radio->setChecked(info.Opts & MOVIE_OPT_FROM_RESET);
        from_now_radio->setChecked(!(info.Opts & MOVIE_OPT_FROM_RESET));

        const bool has_rom_info = info.SyncFlags & MOVIE_SYNC_HASROMINFO;
        if (has_rom_info)
            movie_rom_label->setText(tr("Movie's ROM: %1").arg(romDescription(info.ROMCRC32, info.ROMName)));
        else
            movie_rom_label->setText(tr("Movie's ROM: (not stored in movie file)"));

        const bool mismatch = has_rom_info && info.ROMCRC32 != Memory.ROMCRC32;
        if (mismatch)
        {
            current_rom_label->setText(current_rom_label->text() + tr(" <-- MISMATCH !!!"));
            warning_label->setText(tr("WARNING: You don't have the right ROM loaded!"));
        }
        else
            warning_label->setText(tr("Press OK to start playing the movie."));

        ok_button->setEnabled(true);
    }
    else
    {
        info_title_label->setText(tr("Error Info:"));
        info_label->setText(path.empty() ? QString() : movieErrorString(result, false));
        warning_label->setText(path.empty() ? QString() : movieErrorString(result, true));

        read_only_box->setEnabled(false);
        for (auto box : joypad_boxes)
            box->setChecked(false);
        from_reset_radio->setChecked(false);
        from_now_radio->setChecked(false);

        // Show where the movie was looked for, as win32 does.
        QFileInfo file(QString::fromStdString(path));
        movie_rom_label->setText(tr("Path: %1").arg(file.dir().absolutePath()));

        ok_button->setEnabled(false);
    }
}

RecordMovieDialog::RecordMovieDialog(EmuApplication *app, QWidget *parent, bool sram_exists)
    : QDialog(parent), app(app), sram_exists(sram_exists)
{
    setWindowTitle(tr("Record Movie"));

    auto layout = new QVBoxLayout(this);

    auto grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Movie File:")), 0, 0);
    path_edit = new QLineEdit;
    path_edit->setMinimumWidth(320);
    grid->addWidget(path_edit, 0, 1);
    auto browse_button = new QPushButton(tr("&Browse..."));
    grid->addWidget(browse_button, 0, 2);

    grid->addWidget(new QLabel(tr("Author Info:")), 1, 0);
    metadata_edit = new QLineEdit;
    metadata_edit->setMaxLength(MOVIE_MAX_METADATA - 1);
    grid->addWidget(metadata_edit, 1, 1, 1, 2);
    layout->addLayout(grid);

    auto middle = new QHBoxLayout;

    auto options_group = new QGroupBox(tr("Record Options"));
    auto options_layout = new QVBoxLayout(options_group);
    from_reset_radio = new QRadioButton(tr("Record from reset"));
    from_now_radio = new QRadioButton(tr("Record from now"));
    clear_sram_box = new QCheckBox(tr("Clear SRAM"));
    options_layout->addWidget(from_reset_radio);
    options_layout->addWidget(from_now_radio);
    options_layout->addWidget(clear_sram_box);
    middle->addWidget(options_group);

    middle->addWidget(makeJoypadGroup(joypad_boxes, true), 1);
    layout->addLayout(middle);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(browse_button, &QPushButton::clicked, this, &RecordMovieDialog::browse);
    connect(from_reset_radio, &QRadioButton::toggled, this, &RecordMovieDialog::updateClearSRAM);

    path_edit->setText(defaultMoviePath());
    joypad_boxes[0]->setChecked(true);
    from_reset_radio->setChecked(app->config->movie_default_from_reset);
    from_now_radio->setChecked(!app->config->movie_default_from_reset);
    // Without a battery save there is nothing to clear, so the box is shown
    // checked and disabled, as on win32.
    clear_sram_box->setChecked(sram_exists ? app->config->movie_default_clear_sram : true);
    updateClearSRAM();
}

std::string RecordMovieDialog::path() const
{
    return path_edit->text().toStdString();
}

std::wstring RecordMovieDialog::metadata() const
{
    return metadata_edit->text().toStdWString();
}

uint8_t RecordMovieDialog::controllersMask() const
{
    uint8_t mask = 0;
    for (int i = 0; i < kMovieJoypads; i++)
        if (joypad_boxes[i]->isChecked())
            mask |= 1 << i;
    return mask;
}

bool RecordMovieDialog::fromReset() const
{
    return from_reset_radio->isChecked();
}

bool RecordMovieDialog::clearSRAM() const
{
    return sram_exists && fromReset() && clear_sram_box->isChecked();
}

void RecordMovieDialog::browse()
{
    QFileInfo current(path_edit->text());
    QString start = current.dir().exists() ? current.dir().path()
                                           : QString::fromStdString(S9xGetDirectory(SCREENSHOT_DIR));

    QFileDialog dialog(this, tr("Record Movie"));
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("smv");
    dialog.setDirectory(start);
    if (!current.fileName().isEmpty())
        dialog.selectFile(current.fileName());
    dialog.setNameFilters({ tr("SuperSnes9x Movie Files (*.smv)"), tr("All Files (*)") });
    if (dialog.exec() && !dialog.selectedFiles().empty())
        path_edit->setText(dialog.selectedFiles()[0]);
}

void RecordMovieDialog::updateClearSRAM()
{
    clear_sram_box->setEnabled(sram_exists && from_reset_radio->isChecked());
}

void RecordMovieDialog::accept()
{
    if (path_edit->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, tr("Record Movie"), tr("Choose a file name for the movie."));
        return;
    }
    if (controllersMask() == 0)
    {
        QMessageBox::warning(this, tr("Record Movie"), tr("Select at least one controller to record."));
        return;
    }

    // Remember the choices for next time, like win32's MovieDefault* settings.
    app->config->movie_default_from_reset = fromReset();
    if (sram_exists && fromReset())
        app->config->movie_default_clear_sram = clear_sram_box->isChecked();

    QDialog::accept();
}
