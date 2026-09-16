#include "PPUViewerWindow.hpp"
#include "EmuApplication.hpp"
#include "EmuMainWindow.hpp"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

/* ---- PPUImageView -------------------------------------------------------- */

PPUImageView::PPUImageView(QWidget *parent)
    : QWidget(parent)
{
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void PPUImageView::setImage(const ppuviewer::Image &image)
{
    if (image.empty())
    {
        picture = QImage();
        applySize();
        update();
        return;
    }

    /* QImage does not own the pixels it is handed, so copy them: the source
     * buffer belongs to the caller's render and is about to be reused. */
    picture = QImage((const uchar *)image.pixels.data(), image.width, image.height,
                     image.width * 4, QImage::Format_ARGB32)
                  .copy();
    applySize();
    update();
}

void PPUImageView::setZoom(int zoom)
{
    zoom_factor = std::max(1, zoom);
    applySize();
    update();
}

void PPUImageView::setFitToWidget(bool fit)
{
    fit_to_widget = fit;
    setSizePolicy(fit ? QSizePolicy::Expanding : QSizePolicy::Fixed,
                  fit ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    if (fit)
    {
        // Drop whatever setFixedSize left behind; from here the widget's size
        // is the caller's business and every picture is scaled into it.
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    applySize();
    update();
}

void PPUImageView::setBackground(QColor color)
{
    background = color;
    update();
}

void PPUImageView::applySize()
{
    if (fit_to_widget)
        return;
    if (picture.isNull())
    {
        setFixedSize(1, 1);
        return;
    }
    setFixedSize(picture.width() * zoom_factor, picture.height() * zoom_factor);
}

void PPUImageView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), background);
    if (picture.isNull())
        return;

    if (!fit_to_widget)
    {
        painter.drawImage(QRect(0, 0, picture.width() * zoom_factor,
                                picture.height() * zoom_factor),
                          picture);
        return;
    }

    const int scale = std::max(1, std::min(width() / std::max(1, picture.width()),
                                           height() / std::max(1, picture.height())));
    const int w = picture.width() * scale;
    const int h = picture.height() * scale;
    painter.drawImage(QRect((width() - w) / 2, (height() - h) / 2, w, h), picture);
}

void PPUImageView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || picture.isNull())
    {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint position = event->position().toPoint();
    int x, y;
    if (fit_to_widget)
    {
        const int scale = std::max(1, std::min(width() / std::max(1, picture.width()),
                                               height() / std::max(1, picture.height())));
        x = (position.x() - (width() - picture.width() * scale) / 2) / scale;
        y = (position.y() - (height() - picture.height() * scale) / 2) / scale;
    }
    else
    {
        x = position.x() / zoom_factor;
        y = position.y() / zoom_factor;
    }

    if (x >= 0 && x < picture.width() && y >= 0 && y < picture.height())
        Q_EMIT clicked(x, y);
}

void PPUImageView::contextMenuEvent(QContextMenuEvent *event)
{
    Q_EMIT contextMenuAt(event->globalPos());
    event->accept();
}

/* ---- PPUPaletteView ------------------------------------------------------ */

PPUPaletteView::PPUPaletteView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    for (auto &color : colors)
        color = 0xFF000000u;
}

void PPUPaletteView::setPalette(const ppuviewer::Pixel new_colors[256])
{
    std::copy(new_colors, new_colors + 256, colors);
    update();
}

void PPUPaletteView::setSelected(int index)
{
    selected_index = std::clamp(index, 0, 255);
    update();
}

void PPUPaletteView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const double cell_w = width() / 16.0;
    const double cell_h = height() / 16.0;

    for (int i = 0; i < 256; i++)
    {
        const QRectF cell((i % 16) * cell_w, (i / 16) * cell_h, cell_w, cell_h);
        painter.fillRect(cell, QColor::fromRgb(colors[i]));
    }

    const QRectF cell((selected_index % 16) * cell_w, (selected_index / 16) * cell_h,
                      cell_w, cell_h);
    painter.setPen(QPen(Qt::white, 1));
    painter.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
}

void PPUPaletteView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint position = event->position().toPoint();
    const int column = std::clamp(position.x() * 16 / std::max(1, width()), 0, 15);
    const int row = std::clamp(position.y() * 16 / std::max(1, height()), 0, 15);
    setSelected(row * 16 + column);
    Q_EMIT selectionChanged(selected_index);
}

/* ---- PPUViewerWindow ----------------------------------------------------- */

PPUViewerWindow::PPUViewerWindow(EmuMainWindow *parent, EmuApplication *app_,
                                 const QString &title)
    : QWidget(parent, Qt::Window), main_window(parent), app(app_)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);

    connect(&timer, &QTimer::timeout, this, [this] {
        if (auto_update && isVisible())
            refresh();
    });
    timer.start(1000 / 60);
}

PPUViewerWindow::~PPUViewerWindow()
{
    timer.stop();
}

void PPUViewerWindow::setUpdateInterval(int milliseconds)
{
    timer.start(std::max(1, milliseconds));
}

void PPUViewerWindow::runOnCore(const std::function<void()> &function)
{
    app->emu_thread->runOnThread(function, true);
}

bool PPUViewerWindow::event(QEvent *event)
{
    /* For "pause emulation when unfocused" these windows are part of the main
     * window: moving between the two doesn't pause, leaving from here does. */
    switch (event->type())
    {
    case QEvent::WindowActivate:
        main_window->handleFocusChange(true);
        break;
    case QEvent::WindowDeactivate:
        // Not while closing: focus then goes back to the main window anyway.
        if (isVisible() && QApplication::activeWindow() != main_window)
            main_window->handleFocusChange(false);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void PPUViewerWindow::exportImage(const ppuviewer::Image &image, const QString &default_name)
{
    if (image.empty())
        return;

    const QString filename = QFileDialog::getSaveFileName(this, tr("Export to PNG"),
                                                          default_name,
                                                          tr("PNG Image (*.png)"));
    if (filename.isEmpty())
        return;

    if (!ppuviewer::write_png(filename.toStdString(), image))
        QMessageBox::warning(this, tr("Export"), tr("Failed to save PNG."));
}

void ppuFillZoomCombo(QComboBox *combo, int zoom)
{
    for (int i = 1; i <= 9; i++)
        combo->addItem(QStringLiteral("%1x").arg(i));
    combo->setCurrentIndex(std::clamp(zoom, 1, 9) - 1);
}
