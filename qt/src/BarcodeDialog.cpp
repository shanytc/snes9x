/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "BarcodeDialog.hpp"
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

#include <algorithm>
#include <string.h>

namespace {

// One printed card: the run of entries from i sharing its branch and photo
// stem (a card with several barcodes); a single entry when it has no photos.
int cardEnd(const S9xBarcodeReaderCard *cards, int count, int i)
{
    int end = i + 1;
    if (*cards[i].image)
        while (end < count && !strcmp(cards[end].branch, cards[i].branch) && !strcmp(cards[end].image, cards[i].image))
            end++;
    return end;
}

bool cardMatches(const S9xBarcodeReaderCard &card, const QString &find)
{
    return find.isEmpty() || QString::fromUtf8(card.name).contains(find, Qt::CaseInsensitive) ||
           QString::fromUtf8(card.kana).contains(find) ||
           QString::fromUtf8(card.branch).contains(find, Qt::CaseInsensitive) || QString(card.code).contains(find) ||
           (card.group && QString::fromUtf8(card.group).contains(find, Qt::CaseInsensitive));
}

// A card with several barcodes is named after them, less their positions.
QString cardLabel(const S9xBarcodeReaderCard *cards, int i, int end)
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

BarcodeDialog::BarcodeDialog(QWidget *parent, S9xBarcodeReader reader)
    : QDialog(parent), reader(reader)
{
    const bool battler = reader == S9X_BARCODE_BATTLER;
    setWindowTitle(QString::fromUtf8(S9xBarcodeReaderName(reader)));
    setAcceptDrops(true);

    auto columns = new QHBoxLayout(this);
    auto layout = new QVBoxLayout;
    columns->addLayout(layout, 1);

    auto intro = new QLabel(battler ? tr("Enter a card's 13-digit (EAN-13), 12-digit (UPC-A) or 8-digit (EAN-8) "
                                         "barcode, pick a known card, or read it with a camera or from a picture.")
                            : reader == S9X_BARCODE_BARCODEBOY
                                ? tr("Enter a card's 13-digit (JAN/EAN-13) barcode, pick a known card, or read it "
                                     "with a camera or from a picture.")
                                : tr("Enter a card's 13-digit (JAN/EAN-13) or 8-digit (EAN-8) barcode, pick a known "
                                     "card, or read it with a camera or from a picture. Shop barcodes work too."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto code_row = new QHBoxLayout;
    code_row->addWidget(new QLabel(tr("Barcode:")));
    code_edit = new QLineEdit;
    code_edit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9 -]{0,20}"), code_edit));
    code_edit->setPlaceholderText(tr("e.g. 4901234567894"));
    code_edit->setText(S9xBarcodeReaderLastCode(reader));
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

    photo_dir = S9xBarcodePhotoDir({ EmuConfig::findConfigDir(), QCoreApplication::applicationDirPath().toStdString() },
                                   S9xBarcodeReaderPhotoFolder(reader));

    preview_label = new QLabel;
    preview_label->setMinimumSize(320, 240);
    preview_label->setAlignment(Qt::AlignCenter);
    preview_label->setFrameShape(QFrame::StyledPanel);
    preview_label->setWordWrap(true);
    layout->addWidget(preview_label, 1);

    auto note = new QLabel(battler ? tr("Some games look for the Barcode Battler only when they start; "
                                        "reset the game if it doesn't respond.")
                                   : tr("The game reads the card the next time it asks for one."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    scan_button = buttons->addButton(tr("&Scan"), QDialogButtonBox::AcceptRole);
    scan_button->setDefault(true);
    layout->addWidget(buttons);

    // The known cards, one branch per card set or game, per group within it
    // and per card with several barcodes.
    auto cards_column = new QVBoxLayout;
    auto find_label = new QLabel(tr("Fin&d a card:"));
    search_edit = new QLineEdit;
    search_edit->setPlaceholderText(battler ? tr("Name, set or barcode") : tr("Name, game or barcode"));
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
    connect(code_edit, &QLineEdit::textChanged, this, &BarcodeDialog::codeEdited);
    connect(random_button, &QPushButton::clicked, [this] {
        // Any 13 digits work; the Game Boy games' made-up codes carry JAN's
        // old flag 49, as most of their cards do.
        char code[14];
        S9xBarcodeRandom(code);
        if (this->reader != S9X_BARCODE_BATTLER)
        {
            code[0] = '4';
            code[1] = '9';
            int sum = 0;
            for (int i = 0; i < 12; i++)
                sum += (code[i] - '0') * ((i & 1) ? 3 : 1);
            code[12] = (char)('0' + (10 - sum % 10) % 10);
        }
        code_edit->setText(code);
    });
    connect(image_button, &QPushButton::clicked, this, &BarcodeDialog::fromImage);
    connect(camera_button, &QPushButton::clicked, this, &BarcodeDialog::toggleCamera);
    connect(camera_combo, &QComboBox::activated, [&] {
        if (camera_on)
        {
            stopCamera();
            toggleCamera();
        }
    });
    connect(&camera_timer, &QTimer::timeout, this, &BarcodeDialog::pollCamera);
    connect(search_edit, &QLineEdit::textChanged, this, &BarcodeDialog::fillCards);
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

BarcodeDialog::~BarcodeDialog()
{
    stopCamera();
}

void BarcodeDialog::codeEdited()
{
    QString digits = code_edit->text();
    digits.remove(' ').remove('-');
    const int n = digits.size();
    const bool length_ok = S9xBarcodeReaderTakes(reader, n);
    const QByteArray latin = digits.toLatin1();
    const S9xBarcodeReaderCard *card = length_ok ? S9xBarcodeReaderCardFind(reader, latin.constData()) : nullptr;

    scan_button->setEnabled(length_ok);
    if (n == 0)
        status_label->clear();
    else if (!length_ok)
        status_label->setText(reader == S9X_BARCODE_BATTLER    ? tr("%n digit(s); a barcode has 8, 12 or 13.", "", n)
                              : reader == S9X_BARCODE_BARCODEBOY ? tr("%n digit(s); a Barcode Boy card has 13.", "", n)
                                                                 : tr("%n digit(s); a barcode has 13, or 8 for EAN-8.", "", n));
    else if (!S9xBarcodeChecksumValid(latin.constData()))
        status_label->setText(tr("The check digit doesn't add up. A printed barcode would not read like this, "
                                 "but it can still be sent."));
    else if (card)
    {
        QString name = QString::fromUtf8(card->name);
        if (*card->kana)
            name += "  " + QString::fromUtf8(card->kana);
        status_label->setText(tr("%1, from %2.").arg(name, QString::fromUtf8(card->branch)));
    }
    else if (n == 8 && reader == S9X_BARCODE_BARDIGUN)
        status_label->setText(tr("Valid EAN-8 barcode. It reads, but it is too weak to hatch an egg."));
    else
        status_label->setText(tr("Valid %1 barcode.").arg(n == 13 ? "EAN-13" : n == 12 ? "UPC-A" : "EAN-8"));

    showCard(card);
    const std::string whole = length_ok ? latin.toStdString() : std::string();
    if (whole != whole_code)
    {
        whole_code = whole;
        showPreview();
    }
}

void BarcodeDialog::found(const char *code, const QString &where)
{
    code_edit->setText(code);
    status_label->setText(tr("Read %1 from the %2.").arg(code, where));
}

void BarcodeDialog::fromImage()
{
    auto filename = QFileDialog::getOpenFileName(this, tr("Read Barcode from Image"), QString(),
                                                 tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All Files (*)"));
    if (!filename.isEmpty())
        readImage(filename);
}

void BarcodeDialog::readImage(const QString &filename)
{
    const QString title = QString::fromUtf8(S9xBarcodeReaderName(reader));
    QImage image(filename);
    if (image.isNull())
    {
        QMessageBox::warning(this, title, tr("This picture could not be opened."));
        return;
    }

    image = image.convertToFormat(QImage::Format_Grayscale8);
    char code[14];
    if (S9xBarcodeDecode(image.constBits(), image.width(), image.height(), image.bytesPerLine(), code))
        found(code, tr("picture"));
    else
        QMessageBox::information(this, title,
                                 tr("No EAN-13, UPC-A or EAN-8 barcode was found in this picture. "
                                    "Crop it closer to the barcode and keep the bars upright."));
}

// A picture dropped anywhere on the dialog reads like one picked with From Image.
void BarcodeDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void BarcodeDialog::dropEvent(QDropEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return;

    event->acceptProposedAction();
    // After the drop has finished, so a message box doesn't run inside it.
    const QString filename = urls.first().toLocalFile();
    QTimer::singleShot(0, this, [this, filename] { readImage(filename); });
}

// The preview is drawn to its box's size.
void BarcodeDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showPreview();
}

void BarcodeDialog::toggleCamera()
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

void BarcodeDialog::stopCamera()
{
    if (!camera_on)
        return;

    camera_on = false;
    camera_timer.stop();
    camera_button->setText(tr("Start &Camera"));
    // Back to what Display > Game Boy Image asks for.
    S9xGBCameraStop();
    S9xGBCameraApply();
    showPreview();
}

void BarcodeDialog::pollCamera()
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

// The reader's known cards with a branch, group, name or code holding the
// search text. With nothing to search for, only the loaded game's branch is
// open. An item's data is the entry for a barcode, -1 for a branch or group
// and -2 - first entry for a card's branch.
void BarcodeDialog::fillCards()
{
    const QString find = search_edit->text().trimmed();
    const char *open = S9xBarcodeReaderOpenBranch(reader);
    card_tree->clear();

    int count = 0;
    const S9xBarcodeReaderCard *cards = S9xBarcodeReaderCards(reader, &count);
    const char *branch_name = nullptr, *group_name = nullptr;
    QTreeWidgetItem *branch = nullptr, *group = nullptr;
    int in_branch = 0, in_group = 0;

    auto close_group = [&] {
        if (group)
            group->setText(0, QString("%1 (%2)").arg(QString::fromUtf8(group_name)).arg(in_group));
        group = nullptr;
    };
    auto close_branch = [&] {
        close_group();
        if (branch)
            branch->setText(0, QString("%1 (%2)").arg(QString::fromUtf8(branch_name)).arg(in_branch));
    };

    for (int i = 0, end; i < count; i = end)
    {
        end = cardEnd(cards, count, i);
        bool hit = false;
        for (int k = i; k < end && !hit; k++)
            hit = cardMatches(cards[k], find);
        if (!hit)
            continue;

        if (!branch_name || strcmp(branch_name, cards[i].branch))
        {
            close_branch();
            branch_name = cards[i].branch;
            branch = new QTreeWidgetItem(card_tree, { QString::fromUtf8(branch_name) });
            branch->setData(0, Qt::UserRole, -1);
            branch->setExpanded(!find.isEmpty() || (open && !strcmp(open, branch_name)));
            in_branch = 0;
        }

        QTreeWidgetItem *under = branch;
        if (cards[i].group)
        {
            if (!group || strcmp(group_name, cards[i].group))
            {
                close_group();
                group_name = cards[i].group;
                group = new QTreeWidgetItem(branch, { QString::fromUtf8(group_name) });
                group->setData(0, Qt::UserRole, -1);
                group->setExpanded(true);
                in_group = 0;
            }
            under = group;
            in_group++;
        }
        else
            close_group();

        QTreeWidgetItem *parent = under;
        if (end - i > 1)
        {
            parent = new QTreeWidgetItem(under, { cardLabel(cards, i, end) });
            parent->setData(0, Qt::UserRole, -2 - i);
            parent->setExpanded(!find.isEmpty());
        }

        for (int k = i; k < end; k++)
        {
            auto item = new QTreeWidgetItem(parent, { QString("%1  %2").arg(QString::fromUtf8(cards[k].name), cards[k].code) });
            item->setData(0, Qt::UserRole, k);
        }
        in_branch++;
    }
    close_branch();
}

// Only a barcode types its code; a branch, group or card's branch clears it,
// a card's branch still showing the card's photos.
void BarcodeDialog::cardPicked(QTreeWidgetItem *item)
{
    if (!item)
        return;

    int count = 0;
    const S9xBarcodeReaderCard *cards = S9xBarcodeReaderCards(reader, &count);
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

void BarcodeDialog::showCard(const S9xBarcodeReaderCard *card)
{
    if (card == photo_card)
        return;

    photo_card = card;
    photos.clear();
    if (card)
        for (auto &photo : S9xBarcodeCardPhotos(photo_dir, card->image, S9xBarcodeReaderPhotoSides(reader)))
            photos.push_back(QImage(photo.rgba.data(), photo.width, photo.height, photo.width * 4,
                                    QImage::Format_RGBA8888).copy());
    showPreview();
}

// A code with no photos as its printed barcode: bars on a white label, the
// guard bars running down between the digits.
void BarcodeDialog::drawBarcode(QPainter &painter, const QSize &box)
{
    bool bars[95];
    const int modules = S9xBarcodeReaderBars(whole_code.c_str(), bars);
    if (!modules)
        return;

    // A UPC-A prints as the EAN-13 it reads as.
    const std::string code = whole_code.size() == 12 ? "0" + whole_code : whole_code;
    const bool ean13 = modules == 95;
    const int quiet_l = ean13 ? 11 : 7, quiet_r = 7, total = quiet_l + modules + quiet_r;
    const int m = std::max(1, box.width() * 85 / 100 / total);    // pixels per module
    const int digit_h = 9 * m, pad = 4 * m;
    const int bar_h = std::min(55 * m, box.height() - digit_h - 2 * pad - 8);
    const int w = total * m, h = pad + bar_h + digit_h + pad;
    const int x0 = (box.width() - w) / 2, y0 = (box.height() - h) / 2;
    const int bars_x = x0 + quiet_l * m, bar_top = y0 + pad;

    painter.fillRect(x0, y0, w, h, Qt::white);
    const int half = ean13 ? 6 : 4, mid = 3 + 7 * half;
    for (int i = 0; i < modules; i++)
    {
        if (!bars[i])
            continue;
        const bool guard = i < 3 || i >= modules - 3 || (i >= mid && i < mid + 5);
        painter.fillRect(bars_x + i * m, bar_top, m, bar_h + (guard ? digit_h / 2 : 0), Qt::black);
    }

    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(digit_h);
    painter.setFont(font);
    painter.setPen(Qt::black);
    // Each digit under its own seven modules; an EAN-13's first one in the quiet zone.
    auto digit = [&](char c, int left) {
        painter.drawText(QRect(left, bar_top + bar_h, 7 * m, digit_h + m), Qt::AlignHCenter | Qt::AlignTop, QString(QChar(c)));
    };
    int k = 0;
    if (ean13)
        digit(code[k++], bars_x - 9 * m);
    for (int i = 0; i < half; i++)
        digit(code[k++], bars_x + (3 + 7 * i) * m);
    for (int i = 0; i < half; i++)
        digit(code[k++], bars_x + (mid + 5 + 7 * i) * m);
}

// While the camera is off: the shown card's photos side by side, else the
// code's barcode, else a hint.
void BarcodeDialog::showPreview()
{
    if (camera_on)
        return;

    bool bars[95];
    if (photos.empty() && !S9xBarcodeReaderBars(whole_code.c_str(), bars))
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
    if (photos.empty())
        drawBarcode(painter, box);
    else
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const int n = (int)photos.size(), gap = 6;
        const int slot_w = (box.width() - gap * (n + 1)) / n, slot_h = box.height() - gap * 2;
        for (int i = 0; i < n; i++)
        {
            const QSize size = photos[i].size().scaled(slot_w, slot_h, Qt::KeepAspectRatio);
            const QPoint at(gap + i * (slot_w + gap) + (slot_w - size.width()) / 2, (box.height() - size.height()) / 2);
            painter.drawImage(QRect(at, size), photos[i]);
        }
    }
    painter.end();
    preview_label->setPixmap(canvas);
}

void BarcodeDialog::done(int result)
{
    stopCamera();
    QDialog::done(result);
}

void BarcodeDialog::accept()
{
    QString digits = code_edit->text();
    digits.remove(' ').remove('-');
    if (!S9xBarcodeReaderTakes(reader, digits.size()))
        return;

    scanned = digits.toStdString();
    QDialog::accept();
}
