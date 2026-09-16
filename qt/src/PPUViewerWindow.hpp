#pragma once
#include <QColor>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "common/video/ppu_viewer.hpp"

class EmuApplication;
class EmuMainWindow;

/* The picture half of the Emulation > S-PPU viewers: a canvas that paints
 * one ppuviewer::Image at a whole-number zoom, reports clicks back in source
 * pixels, and asks for a context menu. Put it in a QScrollArea and it pans
 * itself; win32's dialogs drag-pan a fixed canvas instead. */
class PPUImageView : public QWidget
{
    Q_OBJECT

  public:
    explicit PPUImageView(QWidget *parent = nullptr);

    void setImage(const ppuviewer::Image &image);
    void setZoom(int zoom);
    int zoom() const { return zoom_factor; }
    /* Scale to the widget at the largest whole multiple that fits, centered,
     * rather than to the zoom factor. What the sprite preview wants. */
    void setFitToWidget(bool fit);
    void setBackground(QColor color);

    const QImage &image() const { return picture; }

  Q_SIGNALS:
    void clicked(int x, int y); // in source pixels
    void contextMenuAt(const QPoint &global_position);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    void applySize();

    QImage picture;
    int zoom_factor = 1;
    bool fit_to_widget = false;
    QColor background = QColor(0, 0, 0);
};

/* CGRAM as a 16x16 grid of swatches; clicking one picks the palette offset
 * the Tile Viewer indexes tiles through. */
class PPUPaletteView : public QWidget
{
    Q_OBJECT

  public:
    explicit PPUPaletteView(QWidget *parent = nullptr);

    void setPalette(const ppuviewer::Pixel colors[256]);
    void setSelected(int index);
    int selected() const { return selected_index; }

  Q_SIGNALS:
    void selectionChanged(int index);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    ppuviewer::Pixel colors[256] = {};
    int selected_index = 0;
};

/* What the three viewer windows share: they live beside the main window for
 * "pause emulation when unfocused", they re-render on a timer while Auto
 * update is on, and every render runs on the emulation thread so the PPU
 * state it reads cannot move underneath it. */
class PPUViewerWindow : public QWidget
{
    Q_OBJECT

  public:
    PPUViewerWindow(EmuMainWindow *parent, EmuApplication *app, const QString &title);
    ~PPUViewerWindow() override;

  protected:
    bool event(QEvent *event) override;

    /* Runs on the emulation thread and waits for it. */
    void runOnCore(const std::function<void()> &function);
    void setUpdateInterval(int milliseconds);
    /* Re-read the PPU and repaint. Called by the timer and the Refresh button. */
    virtual void refresh() = 0;

    /* Save-as dialog plus the PNG write, with a message box if it fails. */
    void exportImage(const ppuviewer::Image &image, const QString &default_name);

    EmuMainWindow *main_window = nullptr;
    EmuApplication *app = nullptr;
    bool auto_update = true;

  private:
    QTimer timer;
};

/* A zoom combo carrying 1x..9x, with `zoom` selected. */
class QComboBox;
void ppuFillZoomCombo(QComboBox *combo, int zoom);
