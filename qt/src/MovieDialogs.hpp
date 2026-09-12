/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>

#include <array>
#include <string>

class EmuApplication;

/* The number of joypads a movie can record: one bit each in the SMV header's
 * controller mask. */
constexpr int kMovieJoypads = 8;

/* File->Movie Play...: win32's "Play Movie" dialog. Shows what the chosen
 * .smv holds (date, length, ROM, author) and whether it matches the loaded
 * ROM before the movie is opened. */
class PlayMovieDialog : public QDialog
{
    Q_OBJECT

  public:
    PlayMovieDialog(EmuApplication *app, QWidget *parent);

    std::string path() const;
    bool readOnly() const;

  private:
    void browse();
    void refreshInfo();

    EmuApplication *app;
    QLineEdit *path_edit;
    QCheckBox *read_only_box;
    QLabel *date_label;
    QLabel *length_label;
    QLabel *frames_label;
    QLabel *rerecord_label;
    QLabel *movie_rom_label;
    QLabel *current_rom_label;
    QLabel *info_title_label;
    QLabel *info_label;
    QLabel *warning_label;
    QRadioButton *from_reset_radio;
    QRadioButton *from_now_radio;
    std::array<QCheckBox *, kMovieJoypads> joypad_boxes{};
    QPushButton *ok_button;
};

/* File->Movie Record...: win32's "Record Movie" dialog. */
class RecordMovieDialog : public QDialog
{
    Q_OBJECT

  public:
    /* sram_exists: whether the loaded game has a battery save on disk, which
     * is what the "Clear SRAM" option would delete. */
    RecordMovieDialog(EmuApplication *app, QWidget *parent, bool sram_exists);

    std::string path() const;
    std::wstring metadata() const;
    uint8_t controllersMask() const;
    bool fromReset() const;
    bool clearSRAM() const;

  private:
    void browse();
    void updateClearSRAM();
    void accept() override;

    EmuApplication *app;
    bool sram_exists;
    QLineEdit *path_edit;
    QLineEdit *metadata_edit;
    QRadioButton *from_reset_radio;
    QRadioButton *from_now_radio;
    QCheckBox *clear_sram_box;
    std::array<QCheckBox *, kMovieJoypads> joypad_boxes{};
};

/* The message for a movie.cpp result code, as win32 words it. */
QString movieErrorString(int result, bool brief);
