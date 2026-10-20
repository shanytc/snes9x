/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "BarcodeBattlerDialog.hpp"
#include "common/video/gb_camera_v4l2.hpp"
#include "barcodebattler.h"
#include "display.h"
#include "snes9x.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QMessageBox>
#include <QPixmap>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

BarcodeBattlerDialog::BarcodeBattlerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Barcode Battler II"));

    auto layout = new QVBoxLayout(this);

    auto intro = new QLabel(tr("Swipe a card through the Barcode Battler II on controller port 2. "
                               "Enter its 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
                               "barcode, or read it with a camera or from a picture."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto code_row = new QHBoxLayout;
    code_row->addWidget(new QLabel(tr("Barcode:")));
    code_edit = new QLineEdit;
    code_edit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9 -]{0,20}"), code_edit));
    code_edit->setPlaceholderText(tr("e.g. 4901234567894"));
    code_edit->setText(S9xBarcodeBattlerLastCode());
    code_row->addWidget(code_edit, 1);
    auto random_button = new QPushButton(tr("&Random"));
    code_row->addWidget(random_button);
    auto image_button = new QPushButton(tr("From &Image..."));
    code_row->addWidget(image_button);
    layout->addLayout(code_row);

    status_label = new QLabel;
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    auto camera_row = new QHBoxLayout;
    camera_row->addWidget(new QLabel(tr("Camera:")));
    camera_combo = new QComboBox;
    camera_combo->setPlaceholderText(tr("No video camera found"));
    std::vector<std::string> names;
    S9xGBCameraEnumerate(names);
    for (auto &name : names)
        camera_combo->addItem(QString::fromStdString(name));
    if (!names.empty())
        camera_combo->setCurrentIndex(Settings.GBVideoCameraIndex < names.size() ? Settings.GBVideoCameraIndex : 0);
    camera_row->addWidget(camera_combo, 1);
    camera_button = new QPushButton(tr("Start &Camera"));
    camera_button->setEnabled(!names.empty());
    camera_combo->setEnabled(!names.empty());
    camera_row->addWidget(camera_button);
    layout->addLayout(camera_row);

    preview_label = new QLabel;
    preview_label->setMinimumSize(320, 240);
    preview_label->setAlignment(Qt::AlignCenter);
    preview_label->setFrameShape(QFrame::StyledPanel);
    preview_label->setText(tr("Hold the barcode up to the camera, straight and in focus."));
    preview_label->setWordWrap(true);
    preview_label->setVisible(!names.empty());
    layout->addWidget(preview_label, 1);

    auto note = new QLabel(tr("Some games look for the Barcode Battler only when they start; "
                              "reset the game if it doesn't respond."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    scan_button = buttons->addButton(tr("&Scan"), QDialogButtonBox::AcceptRole);
    scan_button->setDefault(true);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(code_edit, &QLineEdit::textChanged, this, &BarcodeBattlerDialog::codeEdited);
    connect(random_button, &QPushButton::clicked, [&] {
        char code[14];
        S9xBarcodeRandom(code);
        code_edit->setText(code);
    });
    connect(image_button, &QPushButton::clicked, this, &BarcodeBattlerDialog::fromImage);
    connect(camera_button, &QPushButton::clicked, this, &BarcodeBattlerDialog::toggleCamera);
    connect(camera_combo, &QComboBox::activated, [&] {
        if (camera_on)
        {
            stopCamera();
            toggleCamera();
        }
    });
    connect(&camera_timer, &QTimer::timeout, this, &BarcodeBattlerDialog::pollCamera);

    codeEdited();
}

BarcodeBattlerDialog::~BarcodeBattlerDialog()
{
    stopCamera();
}

void BarcodeBattlerDialog::codeEdited()
{
    QString digits = code_edit->text();
    digits.remove(' ').remove('-');
    const int n = digits.size();
    const bool length_ok = n == 8 || n == 12 || n == 13;

    scan_button->setEnabled(length_ok);
    if (n == 0)
        status_label->clear();
    else if (!length_ok)
        status_label->setText(tr("%n digit(s); a barcode has 8, 12 or 13.", "", n));
    else if (!S9xBarcodeChecksumValid(digits.toLatin1().constData()))
        status_label->setText(tr("The check digit doesn't add up. A printed barcode would not read like this, "
                                 "but it can still be sent."));
    else
        status_label->setText(tr("Valid %1 barcode.").arg(n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));
}

void BarcodeBattlerDialog::found(const char *code, const QString &where)
{
    code_edit->setText(code);
    status_label->setText(tr("Read %1 from the %2.").arg(code, where));
}

void BarcodeBattlerDialog::fromImage()
{
    auto filename = QFileDialog::getOpenFileName(this, tr("Read Barcode from Image"), QString(),
                                                 tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All Files (*)"));
    if (filename.isEmpty())
        return;

    QImage image(filename);
    if (image.isNull())
    {
        QMessageBox::warning(this, tr("Barcode Battler II"), tr("This picture could not be opened."));
        return;
    }

    image = image.convertToFormat(QImage::Format_Grayscale8);
    char code[14];
    if (S9xBarcodeDecode(image.constBits(), image.width(), image.height(), image.bytesPerLine(), code))
        found(code, tr("picture"));
    else
        QMessageBox::information(this, tr("Barcode Battler II"),
                                 tr("No EAN-13, UPC-A or EAN-8 barcode was found in this picture. "
                                    "Crop it closer to the barcode and keep the bars upright."));
}

void BarcodeBattlerDialog::toggleCamera()
{
    if (camera_on)
    {
        stopCamera();
        return;
    }

    if (!S9xGBCameraStart(camera_combo->currentIndex()))
    {
        status_label->setText(tr("The camera could not be started."));
        return;
    }

    camera_on = true;
    camera_button->setText(tr("Stop &Camera"));
    preview_label->setText(tr("Waiting for the camera..."));
    camera_timer.start(100);
}

void BarcodeBattlerDialog::stopCamera()
{
    if (!camera_on)
        return;

    camera_on = false;
    camera_timer.stop();
    camera_button->setText(tr("Start &Camera"));
    // Back to what Display > Game Boy Image asks for.
    S9xGBCameraStop();
    S9xGBCameraApply();
}

void BarcodeBattlerDialog::pollCamera()
{
    int width = 0, height = 0;
    if (!S9xGBCameraGetFrame(frame, width, height))
        return;

    // Mirrored, as a webcam preview is expected to look. The decoder reads
    // both directions, so it gets the frame as captured.
    QImage image(frame.data(), width, height, width, QImage::Format_Grayscale8);
    preview_label->setPixmap(QPixmap::fromImage(image.mirrored(true, false))
                                 .scaled(preview_label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    char code[14];
    if (S9xBarcodeDecode(frame.data(), width, height, width, code) && code_edit->text() != code)
    {
        found(code, tr("camera"));
        QApplication::beep();
    }
}

void BarcodeBattlerDialog::done(int result)
{
    stopCamera();
    QDialog::done(result);
}

void BarcodeBattlerDialog::accept()
{
    const QByteArray code = code_edit->text().toLatin1();
    if (!S9xBarcodeBattlerScan(code.constData()))
        return;

    S9xSetInfoString(tr("Barcode Battler: %1 scanned").arg(S9xBarcodeBattlerLastCode()).toUtf8().constData());
    QDialog::accept();
}
