#include "SpriteViewerWindow.hpp"
#include "EmuApplication.hpp"
#include "EmuMainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace pv = ppuviewer;

namespace
{

/* How often the list and the composition are re-read. The SNES rewrites OAM
 * every frame, so a slower rate is what makes the numbers readable. */
struct RateOption
{
    const char *label;
    int interval_ms;
};

const RateOption kRates[] = {
    { QT_TRANSLATE_NOOP("SpriteViewerWindow", "Realtime"), 1000 / 60 },
    { QT_TRANSLATE_NOOP("SpriteViewerWindow", "0.25s"), 250 },
    { QT_TRANSLATE_NOOP("SpriteViewerWindow", "0.5s"), 500 },
    { QT_TRANSLATE_NOOP("SpriteViewerWindow", "1s"), 1000 },
    { QT_TRANSLATE_NOOP("SpriteViewerWindow", "2s"), 2000 },
};
constexpr int kDefaultRate = 3; // 1s

QString hex3(uint32_t value)
{
    return QStringLiteral("0x") +
           QString::number(value & 0x1FF, 16).toUpper().rightJustified(3, QLatin1Char('0'));
}

} // namespace

SpriteViewerWindow::SpriteViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("Sprite Viewer"))
{
    for (bool &shown : visible)
        shown = true;

    createWidgets();
    setUpdateInterval(kRates[kDefaultRate].interval_ms);
    resize(1000, 760);
    refresh();
}

void SpriteViewerWindow::createWidgets()
{
    auto outer = new QVBoxLayout(this);

    auto screen_box = new QGroupBox(tr("Screen"));
    auto screen_layout = new QVBoxLayout(screen_box);
    screen_layout->setContentsMargins(4, 4, 4, 4);
    screen_view = new PPUImageView;
    connect(screen_view, &PPUImageView::contextMenuAt, this, [this](const QPoint &position) {
        QMenu menu;
        auto action = menu.addAction(tr("Export to PNG..."));
        if (menu.exec(position) == action)
            exportScreen();
    });
    auto screen_area = new QScrollArea;
    screen_area->setWidget(screen_view);
    screen_area->setAlignment(Qt::AlignCenter);
    screen_area->setBackgroundRole(QPalette::Dark);
    screen_layout->addWidget(screen_area);
    outer->addWidget(screen_box, 3);

    auto bottom = new QHBoxLayout;

    list = new QTreeWidget;
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setUniformRowHeights(true);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->setHeaderLabels({ tr("#"), tr("Size"), tr("X"), tr("Y"), tr("Char"), tr("Pri"),
                            tr("Pal"), tr("Flags") });
    for (int i = 0; i < 128; i++)
    {
        auto item = new QTreeWidgetItem(list);
        item->setText(0, QString::number(i));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
    }
    list->resizeColumnToContents(0);
    connect(list, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto items = list->selectedItems();
        if (items.isEmpty())
            return;
        selected = list->indexOfTopLevelItem(items.first());
        drawPreview();
    });
    connect(list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (updating_list || column != 0)
            return;
        const int index = list->indexOfTopLevelItem(item);
        if (index < 0 || index >= 128)
            return;
        const bool shown = item->checkState(0) == Qt::Checked;
        if (visible[index] == shown)
            return;
        visible[index] = shown;
        drawScreen();
    });
    connect(list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        showListMenu(list->viewport()->mapToGlobal(position));
    });
    bottom->addWidget(list, 2);

    auto side = new QVBoxLayout;

    auto preview_box = new QGroupBox(tr("Preview"));
    auto preview_layout = new QHBoxLayout(preview_box);
    preview_layout->setSpacing(10);
    preview = new PPUImageView;
    preview->setFitToWidget(true);
    /* Pinned, not stretched: paintEvent floods the whole widget with the
     * background colour, so a wider widget means a wider block of colour
     * rather than more room around the sprite. The panel's spare width goes
     * to the gap before the controls instead. */
    preview->setFixedWidth(140);
    preview->setMinimumHeight(140);
    connect(preview, &PPUImageView::contextMenuAt, this, [this](const QPoint &position) {
        QMenu menu;
        auto action = menu.addAction(tr("Export to PNG..."));
        if (menu.exec(position) == action)
            exportPreview();
    });
    preview_layout->addWidget(preview);
    preview_layout->addStretch(1); // holds the controls off the image

    auto controls = new QGridLayout;
    int row = 0;
    controls->addWidget(new QLabel(tr("Zoom:")), row, 0);
    auto zoom_combo = new QComboBox;
    ppuFillZoomCombo(zoom_combo, 1);
    connect(zoom_combo, &QComboBox::currentIndexChanged, this,
            [this](int index) { screen_view->setZoom(index + 1); });
    controls->addWidget(zoom_combo, row++, 1);

    auto auto_update_check = new QCheckBox(tr("Auto update"));
    auto_update_check->setChecked(true);
    connect(auto_update_check, &QCheckBox::toggled, this,
            [this](bool checked) { auto_update = checked; });
    controls->addWidget(auto_update_check, row++, 0, 1, 2);

    auto refresh_button = new QPushButton(tr("Refresh"));
    connect(refresh_button, &QPushButton::clicked, this, [this] { refresh(); });
    controls->addWidget(refresh_button, row++, 0, 1, 2);

    controls->addWidget(new QLabel(tr("Rate:")), row, 0);
    auto rate_combo = new QComboBox;
    for (const auto &rate : kRates)
        rate_combo->addItem(tr(rate.label));
    rate_combo->setCurrentIndex(kDefaultRate);
    connect(rate_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && index < (int)(sizeof(kRates) / sizeof(kRates[0])))
            setUpdateInterval(kRates[index].interval_ms);
    });
    controls->addWidget(rate_combo, row++, 1);
    controls->setRowStretch(row, 1);
    preview_layout->addLayout(controls);
    side->addWidget(preview_box);

    auto outline_check = new QCheckBox(tr("Show Screen Outline"));
    outline_check->setChecked(show_outline);
    connect(outline_check, &QCheckBox::toggled, this, [this](bool checked) {
        show_outline = checked;
        drawScreen();
    });
    side->addWidget(outline_check);

    auto background_row = new QHBoxLayout;
    background_row->addWidget(new QLabel(tr("Background:")));
    auto background_combo = new QComboBox;
    for (int i = 0; i < pv::kSpriteBackgroundCount; i++)
    {
        const char *name = pv::sprite_background_name(i);
        background_combo->addItem(name ? tr(name)
                                       : tr("Sprite Palette %1").arg(i - pv::kBackgroundPalette0));
    }
    connect(background_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        background = index;
        drawScreen();
        drawPreview();
    });
    background_row->addWidget(background_combo, 1);
    side->addLayout(background_row);

    auto first_sprite_row = new QHBoxLayout;
    first_sprite_row->addWidget(new QLabel(tr("First Sprite:")));
    first_sprite_label = new QLabel;
    first_sprite_row->addWidget(first_sprite_label);
    first_sprite_row->addStretch();
    side->addLayout(first_sprite_row);

    details_label = new QLabel;
    details_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    side->addWidget(details_label);
    side->addStretch();

    auto side_widget = new QWidget;
    side_widget->setLayout(side);
    side_widget->setFixedWidth(380);
    bottom->addWidget(side_widget);

    outer->addLayout(bottom, 4);

    list->topLevelItem(0)->setSelected(true);
}

void SpriteViewerWindow::refresh()
{
    runOnCore([this] { pv::snapshot_oam(oam); });

    updateList();
    first_sprite_label->setText(QString::number(oam.first_sprite));
    drawScreen();
    drawPreview();
}

void SpriteViewerWindow::updateList()
{
    updating_list = true;
    list->setUpdatesEnabled(false);
    for (int i = 0; i < 128; i++)
    {
        const pv::Sprite &sprite = oam.sprites[i];
        int width, height;
        pv::sprite_size(oam.size_select, sprite.large, &width, &height);

        QTreeWidgetItem *item = list->topLevelItem(i);
        item->setText(1, QStringLiteral("%1x%2").arg(width).arg(height));
        item->setText(2, QString::number(sprite.x));
        item->setText(3, QString::number(sprite.y));
        item->setText(4, hex3(sprite.name));
        item->setText(5, QString::number(sprite.priority));
        item->setText(6, QString::number(sprite.palette));
        item->setText(7, QStringLiteral("%1%2")
                             .arg(sprite.hflip ? QStringLiteral("H") : QStringLiteral("-"))
                             .arg(sprite.vflip ? QStringLiteral("V") : QStringLiteral("-")));
    }
    list->setUpdatesEnabled(true);
    updating_list = false;
}

void SpriteViewerWindow::drawScreen()
{
    pv::Pixel color;
    runOnCore([&] {
        pv::render_sprite_screen(oam, visible, background, show_outline, false, screen_picture);
        pv::Pixel palette[256];
        pv::snapshot_palette(palette);
        color = pv::sprite_background_color(background, palette);
    });

    /* The widget is wider than the 512x256 composition, so it carries the
     * same backdrop out to its edges. */
    screen_view->setBackground(QColor::fromRgb(color));
    screen_view->setImage(screen_picture);
}

void SpriteViewerWindow::drawPreview()
{
    if (selected < 0 || selected >= 128)
    {
        details_label->clear();
        preview->setImage(pv::Image());
        return;
    }

    pv::Pixel color;
    runOnCore([&] {
        pv::render_sprite(oam, selected, preview_picture, background, false);
        pv::Pixel palette[256];
        pv::snapshot_palette(palette);
        color = pv::sprite_background_color(background, palette);
    });

    preview->setBackground(QColor::fromRgb(color));
    preview->setImage(preview_picture);

    const pv::Sprite &sprite = oam.sprites[selected];
    int width, height;
    pv::sprite_size(oam.size_select, sprite.large, &width, &height);
    details_label->setText(tr("Sprite #%1\nsize %2x%3\npos (%4,%5)\ntile %6\npal %7  pri %8\nflags %9")
                               .arg(selected)
                               .arg(width)
                               .arg(height)
                               .arg(sprite.x)
                               .arg(sprite.y)
                               .arg(hex3(sprite.name))
                               .arg(sprite.palette)
                               .arg(sprite.priority)
                               .arg(QStringLiteral("%1%2")
                                        .arg(sprite.hflip ? QStringLiteral("H")
                                                          : QStringLiteral("-"))
                                        .arg(sprite.vflip ? QStringLiteral("V")
                                                          : QStringLiteral("-"))));
}

std::vector<int> SpriteViewerWindow::selectedSprites() const
{
    std::vector<int> indices;
    for (QTreeWidgetItem *item : list->selectedItems())
    {
        const int index = list->indexOfTopLevelItem(item);
        if (index >= 0 && index < 128)
            indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

void SpriteViewerWindow::showListMenu(const QPoint &global_position)
{
    QMenu menu;
    auto toggle_action = menu.addAction(tr("Toggle Visibility"));
    auto only_action = menu.addAction(tr("Show Only Selected Objects"));
    auto all_action = menu.addAction(tr("Show All Objects"));
    menu.addSeparator();
    auto export_action = menu.addAction(tr("Export Selected to PNG..."));

    QAction *chosen = menu.exec(global_position);
    if (!chosen)
        return;

    if (chosen == export_action)
    {
        exportSelected();
        return;
    }

    const std::vector<int> indices = selectedSprites();

    updating_list = true;
    if (chosen == toggle_action)
    {
        for (int index : indices)
            visible[index] = !visible[index];
    }
    else if (chosen == only_action)
    {
        for (int i = 0; i < 128; i++)
            visible[i] = false;
        for (int index : indices)
            visible[index] = true;
    }
    else if (chosen == all_action)
    {
        for (bool &shown : visible)
            shown = true;
    }
    for (int i = 0; i < 128; i++)
        list->topLevelItem(i)->setCheckState(0, visible[i] ? Qt::Checked : Qt::Unchecked);
    updating_list = false;

    drawScreen();
}

void SpriteViewerWindow::exportSelected()
{
    const std::vector<int> indices = selectedSprites();
    if (indices.empty())
        return;

    /* One object goes out as a PNG; a set of them as a zip of PNGs, which is
     * what win32's list context menu does. */
    if (indices.size() == 1)
    {
        pv::Image exported;
        runOnCore([&] {
            pv::render_sprite(oam, indices[0], exported, pv::kBackgroundTransparent, true);
        });
        exportImage(exported, QStringLiteral("sprite_%1.png").arg(indices[0]));
        return;
    }

    std::vector<pv::ZipBlob> entries;
    runOnCore([&] {
        for (int index : indices)
        {
            pv::Image exported;
            pv::render_sprite(oam, index, exported, pv::kBackgroundTransparent, true);
            pv::ZipBlob blob;
            blob.name = "sprite_" + std::to_string(index) + ".png";
            if (pv::write_png_memory(exported, blob.data))
                entries.push_back(std::move(blob));
        }
    });
    if (entries.empty())
        return;

    const QString filename = QFileDialog::getSaveFileName(this, tr("Export Selected to PNG"),
                                                          QStringLiteral("sprites.zip"),
                                                          tr("ZIP Archive (*.zip)"));
    if (filename.isEmpty())
        return;
    if (!pv::write_zip(filename.toStdString(), entries))
        QMessageBox::warning(this, tr("Export"), tr("Failed to save ZIP."));
}

void SpriteViewerWindow::exportScreen()
{
    /* No screen outline in the file, and a transparent background stays
     * transparent rather than becoming the viewer's gray. */
    pv::Image exported;
    runOnCore([&] {
        pv::render_sprite_screen(oam, visible, background, false, true, exported);
    });
    exportImage(exported, QStringLiteral("sprite_screen.png"));
}

void SpriteViewerWindow::exportPreview()
{
    if (selected < 0 || selected >= 128)
        return;
    pv::Image exported;
    runOnCore([&] {
        pv::render_sprite(oam, selected, exported, pv::kBackgroundTransparent, true);
    });
    exportImage(exported, QStringLiteral("sprite_%1.png").arg(selected));
}
