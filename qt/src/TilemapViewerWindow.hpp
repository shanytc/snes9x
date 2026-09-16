#pragma once
#include "PPUViewerWindow.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

/* Emulation > S-PPU > Tilemap Viewer, as win32's CTilemapViewerDlg offers
 * it: one background layer drawn whole, either as the PPU has it set up or
 * as a screen mode, map and character addresses the user names instead. */
class TilemapViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    TilemapViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void createWidgets();
    void applyEnabledState();
    void seedAutomaticFields(const ppuviewer::TilemapInfo &info);
    void readOverrideFields();
    void rerender();
    void exportTilemap();

    ppuviewer::TilemapViewerState state;
    ppuviewer::Image picture;

    /* Only the controls something outside createWidgets() reads or writes. */
    QSpinBox *mode_spin = nullptr;
    QComboBox *bit_depth_combo = nullptr;
    QComboBox *map_size_combo = nullptr;
    QComboBox *tile_size_combo = nullptr;
    QLineEdit *map_address_edit = nullptr;
    QLineEdit *tile_address_edit = nullptr;
    QLabel *info_label = nullptr;
    PPUImageView *canvas = nullptr;
};
