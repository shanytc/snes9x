/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "BarcodeBattlerDialog.hpp"
#include "EmuConfig.hpp"
#include "common/desktop/barcode_card_photos.hpp"
#include "common/video/gb_camera_v4l2.hpp"
#include "barcodebattler.h"
#include "display.h"
#include "snes9x.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <string.h>

namespace {

// One printed card: the run of entries from i sharing its set and photo stem
// (a card with several barcodes); a single entry when it has no photos.
int cardEnd(const S9xBarcodeCard *cards, int count, int i)
{
    int end = i + 1;
    if (*cards[i].image)
        while (end < count && !strcmp(cards[end].set, cards[i].set) && !strcmp(cards[end].image, cards[i].image))
            end++;
    return end;
}

bool cardMatches(const S9xBarcodeCard &card, const QString &find)
{
    return find.isEmpty() || QString::fromUtf8(card.name).contains(find, Qt::CaseInsensitive) ||
           QString::fromUtf8(card.set).contains(find, Qt::CaseInsensitive) || QString(card.code).contains(find);
}

// A card with several barcodes is named after them, less their positions.
QString cardLabel(const S9xBarcodeCard *cards, int i, int end)
{
    static const QRegularExpression where(" \\((Up|Down|Left|Right|Front|Back)\\)$");
    QStringList names;
    for (int k = i; k < end; k++)
    {
        const QString name = QString::fromUtf8(cards[k].name).remove(where);
        if (!names.contains(name))
            names << name;
    }
    return names.join(" / ");
}

} // namespace

BarcodeBattlerDialog::BarcodeBattlerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Barcode Battler II"));
    setAcceptDrops(true);

    auto columns = new QHBoxLayout(this);
    auto layout = new QVBoxLayout;
    columns->addLayout(layout, 1);

    auto intro = new QLabel(tr("Swipe a card through the Barcode Battler II on controller port 2. "
                               "Enter its 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
                               "barcode, pick a known card, or read it with a camera or from a picture."));
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

    photo_dir = S9xBarcodePhotoDir({ EmuConfig::findConfigDir(), QCoreApplication::applicationDirPath().toStdString() });

    preview_label = new QLabel;
    preview_label->setMinimumSize(320, 240);
    preview_label->setAlignment(Qt::AlignCenter);
    preview_label->setFrameShape(QFrame::StyledPanel);
    preview_label->setText(tr("Hold the barcode up to the camera, straight and in focus."));
    preview_label->setWordWrap(true);
    preview_label->setVisible(!names.empty() || !photo_dir.empty());
    layout->addWidget(preview_label, 1);

    auto note = new QLabel(tr("Some games look for the Barcode Battler only when they start; "
                              "reset the game if it doesn't respond."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    scan_button = buttons->addButton(tr("&Scan"), QDialogButtonBox::AcceptRole);
    scan_button->setDefault(true);
    layout->addWidget(buttons);

    // The known cards, one branch per set and one per card with several barcodes.
    auto cards_column = new QVBoxLayout;
    auto find_label = new QLabel(tr("Fin&d a card:"));
    search_edit = new QLineEdit;
    search_edit->setPlaceholderText(tr("Name, set or barcode"));
    search_edit->setClearButtonEnabled(true);
    find_label->setBuddy(search_edit);
    card_tree = new QTreeWidget;
    card_tree->setHeaderHidden(true);
    card_tree->setMinimumWidth(300);
    cards_column->addWidget(find_label);
    cards_column->addWidget(search_edit);
    cards_column->addWidget(card_tree, 1);
    columns->addLayout(cards_column);

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
    connect(search_edit, &QLineEdit::textChanged, this, &BarcodeBattlerDialog::fillCards);
    connect(card_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        cardPicked(item);
    });
    // A double-click on a barcode scans it.
    connect(card_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toInt() >= 0)
            accept();
    });

    codeEdited();
    fillCards();
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
    const QByteArray latin = digits.toLatin1();
    const S9xBarcodeCard *card = length_ok ? S9xBarcodeCardFind(latin.constData()) : nullptr;

    scan_button->setEnabled(length_ok);
    if (n == 0)
        status_label->clear();
    else if (!length_ok)
        status_label->setText(tr("%n digit(s); a barcode has 8, 12 or 13.", "", n));
    else if (!S9xBarcodeChecksumValid(latin.constData()))
        status_label->setText(tr("The check digit doesn't add up. A printed barcode would not read like this, "
                                 "but it can still be sent."));
    else if (card)
        status_label->setText(tr("%1, from %2.").arg(QString::fromUtf8(card->name), QString::fromUtf8(card->set)));
    else
        status_label->setText(tr("Valid %1 barcode.").arg(n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));

    showCard(card);
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
    if (!filename.isEmpty())
        readImage(filename);
}

void BarcodeBattlerDialog::readImage(const QString &filename)
{
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

// A picture dropped anywhere on the dialog reads like one picked with From Image.
void BarcodeBattlerDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void BarcodeBattlerDialog::dropEvent(QDropEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return;

    event->acceptProposedAction();
    // After the drop has finished, so a message box doesn't run inside it.
    const QString filename = urls.first().toLocalFile();
    QTimer::singleShot(0, this, [this, filename] { readImage(filename); });
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
    showPhotos();
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

// The known cards with a set, name or code holding the search text; all
// folded when there's nothing to search for. An item's data is the entry for
// a barcode, -1 for a set and -2 - first entry for a card's branch.
void BarcodeBattlerDialog::fillCards()
{
    const QString find = search_edit->text().trimmed();
    card_tree->clear();

    int count = 0;
    const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
    const char *set_name = nullptr;
    QTreeWidgetItem *set = nullptr;
    int in_set = 0;

    auto close_set = [&] {
        if (set)
            set->setText(0, QString("%1 (%2)").arg(QString::fromUtf8(set_name)).arg(in_set));
    };

    for (int i = 0, end; i < count; i = end)
    {
        end = cardEnd(cards, count, i);
        bool hit = false;
        for (int k = i; k < end && !hit; k++)
            hit = cardMatches(cards[k], find);
        if (!hit)
            continue;

        if (!set_name || strcmp(set_name, cards[i].set))
        {
            close_set();
            set_name = cards[i].set;
            set = new QTreeWidgetItem(card_tree, { QString::fromUtf8(set_name) });
            set->setData(0, Qt::UserRole, -1);
            set->setExpanded(!find.isEmpty());
            in_set = 0;
        }

        QTreeWidgetItem *parent = set;
        if (end - i > 1)
        {
            parent = new QTreeWidgetItem(set, { cardLabel(cards, i, end) });
            parent->setData(0, Qt::UserRole, -2 - i);
            parent->setExpanded(!find.isEmpty());
        }

        for (int k = i; k < end; k++)
        {
            auto item = new QTreeWidgetItem(parent, { QString("%1  %2").arg(QString::fromUtf8(cards[k].name), cards[k].code) });
            item->setData(0, Qt::UserRole, k);
        }
        in_set++;
    }
    close_set();
}

// Only a barcode types its code; a set or a card's branch clears it, the
// branch still showing the card's photos.
void BarcodeBattlerDialog::cardPicked(QTreeWidgetItem *item)
{
    if (!item)
        return;

    int count = 0;
    const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
    const int entry = item->data(0, Qt::UserRole).toInt();
    if (entry >= 0 && entry < count)
    {
        code_edit->setText(cards[entry].code);
        showCard(&cards[entry]);    // this set's printing, not the first card with the code
        return;
    }

    code_edit->clear();
    const int first = -2 - entry;
    showCard(first >= 0 && first < count ? &cards[first] : nullptr);
}

void BarcodeBattlerDialog::showCard(const S9xBarcodeCard *card)
{
    if (card == photo_card)
        return;

    photo_card = card;
    photos.clear();
    if (card)
        for (auto &photo : S9xBarcodeCardPhotos(photo_dir, *card))
            photos.push_back(QImage(photo.rgba.data(), photo.width, photo.height, photo.width * 4,
                                    QImage::Format_RGBA8888).copy());
    showPhotos();
}

// The shown card's photos side by side in the preview, while the camera is off.
void BarcodeBattlerDialog::showPhotos()
{
    if (camera_on)
        return;

    if (photos.empty())
    {
        preview_label->setText(tr("Hold the barcode up to the camera, straight and in focus."));
        return;
    }

    const QSize box = preview_label->contentsRect().size();
    const qreal ratio = devicePixelRatioF();
    QPixmap canvas(box * ratio);
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const int n = (int)photos.size(), gap = 6;
    const int slot_w = (box.width() - gap * (n + 1)) / n, slot_h = box.height() - gap * 2;
    for (int i = 0; i < n; i++)
    {
        const QSize size = photos[i].size().scaled(slot_w, slot_h, Qt::KeepAspectRatio);
        const QPoint at(gap + i * (slot_w + gap) + (slot_w - size.width()) / 2, (box.height() - size.height()) / 2);
        painter.drawImage(QRect(at, size), photos[i]);
    }
    painter.end();
    preview_label->setPixmap(canvas);
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
