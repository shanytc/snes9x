/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#include <functional>
#include <string>
#include <vector>

struct S9xBarcodeCard;

/* Input > Barcodes > Scan Barcode: the code to swipe, typed in,
 * made up, picked from the known cards, or read off a picture (picked or
 * dropped on the dialog) or off a webcam held up to the card. Scan hands it
 * to the BBII interface on port 2 (barcodebattler.h). The webcam is the one
 * Options > Display turns on and picks for the Game Boy Camera; the dialog
 * only reads it. A card's photos show from a "battler" folder in the config folder or beside the
 * executable (common/desktop/barcode_card_photos.hpp). Modeless: Scan leaves
 * it open, the game running on behind it. */
class BarcodeBattlerDialog : public QDialog
{
    Q_OBJECT

  public:
    // game_reads: whether the running game reads the Barcode Battler now.
    BarcodeBattlerDialog(QWidget *parent, std::function<bool()> game_reads);
    ~BarcodeBattlerDialog();

  private:
    void accept() override;
    void done(int result) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;
    void codeEdited();
    void fromImage();
    void readImage(const QString &filename);
    void showCameraChoice();
    void toggleCamera();
    void pollCamera();
    void stopCamera();
    void found(const char *code, const QString &where);
    void fillCards();
    void cardPicked(QTreeWidgetItem *item);
    void showCard(const S9xBarcodeCard *card);
    void showPhotos();

    QLineEdit *code_edit;
    QLabel *status_label;
    QLabel *camera_label;
    QPushButton *camera_button;
    QLabel *preview_label;
    QPushButton *scan_button;
    QLineEdit *search_edit;
    QTreeWidget *card_tree;
    QTimer camera_timer;
    std::function<bool()> game_reads;
    bool camera_on = false;
    std::vector<unsigned char> frame;
    std::string photo_dir;
    const S9xBarcodeCard *photo_card = nullptr;
    std::vector<QImage> photos;
};
