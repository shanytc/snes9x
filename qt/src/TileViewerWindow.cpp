#include "TileViewerWindow.hpp"
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
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pv = ppuviewer;

namespace
{

QString hex(uint32_t value, int digits)
{
    return QStringLiteral("0x") +
           QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

} // namespace

TileViewerWindow::TileViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("Tile Viewer"))
{
    createWidgets();
    resize(940, 700);
    refresh();
}

void TileViewerWindow::createWidgets()
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

    auto form = new QGridLayout;
    form->setHorizontalSpacing(6);
    int row = 0;

    form->addWidget(new QLabel(tr("Source:")), row, 0);
    source_combo = new QComboBox;
    for (int i = 0; i < pv::kSourceCount; i++)
        source_combo->addItem(tr(pv::source_name(i)));
    connect(source_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.source = index;
        state.address = 0;
        selected_tile = -1;
        showAddress();
        showTileInfo();
        rerender();
    });
    form->addWidget(source_combo, row++, 1, 1, 2);

    form->addWidget(new QLabel(tr("Address:")), row, 0);
    auto address_row = new QHBoxLayout;
    address_edit = new QLineEdit;
    connect(address_edit, &QLineEdit::textEdited, this, [this](const QString &text) {
        uint32_t value;
        if (!pv::parse_hex(text.toStdString(), &value))
            return;
        state.address = pv::align_address(state.bit_depth,
                                          value & pv::address_mask(state.source));
        rerender();
    });
    address_row->addWidget(address_edit);
    /* The auto-repeat matches win32's subclassed step buttons: hold one down
     * and it keeps walking through the source. */
    auto prev_button = new QPushButton(QStringLiteral("<"));
    prev_button->setAutoRepeat(true);
    prev_button->setMaximumWidth(28);
    connect(prev_button, &QPushButton::clicked, this, [this] {
        pv::step_address(state, false);
        showAddress();
        rerender();
    });
    address_row->addWidget(prev_button);
    auto next_button = new QPushButton(QStringLiteral(">"));
    next_button->setAutoRepeat(true);
    next_button->setMaximumWidth(28);
    connect(next_button, &QPushButton::clicked, this, [this] {
        pv::step_address(state, true);
        showAddress();
        rerender();
    });
    address_row->addWidget(next_button);
    form->addLayout(address_row, row++, 1, 1, 2);

    form->addWidget(new QLabel(tr("Bit Depth:")), row, 0);
    bit_depth_combo = new QComboBox;
    for (int i = 0; i < pv::kBitDepthCount; i++)
        bit_depth_combo->addItem(tr(pv::bit_depth_name(i)));
    bit_depth_combo->setCurrentIndex(state.bit_depth);
    connect(bit_depth_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.bit_depth = index;
        selected_tile = -1;
        showTileInfo();
        rerender();
    });
    form->addWidget(bit_depth_combo, row++, 1, 1, 2);

    form->addWidget(new QLabel(tr("Width:")), row, 0);
    auto width_spin = new QSpinBox;
    width_spin->setRange(8, 64);
    width_spin->setValue(state.width_tiles);
    connect(width_spin, &QSpinBox::valueChanged, this, [this](int value) {
        state.width_tiles = value;
        rerender();
    });
    form->addWidget(width_spin, row++, 1);

    form->addWidget(new QLabel(tr("Layout:")), row, 0);
    auto layout_combo = new QComboBox;
    for (int i = 0; i < pv::kTileLayoutCount; i++)
        layout_combo->addItem(tr(pv::tile_layout_name(i)));
    connect(layout_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.layout = index;
        selected_tile = -1;
        showTileInfo();
        rerender();
    });
    form->addWidget(layout_combo, row++, 1, 1, 2);

    side->addLayout(form);

    cgram_check = new QCheckBox(tr("Use CGRAM"));
    cgram_check->setChecked(state.use_cgram);
    connect(cgram_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.use_cgram = checked;
        rerender();
    });
    side->addWidget(cgram_check);

    auto palette_box = new QGroupBox(tr("Palette (click to select offset)"));
    auto palette_layout = new QVBoxLayout(palette_box);
    palette_view = new PPUPaletteView;
    connect(palette_view, &PPUPaletteView::selectionChanged, this, [this](int index) {
        state.palette_offset = index;
        // Picking a colour block only means something through the real palette.
        state.use_cgram = true;
        cgram_check->setChecked(true);
        rerender();
    });
    palette_layout->addWidget(palette_view);
    side->addWidget(palette_box);

    auto base_box = new QGroupBox(tr("Base Tile Addresses"));
    auto base_layout = new QGridLayout(base_box);
    static const char *base_labels[pv::kBaseAddressCount] = {
        QT_TR_NOOP("BG1:"), QT_TR_NOOP("BG2:"), QT_TR_NOOP("BG3:"),
        QT_TR_NOOP("BG4:"), QT_TR_NOOP("OAM1:"), QT_TR_NOOP("OAM2:"),
    };
    for (int i = 0; i < pv::kBaseAddressCount; i++)
    {
        base_layout->addWidget(new QLabel(tr(base_labels[i])), i, 0);
        base_address_edits[i] = new QLineEdit;
        base_address_edits[i]->setReadOnly(true);
        base_layout->addWidget(base_address_edits[i], i, 1);
        auto button = new QPushButton(tr("goto"));
        connect(button, &QPushButton::clicked, this, [this, i] { gotoBaseAddress(i); });
        base_layout->addWidget(button, i, 2);
    }
    side->addWidget(base_box);

    tile_info = new QLabel;
    tile_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    side->addWidget(tile_info);
    side->addStretch();

    auto side_widget = new QWidget;
    side_widget->setLayout(side);
    side_widget->setFixedWidth(260);
    outer->addWidget(side_widget);

    auto canvas_box = new QGroupBox(tr("Tiles"));
    auto canvas_layout = new QVBoxLayout(canvas_box);
    canvas_layout->setContentsMargins(4, 4, 4, 4);
    canvas = new PPUImageView;
    connect(canvas, &PPUImageView::clicked, this, [this](int x, int y) {
        const int tile = pv::tile_at_pixel(state, x, y);
        if (tile < 0)
            return;
        selected_tile = tile;
        showTileInfo();
    });
    connect(canvas, &PPUImageView::contextMenuAt, this, [this](const QPoint &position) {
        QMenu menu;
        auto action = menu.addAction(tr("Export to PNG..."));
        if (menu.exec(position) == action)
            exportTiles();
    });
    auto scroll_area = new QScrollArea;
    scroll_area->setWidget(canvas);
    scroll_area->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll_area->setBackgroundRole(QPalette::Dark);
    canvas_layout->addWidget(scroll_area);
    outer->addWidget(canvas_box, 1);

    showAddress();
}

void TileViewerWindow::showAddress()
{
    const int digits = state.source == pv::kSourceVRAM ? 4 : 6;
    const QSignalBlocker blocker(address_edit);
    address_edit->setText(hex(state.address & pv::address_mask(state.source), digits));
}

void TileViewerWindow::showTileInfo()
{
    if (selected_tile < 0)
    {
        tile_info->clear();
        return;
    }
    const int digits = state.source == pv::kSourceVRAM ? 4 : 6;
    tile_info->setText(tr("Selected tile #%1\nAddress %2")
                           .arg(selected_tile)
                           .arg(hex(pv::tile_address(state, selected_tile), digits)));
}

void TileViewerWindow::gotoBaseAddress(int which)
{
    uint32_t addresses[pv::kBaseAddressCount];
    int bit_depth = state.bit_depth;
    runOnCore([&] {
        pv::base_addresses(addresses);
        bit_depth = pv::base_address_bit_depth(which);
    });

    state.bit_depth = bit_depth;
    bit_depth_combo->setCurrentIndex(bit_depth);
    // The base addresses name places in VRAM, wherever the view was pointed.
    state.source = pv::kSourceVRAM;
    source_combo->setCurrentIndex(pv::kSourceVRAM);
    state.address = pv::align_address(bit_depth, addresses[which]);
    selected_tile = -1;

    showAddress();
    showTileInfo();
    rerender();
}

void TileViewerWindow::rerender()
{
    pv::Pixel palette[256];
    runOnCore([&] {
        pv::render_tiles(state, picture);
        pv::snapshot_palette(palette);
    });

    canvas->setImage(picture);
    palette_view->setPalette(palette);
    palette_view->setSelected(state.palette_offset);
}

void TileViewerWindow::refresh()
{
    uint32_t addresses[pv::kBaseAddressCount];
    pv::Pixel palette[256];
    runOnCore([&] {
        pv::render_tiles(state, picture);
        pv::base_addresses(addresses);
        pv::snapshot_palette(palette);
    });

    canvas->setImage(picture);
    palette_view->setPalette(palette);
    palette_view->setSelected(state.palette_offset);

    for (int i = 0; i < pv::kBaseAddressCount; i++)
        base_address_edits[i]->setText(hex(addresses[i], 4));
}

void TileViewerWindow::exportTiles()
{
    /* The grid is a reading aid, not part of the tiles: leave it out of the
     * file however the checkbox is set. */
    pv::Image exported;
    pv::TileViewerState without_grid = state;
    without_grid.show_grid = false;
    runOnCore([&] { pv::render_tiles(without_grid, exported); });
    exportImage(exported, QStringLiteral("tiles.png"));
}
