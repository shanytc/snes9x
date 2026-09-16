#pragma once
#include "PPUViewerWindow.hpp"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

/* Emulation > S-PPU > Sprite Viewer, as win32's CSpriteViewerDlg offers it:
 * every object composed where it sits at its priority, a list that can hide
 * them one at a time, and a preview of whichever one is selected. */
class SpriteViewerWindow : public PPUViewerWindow
{
    Q_OBJECT

  public:
    SpriteViewerWindow(EmuMainWindow *parent, EmuApplication *app);

  protected:
    void refresh() override;

  private:
    void createWidgets();
    void updateList();
    void drawScreen();
    void drawPreview();
    void showListMenu(const QPoint &global_position);
    std::vector<int> selectedSprites() const;
    void exportSelected();
    void exportScreen();
    void exportPreview();

    ppuviewer::OAMSnapshot oam;
    ppuviewer::Image screen_picture;
    ppuviewer::Image preview_picture;
    bool visible[128] = {};
    int selected = 0;
    int background = ppuviewer::kBackgroundTransparent;
    bool show_outline = true;
    /* Guards the bulk check-state updates the list's context menu makes, so
     * the screen is redrawn once instead of 128 times. */
    bool updating_list = false;

    /* Only the widgets something outside createWidgets() writes to. */
    PPUImageView *screen_view = nullptr;
    QTreeWidget *list = nullptr;
    PPUImageView *preview = nullptr;
    QLabel *first_sprite_label = nullptr;
    QLabel *details_label = nullptr;
};
