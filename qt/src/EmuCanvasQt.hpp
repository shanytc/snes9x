#pragma once
#include "EmuCanvas.hpp"

#include <QPainter>
#include <mutex>

class EmuCanvasQt : public EmuCanvas
{
  public:
    EmuCanvasQt(EmuApplication &app, QWidget *parent);

    void deinit() override;
    void draw() override;

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void drawOverlay(QPainter &paint);
    void updateOverlayText();

    std::mutex qimage_mutex;
    QImage qimage;
    // The OSD's view of the emulation thread's IPPU/GFX, taken in draw().
    QString osd_top_right;
    QString osd_message;
};