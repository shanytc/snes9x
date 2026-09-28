#include "EmuCanvasQt.hpp"
#include "EmuConfig.hpp"

#include <QGuiApplication>
#include <QtEvents>
#include <QThread>
#include <QFontDatabase>
#include <QTime>
#include <ctime>

// Last, so snes9x.h's macros don't leak into the Qt headers above.
#include "snes9x.h"
#include "gfx.h"
#include "ppu.h"
#include "memmap.h"
#include "movie.h"

EmuCanvasQt::EmuCanvasQt(EmuApplication &app, QWidget *parent)
    : EmuCanvas(app, parent)
{
    setMinimumSize(256 / devicePixelRatioF(), 224 / devicePixelRatioF());
}

void EmuCanvasQt::deinit()
{
}

void EmuCanvasQt::draw()
{
    qimage_mutex.lock();
    if (qimage.width() != output_data.width || qimage.height() != output_data.height || qimage.format() != output_data.format)
    {
        qimage = QImage(output_data.width, output_data.height, output_data.format);
    }

    for (int y = 0; y < output_data.height; y++)
        memcpy(qimage.bits() + (output_data.width * 2 * y),
               &output_data.buffer[output_data.bytes_per_line * y],
               output_data.width * 2);

    qimage_mutex.unlock();
    throttle();
    update();
}

void EmuCanvasQt::paintEvent(QPaintEvent *event)
{
    // Called directly (on pause, say) rather than for a paint event: QPainter needs one.
    if (!event)
    {
        update();
        return;
    }

    // TODO: If emu not running
    if (!output_data.ready)
    {
        QPainter paint(this);
        paint.fillRect(QRect(0, 0, width(), height()), QBrush(QColor(0, 0, 0)));
        return;
    }

    QPainter paint(this);
    paint.setRenderHint(QPainter::SmoothPixmapTransform, config.bilinear_filter);
    QRect dest = { 0, 0, width(), height() };
    if (config.maintain_aspect_ratio)
    {
        paint.fillRect(QRect(0, 0, width(), height()), QBrush(QColor(0, 0, 0)));
        dest = applyAspect(dest);
    }

    qimage_mutex.lock();
    paint.drawImage(dest, qimage, QRect(0, 0, output_data.width, output_data.height));
    qimage_mutex.unlock();

    if (config.display_messages == EmuConfig::eOnscreen)
        drawOverlay(paint);
}

// "60 fps" over rendered/expected frames, counted as the ImGui overlay counts them.
static QString frameRateText()
{
    static uint32 last_count = 0, fps = 0;
    static time_t last_time = time(nullptr);

    uint32 run_ahead = (Settings.RunAhead > 0) ? (uint32)Settings.RunAhead + 1 : 1;
    time_t now = time(nullptr);
    if (now != last_time)
    {
        if (last_count < IPPU.TotalEmulatedFrames)
            fps = (IPPU.TotalEmulatedFrames - last_count) / (uint32)(now - last_time);
        last_time = now;
        last_count = IPPU.TotalEmulatedFrames;
    }

    return QString::asprintf("%u fps\n%02d/%02d", fps / run_ahead,
                             (int)(IPPU.DisplayedRenderedFrameCount * run_ahead),
                             (int)Memory.ROMFramesPerSecond);
}

// The on-screen display the Vulkan and OpenGL canvases draw with ImGui, as outlined text;
// the pause and fast-forward icons become words.
void EmuCanvasQt::drawOverlay(QPainter &paint)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(config.osd_size);
    font.setBold(true);
    paint.setFont(font);

    int margin = config.osd_size / 2;
    QRect area = rect().adjusted(margin, margin, -margin, -margin);

    auto text = [&](const QString &string, int flags) {
        paint.setPen(Qt::black);
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy)
                    paint.drawText(area.translated(dx, dy), flags, string);
        paint.setPen(Qt::white);
        paint.drawText(area, flags, string);
    };

    QString top_right;
    if (Settings.DisplayFrameRate)
        top_right = frameRateText();
    if (Settings.DisplayFrameNumber)
        top_right += (top_right.isEmpty() ? "#" : "\n#") + QString::number(IPPU.TotalEmulatedFrames);
    if (!top_right.isEmpty())
        text(top_right, Qt::AlignTop | Qt::AlignRight);

    if (Settings.DisplayIndicators)
    {
        if (Settings.Paused || Settings.ForcedPause)
            text(tr("Paused"), Qt::AlignTop | Qt::AlignLeft);
        else if (Settings.TurboMode)
            text(tr("Fast Forwarding"), Qt::AlignTop | Qt::AlignLeft);
    }

    QString message = QString::fromUtf8(GFX.InfoString.c_str());
    if (Settings.DisplayMovieFrame && S9xMovieActive())
    {
        if (!message.isEmpty() && !message.endsWith('\n'))
            message += '\n';
        message += QString::fromUtf8(GFX.FrameDisplayString);
    }
    if (!message.isEmpty())
        text(message, Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap);

    if (Settings.DisplayTime)
        text(QTime::currentTime().toString("hh:mm"), Qt::AlignBottom | Qt::AlignRight);
}

void EmuCanvasQt::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}
