#include "GBViewerWindows.hpp"
#include "EmuApplication.hpp"
#include "EmuMainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace gv = gbviewer;
namespace pv = ppuviewer;

namespace
{

QString hex(uint32_t value, int digits)
{
    return QStringLiteral("0x") +
           QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

/* The layout every GB viewer shares: the controls on the left, then the
 * picture in a titled, scrolling box with the Export to PNG context menu.
 * Returns the left column for the caller to fill. */
QVBoxLayout *buildFrame(PPUViewerWindow *window, const QString &canvas_title, PPUImageView *canvas,
                        const std::function<void()> &export_png)
{
    auto outer = new QHBoxLayout(window);

    auto side = new QVBoxLayout;
    side->setSpacing(6);
    auto side_widget = new QWidget;
    side_widget->setLayout(side);
    side_widget->setFixedWidth(230);
    outer->addWidget(side_widget);

    auto canvas_box = new QGroupBox(canvas_title);
    auto canvas_layout = new QVBoxLayout(canvas_box);
    canvas_layout->setContentsMargins(4, 4, 4, 4);
    QObject::connect(canvas, &PPUImageView::contextMenuAt, window,
                     [export_png](const QPoint &position) {
                         QMenu menu;
                         auto action = menu.addAction(PPUViewerWindow::tr("Export to PNG..."));
                         if (menu.exec(position) == action)
                             export_png();
                     });
    auto scroll_area = new QScrollArea;
    scroll_area->setWidget(canvas);
    scroll_area->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll_area->setBackgroundRole(QPalette::Dark);
    canvas_layout->addWidget(scroll_area);
    outer->addWidget(canvas_box, 1);

    return side;
}

/* Zoom, the checkbox beside it, Auto update and Refresh. */
void addUpdateRows(PPUViewerWindow *window, QVBoxLayout *side, PPUImageView *canvas, int zoom,
                   QWidget *beside_zoom, bool *auto_update, const std::function<void()> &refresh)
{
    auto top_row = new QHBoxLayout;
    top_row->addWidget(new QLabel(PPUViewerWindow::tr("Zoom:")));
    auto zoom_combo = new QComboBox;
    ppuFillZoomCombo(zoom_combo, zoom);
    canvas->setZoom(zoom);
    QObject::connect(zoom_combo, &QComboBox::currentIndexChanged, window,
                     [canvas](int index) { canvas->setZoom(index + 1); });
    top_row->addWidget(zoom_combo);
    if (beside_zoom)
        top_row->addWidget(beside_zoom);
    top_row->addStretch();
    side->addLayout(top_row);

    auto update_row = new QHBoxLayout;
    auto auto_update_check = new QCheckBox(PPUViewerWindow::tr("Auto update"));
    auto_update_check->setChecked(true);
    QObject::connect(auto_update_check, &QCheckBox::toggled, window,
                     [auto_update](bool checked) { *auto_update = checked; });
    update_row->addWidget(auto_update_check);
    auto refresh_button = new QPushButton(PPUViewerWindow::tr("Refresh"));
    QObject::connect(refresh_button, &QPushButton::clicked, window, [refresh] { refresh(); });
    update_row->addWidget(refresh_button);
    update_row->addStretch();
    side->addLayout(update_row);
}

QLabel *addInfoLabel(QVBoxLayout *side)
{
    auto label = new QLabel;
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    side->addWidget(label);
    side->addStretch();
    return label;
}

} // namespace

/* ---- GB Tile Viewer ------------------------------------------------------ */

GBTileViewerWindow::GBTileViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("GB Tile Viewer"))
{
    canvas = new PPUImageView;
    auto side = buildFrame(this, tr("Tiles"), canvas, [this] { exportTiles(); });

    auto grid_check = new QCheckBox(tr("Show Grid"));
    connect(grid_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.show_grid = checked;
        refresh();
    });
    // GB tiles are small, so this one opens at 3x.
    addUpdateRows(this, side, canvas, 3, grid_check, &auto_update, [this] { refresh(); });

    auto form = new QGridLayout;
    form->setHorizontalSpacing(6);
    int row = 0;

    form->addWidget(new QLabel(tr("Palette:")), row, 0);
    auto palette_combo = new QComboBox;
    for (int i = 0; i < gv::kPaletteModeCount; i++)
        palette_combo->addItem(tr(gv::palette_mode_name(i)));
    palette_combo->setCurrentIndex(state.palette_mode);
    connect(palette_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.palette_mode = index;
        refresh();
    });
    form->addWidget(palette_combo, row++, 1, 1, 3);

    form->addWidget(new QLabel(tr("Index:")), row, 0);
    auto index_spin = new QSpinBox;
    index_spin->setRange(0, 7);
    index_spin->setValue(state.palette_index);
    connect(index_spin, &QSpinBox::valueChanged, this, [this](int value) {
        state.palette_index = value;
        refresh();
    });
    form->addWidget(index_spin, row, 1);
    form->addWidget(new QLabel(tr("Bank:")), row, 2);
    auto bank_combo = new QComboBox;
    bank_combo->addItem(QStringLiteral("0"));
    bank_combo->addItem(QStringLiteral("1"));
    connect(bank_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.bank = index;
        showTileInfo();
        refresh();
    });
    form->addWidget(bank_combo, row++, 3);

    form->addWidget(new QLabel(tr("Width:")), row, 0);
    auto width_spin = new QSpinBox;
    width_spin->setRange(8, 32);
    width_spin->setValue(state.width_tiles);
    connect(width_spin, &QSpinBox::valueChanged, this, [this](int value) {
        state.width_tiles = value;
        selected_tile = -1;
        showTileInfo();
        refresh();
    });
    form->addWidget(width_spin, row++, 1);
    side->addLayout(form);

    tile_info = addInfoLabel(side);

    connect(canvas, &PPUImageView::clicked, this, [this](int x, int y) {
        const int tile = gv::tile_at_pixel(state, x, y);
        if (tile < 0)
            return;
        selected_tile = tile;
        showTileInfo();
    });

    resize(640, 620);
    refresh();
}

void GBTileViewerWindow::showTileInfo()
{
    if (selected_tile < 0)
    {
        tile_info->clear();
        return;
    }
    tile_info->setText(tr("Tile #%1  bank %2\nVRAM %3")
                           .arg(selected_tile)
                           .arg(state.bank)
                           .arg(hex(gv::tile_address(selected_tile), 4)));
}

void GBTileViewerWindow::refresh()
{
    runOnCore([&] { gv::render_tiles(state, picture); });
    canvas->setImage(picture);
}

void GBTileViewerWindow::exportTiles()
{
    // The grid is a reading aid, not part of the tiles.
    pv::Image exported;
    gv::TileViewerState without_grid = state;
    without_grid.show_grid = false;
    runOnCore([&] { gv::render_tiles(without_grid, exported); });
    exportImage(exported, QStringLiteral("gb_tiles.png"));
}

/* ---- GB Tilemap Viewer --------------------------------------------------- */

GBTilemapViewerWindow::GBTilemapViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("GB Tilemap Viewer"))
{
    canvas = new PPUImageView;
    auto side = buildFrame(this, tr("Map"), canvas, [this] { exportMap(); });

    auto grid_check = new QCheckBox(tr("Show Grid"));
    connect(grid_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.show_grid = checked;
        refresh();
    });
    addUpdateRows(this, side, canvas, 2, grid_check, &auto_update, [this] { refresh(); });

    auto form = new QGridLayout;
    form->setHorizontalSpacing(6);
    int row = 0;

    form->addWidget(new QLabel(tr("Map:")), row, 0);
    auto map_combo = new QComboBox;
    for (int i = 0; i < gv::kMapCount; i++)
        map_combo->addItem(tr(gv::map_name(i)));
    connect(map_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.map = index;
        refresh();
    });
    form->addWidget(map_combo, row++, 1);

    form->addWidget(new QLabel(tr("Tile Data:")), row, 0);
    auto tile_data_combo = new QComboBox;
    for (int i = 0; i < gv::kTileDataCount; i++)
        tile_data_combo->addItem(tr(gv::tile_data_name(i)));
    connect(tile_data_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.tile_data = index;
        refresh();
    });
    form->addWidget(tile_data_combo, row++, 1);

    auto viewport_check = new QCheckBox(tr("Show viewport"));
    viewport_check->setChecked(state.show_viewport);
    connect(viewport_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.show_viewport = checked;
        refresh();
    });
    form->addWidget(viewport_check, row++, 0, 1, 2);

    form->addWidget(new QLabel(tr("Background:")), row, 0);
    auto background_combo = new QComboBox;
    for (int i = 0; i < gv::kMapBackgroundCount; i++)
        background_combo->addItem(tr(gv::map_background_name(i)));
    connect(background_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.background = index;
        refresh();
    });
    form->addWidget(background_combo, row++, 1);
    side->addLayout(form);

    cell_info = addInfoLabel(side);

    connect(canvas, &PPUImageView::clicked, this, [this](int x, int y) {
        cell_x = x;
        cell_y = y;
        gv::TilemapCell cell;
        runOnCore([&] { cell = gv::tilemap_cell(state, cell_x, cell_y); });
        showCellInfo(cell);
    });

    resize(800, 600);
    refresh();
}

void GBTilemapViewerWindow::showCellInfo(const gv::TilemapCell &cell)
{
    if (cell_x < 0)
    {
        cell_info->clear();
        return;
    }
    if (cell.cgb)
        cell_info->setText(tr("Cell %1,%2  map %3\nTile %4  attr %5 (pal %6 bank %7)")
                               .arg(cell.x)
                               .arg(cell.y)
                               .arg(hex(cell.map_address, 4))
                               .arg((int)cell.tile)
                               .arg(hex(cell.attr, 2))
                               .arg(cell.attr & 7)
                               .arg((cell.attr >> 3) & 1));
    else
        cell_info->setText(tr("Cell %1,%2  map %3\nTile %4")
                               .arg(cell.x)
                               .arg(cell.y)
                               .arg(hex(cell.map_address, 4))
                               .arg((int)cell.tile));
}

void GBTilemapViewerWindow::refresh()
{
    gv::TilemapCell cell;
    runOnCore([&] {
        gv::render_tilemap(state, picture);
        if (cell_x >= 0)
            cell = gv::tilemap_cell(state, cell_x, cell_y);
    });
    canvas->setImage(picture);
    showCellInfo(cell);
}

void GBTilemapViewerWindow::exportMap()
{
    // Grid and viewport are reading aids, not part of the map.
    pv::Image exported;
    gv::TilemapViewerState plain = state;
    plain.show_grid = false;
    plain.show_viewport = false;
    runOnCore([&] { gv::render_tilemap(plain, exported, true); });
    exportImage(exported, QStringLiteral("gb_tilemap.png"));
}

/* ---- GB Sprite Viewer ---------------------------------------------------- */

GBSpriteViewerWindow::GBSpriteViewerWindow(EmuMainWindow *parent, EmuApplication *app_)
    : PPUViewerWindow(parent, app_, tr("GB Sprite Viewer"))
{
    canvas = new PPUImageView;
    auto side = buildFrame(this, tr("Sprites"), canvas, [this] { exportSprites(); });

    auto viewport_check = new QCheckBox(tr("Show screen"));
    viewport_check->setChecked(state.show_viewport);
    connect(viewport_check, &QCheckBox::toggled, this, [this](bool checked) {
        state.show_viewport = checked;
        refresh();
    });
    addUpdateRows(this, side, canvas, 2, viewport_check, &auto_update, [this] { refresh(); });

    auto form = new QGridLayout;
    form->setHorizontalSpacing(6);
    int row = 0;

    form->addWidget(new QLabel(tr("Sprite:")), row, 0);
    auto sprite_spin = new QSpinBox;
    sprite_spin->setRange(0, gv::kSpriteCount - 1);
    connect(sprite_spin, &QSpinBox::valueChanged, this, [this](int value) {
        state.selected = value;
        refresh();
    });
    form->addWidget(sprite_spin, row++, 1);

    form->addWidget(new QLabel(tr("Background:")), row, 0);
    auto background_combo = new QComboBox;
    for (int i = 0; i < pv::kViewerBgCount; i++)
        background_combo->addItem(tr(pv::viewer_bg_name(i)));
    background_combo->setCurrentIndex(state.background);
    connect(background_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.background = index;
        refresh();
    });
    form->addWidget(background_combo, row++, 1);
    side->addLayout(form);

    sprite_info = addInfoLabel(side);

    resize(800, 600);
    refresh();
}

void GBSpriteViewerWindow::refresh()
{
    gv::SpriteInfo s;
    runOnCore([&] {
        gv::render_sprites(state, picture);
        s = gv::sprite_info(state.selected);
    });
    canvas->setImage(picture);

    if (!s.valid)
    {
        sprite_info->clear();
        return;
    }
    const QString flips = QString((s.flags & 0x40) ? "Yflip " : "") +
                          ((s.flags & 0x20) ? "Xflip " : "") + ((s.flags & 0x80) ? "BGprio" : "");
    QString text = tr("Sprite %1\nX %2 Y %3 (screen %4,%5)\nTile %6  flags %7")
                       .arg(state.selected)
                       .arg(s.x)
                       .arg(s.y)
                       .arg(s.x - 8)
                       .arg(s.y - 16)
                       .arg(s.tile)
                       .arg(hex(s.flags, 2));
    if (s.cgb)
        text += tr("\npal %1 bank %2  %3").arg(s.flags & 7).arg((s.flags >> 3) & 1).arg(flips);
    else
        text += tr("\nOBP%1  %2").arg((s.flags >> 4) & 1).arg(flips);
    sprite_info->setText(text);
}

void GBSpriteViewerWindow::exportSprites()
{
    // The screen outline is a reading aid, and Transparent exports as alpha 0.
    pv::Image exported;
    gv::SpriteViewerState plain = state;
    plain.show_viewport = false;
    runOnCore([&] { gv::render_sprites(plain, exported, true); });
    exportImage(exported, QStringLiteral("gb_sprites.png"));
}
