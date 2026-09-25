#pragma once
#include "PPUViewerWindow.hpp"
#include "common/video/gb_ppu_viewer.hpp"

class QLabel;

/* Emulation > GB-PPU > GB Tile Viewer, as win32's CGBTileViewerDlg offers it:
 * the 384 tiles of one VRAM bank through a DMG, CGB or SGB palette. */
class GBTileViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    GBTileViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void showTileInfo();
    void exportTiles();

    gbviewer::TileViewerState state;
    ppuviewer::Image picture;
    int selected_tile = -1;

    QLabel *tile_info = nullptr;
    PPUImageView *canvas = nullptr;
};

/* Emulation > GB-PPU > GB Tilemap Viewer, as win32's CGBTilemapViewerDlg
 * offers it: the BG or window map, with the screen the game is scrolled to. */
class GBTilemapViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    GBTilemapViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void showCellInfo(const gbviewer::TilemapCell &cell);
    void exportMap();

    gbviewer::TilemapViewerState state;
    ppuviewer::Image picture;
    int cell_x = -1, cell_y = -1; // source pixel of the picked cell

    QLabel *cell_info = nullptr;
    PPUImageView *canvas = nullptr;
};

/* Emulation > GB-PPU > GB Sprite Viewer, as win32's CGBSpriteViewerDlg offers
 * it: all 40 objects where OAM puts them, the visible screen outlined, and one
 * picked out for its attributes. */
class GBSpriteViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    GBSpriteViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void exportSprites();

    gbviewer::SpriteViewerState state;
    ppuviewer::Image picture;

    QLabel *sprite_info = nullptr;
    PPUImageView *canvas = nullptr;
};
