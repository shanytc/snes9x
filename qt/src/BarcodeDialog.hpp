/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#include <string>
#include <vector>

#include "barcodereaders.h"

/* Input > Barcodes > Scan Barcode: the code to scan, typed in, made up, picked
 * from the reader's known cards, or read off a picture (picked or dropped on
 * the dialog) or off a webcam held up to the card. The reader is the one the
 * loaded game's system takes: the Barcode Battler II, the Barcode Boy or
 * Barcode Taisen Bardigun's (barcodereaders.h). The webcam is the one the Game
 * Boy Camera uses; the dialog runs it while open if it isn't already, and puts
 * it back as the settings have it on close. A card's photos show from the
 * reader's folder ("battler" or "barcodeboy") in the config folder or beside
 * the executable (common/desktop/barcode_card_photos.hpp); a code with none is
 * drawn as its barcode. */
class BarcodeDialog : public QDialog
{
    Q_OBJECT

  public:
    BarcodeDialog(QWidget *parent, S9xBarcodeReader reader);
    ~BarcodeDialog();

    // The code Scan was pressed on, for the frontend to hand to the reader.
    std::string code() const { return scanned; }

  private:
    void accept() override;
    void done(int result) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void codeEdited();
    void fromImage();
    void readImage(const QString &filename);
    void toggleCamera();
    void pollCamera();
    void stopCamera();
    void found(const char *code, const QString &where);
    void fillCards();
    void cardPicked(QTreeWidgetItem *item);
    void showCard(const S9xBarcodeReaderCard *card);
    void showPreview();
    void drawBarcode(QPainter &painter, const QSize &box);

    S9xBarcodeReader reader;
    QLineEdit *code_edit;
    QLabel *status_label;
    QComboBox *camera_combo;
    QPushButton *camera_button;
    QLabel *preview_label;
    QPushButton *scan_button;
    QLineEdit *search_edit;
    QTreeWidget *card_tree;
    QTimer camera_timer;
    bool camera_on = false;
    std::vector<unsigned char> frame;
    std::string photo_dir;
    const S9xBarcodeReaderCard *photo_card = nullptr;
    std::vector<QImage> photos;
    std::string whole_code;    // the code typed in when it is whole, drawn with no photos
    std::string scanned;
};
