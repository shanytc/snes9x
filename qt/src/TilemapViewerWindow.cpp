#include "TilemapViewerWindow.hpp"
#include "EmuApplication.hpp"
#include "EmuMainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pv = ppuviewer;

namespace
{

QString hex16(uint32_t value)
{
    return QStringLiteral("0x") +
           QString::number(value & 0xFFFF, 16).toUpper().rightJustified(4, QLatin1Char('0'));
}

} // namespace

TilemapViewerWindow::TilemapViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("Tilemap Viewer"))
{
    createWidgets();
    resize(940, 680);
    refresh();
}

void TilemapViewerWindow::createWidgets()
{
    auto outer = new QHBoxLayout(this);

    auto side = new QVBoxLayout;
    side->setSpacing(6);

    auto top_row = new QHBoxLayout;
    top_row->addWidget(new QLabel(tr("Zoom:")));
    auto zoom_combo = new QComboBox;
    ppuFillZoomCombo(zoom_combo, 1);
    connect(zoom_combo, &QComboBox::currentIndexChanged, this,
            [this](int index) { canvas->setZoom(index + 1); });
    top_row->addWidget(zoom_combo);
    auto grid_check = new QCheckBox(tr("Show Grid"));
    connect(grid_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.show_grid = checked;
        rerender();
    });
    top_row->addWidget(grid_check);
    top_row->addStretch();
    side->addLayout(top_row);

    auto background_row = new QHBoxLayout;
    background_row->addWidget(new QLabel(tr("Background:")));
    auto background_combo = new QComboBox;
    for (int i = 0; i < pv::kTilemapBackgroundCount; i++)
        background_combo->addItem(tr(pv::tilemap_background_name(i)));
    background_combo->setCurrentIndex(state.background);
    connect(background_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.background = index;
        rerender();
    });
    background_row->addWidget(background_combo, 1);
    side->addLayout(background_row);

    auto update_row = new QHBoxLayout;
    auto auto_update_check = new QCheckBox(tr("Auto update"));
    auto_update_check->setChecked(true);
    connect(auto_update_check, &QCheckBox::toggled, this,
            [this](bool checked) { auto_update = checked; });
    update_row->addWidget(auto_update_check);
    auto refresh_button = new QPushButton(tr("Refresh"));
    connect(refresh_button, &QPushButton::clicked, this, [this] { refresh(); });
    update_row->addWidget(refresh_button);
    update_row->addStretch();
    side->addLayout(update_row);

    auto custom_mode_check = new QCheckBox(tr("Custom Screen Mode"));
    connect(custom_mode_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.custom_screen_mode = checked;
        if (checked)
            state.custom_mode = mode_spin->value();
        applyEnabledState();
        rerender();
    });
    side->addWidget(custom_mode_check);

    auto mode_row = new QHBoxLayout;
    mode_row->addWidget(new QLabel(tr("Mode:")));
    mode_spin = new QSpinBox;
    mode_spin->setRange(0, 7);
    mode_spin->setValue(state.custom_mode);
    connect(mode_spin, &QSpinBox::valueChanged, this, [this](int value) {
        if (!state.custom_screen_mode)
            return;
        state.custom_mode = value;
        rerender();
    });
    mode_row->addWidget(mode_spin);
    mode_row->addStretch();
    side->addLayout(mode_row);

    auto bg_row = new QHBoxLayout;
    bg_row->addWidget(new QLabel(tr("BG:")));
    QRadioButton *bg_radios[4];
    for (int i = 0; i < 4; i++)
    {
        bg_radios[i] = new QRadioButton(QString::number(i + 1));
        bg_row->addWidget(bg_radios[i]);
    }
    // Picked before the connections, so the initial state does not render
    // through a canvas that does not exist yet.
    bg_radios[0]->setChecked(true);
    for (int i = 0; i < 4; i++)
        connect(bg_radios[i], &QRadioButton::toggled, this, [this, i](bool checked) {
            if (!checked)
                return;
            state.bg = i;
            rerender();
        });
    bg_row->addStretch();
    side->addLayout(bg_row);

    auto override_check = new QCheckBox(tr("Override Tilemap"));
    connect(override_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.override_tilemap = checked;
        if (checked)
            readOverrideFields();
        applyEnabledState();
        rerender();
    });
    side->addWidget(override_check);

    auto form = new QGridLayout;
    int row = 0;

    form->addWidget(new QLabel(tr("Bit Depth:")), row, 0);
    bit_depth_combo = new QComboBox;
    bit_depth_combo->addItems({ tr("2bpp"), tr("4bpp"), tr("8bpp"), tr("Mode 7") });
    bit_depth_combo->setCurrentIndex(state.override_bit_depth);
    form->addWidget(bit_depth_combo, row++, 1);

    form->addWidget(new QLabel(tr("Map Size:")), row, 0);
    map_size_combo = new QComboBox;
    map_size_combo->addItems({ QStringLiteral("32x32"), QStringLiteral("64x32"),
                               QStringLiteral("32x64"), QStringLiteral("64x64") });
    form->addWidget(map_size_combo, row++, 1);

    form->addWidget(new QLabel(tr("Map Addr:")), row, 0);
    map_address_edit = new QLineEdit(QStringLiteral("0x0000"));
    form->addWidget(map_address_edit, row++, 1);

    form->addWidget(new QLabel(tr("Tile Size:")), row, 0);
    tile_size_combo = new QComboBox;
    tile_size_combo->addItems({ QStringLiteral("8x8"), QStringLiteral("16x16") });
    form->addWidget(tile_size_combo, row++, 1);

    form->addWidget(new QLabel(tr("Tile Addr:")), row, 0);
    tile_address_edit = new QLineEdit(QStringLiteral("0x0000"));
    form->addWidget(tile_address_edit, row++, 1);

    for (auto *combo : { bit_depth_combo, map_size_combo, tile_size_combo })
        connect(combo, &QComboBox::currentIndexChanged, this, [this](int) {
            if (!state.override_tilemap)
                return;
            readOverrideFields();
            rerender();
        });
    for (auto *edit : { map_address_edit, tile_address_edit })
        connect(edit, &QLineEdit::textEdited, this, [this](const QString &) {
            if (!state.override_tilemap)
                return;
            readOverrideFields();
            rerender();
        });

    side->addLayout(form);

    info_label = new QLabel;
    info_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    side->addWidget(info_label);
    side->addStretch();

    auto side_widget = new QWidget;
    side_widget->setLayout(side);
    side_widget->setFixedWidth(260);
    outer->addWidget(side_widget);

    auto canvas_box = new QGroupBox(tr("Tilemap"));
    auto canvas_layout = new QVBoxLayout(canvas_box);
    canvas_layout->setContentsMargins(4, 4, 4, 4);
    canvas = new PPUImageView;
    connect(canvas, &PPUImageView::contextMenuAt, this, [this](const QPoint &position) {
        QMenu menu;
        auto action = menu.addAction(tr("Export to PNG..."));
        if (menu.exec(position) == action)
            exportTilemap();
    });
    auto scroll_area = new QScrollArea;
    scroll_area->setWidget(canvas);
    scroll_area->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll_area->setBackgroundRole(QPalette::Dark);
    canvas_layout->addWidget(scroll_area);
    outer->addWidget(canvas_box, 1);

    applyEnabledState();
}

void TilemapViewerWindow::applyEnabledState()
{
    mode_spin->setEnabled(state.custom_screen_mode);
    for (auto *widget : { (QWidget *)bit_depth_combo, (QWidget *)map_size_combo,
                          (QWidget *)map_address_edit, (QWidget *)tile_size_combo,
                          (QWidget *)tile_address_edit })
        widget->setEnabled(state.override_tilemap);
}

void TilemapViewerWindow::readOverrideFields()
{
    state.override_bit_depth = bit_depth_combo->currentIndex();
    state.override_map_size = map_size_combo->currentIndex();
    state.override_tile_size = tile_size_combo->currentIndex();

    uint32_t value;
    if (pv::parse_hex(map_address_edit->text().toStdString(), &value))
        state.override_map_address = value & 0xFFFF;
    if (pv::parse_hex(tile_address_edit->text().toStdString(), &value))
        state.override_tile_address = value & 0xFFFF;
}

/* While the override is off the fields show what the PPU actually has, so
 * turning it on starts from the live configuration rather than from zero. */
void TilemapViewerWindow::seedAutomaticFields(const pv::TilemapInfo &info)
{
    if (!state.custom_screen_mode)
    {
        const QSignalBlocker blocker(mode_spin);
        mode_spin->setValue(info.mode);
    }
    if (state.override_tilemap)
        return;

    const QSignalBlocker depth_blocker(bit_depth_combo);
    const QSignalBlocker size_blocker(map_size_combo);
    const QSignalBlocker tile_blocker(tile_size_combo);
    const QSignalBlocker map_blocker(map_address_edit);
    const QSignalBlocker tile_address_blocker(tile_address_edit);

    bit_depth_combo->setCurrentIndex(info.mode7 ? pv::kTilemapMode7
                                                : pv::tilemap_bit_depth_index(info.bpp));
    map_size_combo->setCurrentIndex((info.width_tiles == 64 ? 1 : 0) |
                                    (info.height_tiles == 64 ? 2 : 0));
    tile_size_combo->setCurrentIndex(info.tile_size == 16 ? 1 : 0);
    map_address_edit->setText(hex16(info.map_address));
    tile_address_edit->setText(hex16(info.tile_address));
}

void TilemapViewerWindow::rerender()
{
    pv::TilemapInfo info;
    runOnCore([&] { pv::render_tilemap(state, picture, &info); });

    canvas->setImage(picture);
    seedAutomaticFields(info);

    if (!info.valid)
    {
        info_label->setText(tr("BG%1 not valid in mode %2").arg(info.bg + 1).arg(info.mode));
        return;
    }
    if (info.mode7)
    {
        info_label->setText(tr("Mode 7  128x128  8bpp"));
        return;
    }
    info_label->setText(tr("Mode %1  BG%2  %3bpp\n%4x%5 tiles  %6x%7 px\nMap @ %8\nTiles @ %9")
                            .arg(info.mode)
                            .arg(info.bg + 1)
                            .arg(info.bpp)
                            .arg(info.width_tiles)
                            .arg(info.height_tiles)
                            .arg(info.width_tiles * info.tile_size)
                            .arg(info.height_tiles * info.tile_size)
                            .arg(hex16(info.map_address))
                            .arg(hex16(info.tile_address)));
}

void TilemapViewerWindow::refresh()
{
    rerender();
}

void TilemapViewerWindow::exportTilemap()
{
    /* As in the Tile Viewer, the grid overlay stays out of the file, and a
     * Transparent background exports as alpha 0. */
    pv::Image exported;
    pv::TilemapViewerState without_grid = state;
    without_grid.show_grid = false;
    runOnCore([&] { pv::render_tilemap(without_grid, exported, nullptr, true); });
    exportImage(exported, QStringLiteral("tilemap.png"));
}
