/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include <vector>

/* Input > Barcode Battler II > Scan Barcode: the code to swipe, typed in,
 * made up, read off a picture or off a webcam held up to the card. Scan hands
 * it to the BBII interface on port 2 (barcodebattler.h). The webcam is the
 * one the Game Boy Camera uses; the dialog runs it while open if it isn't
 * already, and puts it back as the settings have it on close. */
class BarcodeBattlerDialog : public QDialog
{
    Q_OBJECT

  public:
    BarcodeBattlerDialog(QWidget *parent);
    ~BarcodeBattlerDialog();

  private:
    void accept() override;
    void done(int result) override;
    void codeEdited();
    void fromImage();
    void toggleCamera();
    void pollCamera();
    void stopCamera();
    void found(const char *code, const QString &where);

    QLineEdit *code_edit;
    QLabel *status_label;
    QComboBox *camera_combo;
    QPushButton *camera_button;
    QLabel *preview_label;
    QPushButton *scan_button;
    QTimer camera_timer;
    bool camera_on = false;
    std::vector<unsigned char> frame;
};
