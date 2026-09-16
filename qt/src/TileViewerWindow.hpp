#pragma once
#include "PPUViewerWindow.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

/* Emulation > S-PPU > Tile Viewer, as win32's CVRAMViewerDlg offers it:
 * decode any of the tile sources at 2/4/8bpp or Mode 7, at a chosen address,
 * width and object layout, through CGRAM or a grayscale ramp. The palette
 * strip picks the colour block, and the base-address readout jumps to what
 * the PPU is currently pointing each layer at. */
class TileViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    TileViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void createWidgets();
    void showAddress();
    void showTileInfo();
    void rerender();
    void gotoBaseAddress(int which);
    void exportTiles();

    ppuviewer::TileViewerState state;
    ppuviewer::Image picture;
    int selected_tile = -1;

    /* Only the controls something outside createWidgets() writes back to. */
    QCheckBox *cgram_check = nullptr;
    QComboBox *source_combo = nullptr;
    QComboBox *bit_depth_combo = nullptr;
    QLineEdit *address_edit = nullptr;
    PPUPaletteView *palette_view = nullptr;
    QLineEdit *base_address_edits[ppuviewer::kBaseAddressCount] = {};
    QLabel *tile_info = nullptr;
    PPUImageView *canvas = nullptr;
};
