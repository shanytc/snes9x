#include "PadPictureWidget.hpp"
#include "padpicture.h"
#include <QPainter>
#include <QMenu>
#include <QtEvents>
#include <algorithm>
#include <cmath>

// As win32's Input Configuration: the picture at 0.7 of its size, less when narrower.
static const float kPadPictureScale = 0.7f;

static const char *const kPadPictureFiles[S9X_PADPIC_NUM_STYLES] = {
    ":/pads/pad_usa.png", ":/pads/pad_eur.png", ":/pads/pad_sfc.png"
};

PadPictureWidget::PadPictureWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFixedHeight((int)std::ceil(S9X_PADPIC_HEIGHT * kPadPictureScale));
    setMinimumWidth(S9X_PADPIC_WIDTH / 4);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    loadSource();
}

// The style's picture, its surround cleared and the L and R labels in the palette's text colour.
void PadPictureWidget::loadSource()
{
    source = QImage(kPadPictureFiles[pad_style]).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!source.isNull())
        S9xPadPictureMatte((uint32_t *)source.bits(), source.bytesPerLine() / 4,
                           palette().color(QPalette::WindowText).rgb() & 0xffffff);
}

void PadPictureWidget::setPadStyle(int style)
{
    if (style < 0 || style >= S9X_PADPIC_NUM_STYLES)
        style = S9X_PADPIC_USA;
    if (style == pad_style)
        return;
    pad_style = style;
    loadSource();
    rescale();
    update();
}

void PadPictureWidget::setLit(int buttons)
{
    if (buttons == lit)
        return;
    lit = buttons;
    render();
    update();
}

void PadPictureWidget::setMarked(int buttons)
{
    if (buttons == marked)
        return;
    marked = buttons;
    render();
    update();
}

float PadPictureWidget::pictureScale() const
{
    return std::min(kPadPictureScale, (float)width() / S9X_PADPIC_WIDTH);
}

// Centred along the top, in logical pixels.
QRectF PadPictureWidget::pictureRect() const
{
    const float s = pictureScale();
    const float w = S9X_PADPIC_WIDTH * s, h = S9X_PADPIC_HEIGHT * s;
    return QRectF((width() - w) / 2, 0, w, h);
}

int PadPictureWidget::buttonAt(QPointF pos) const
{
    const QRectF r = pictureRect();
    const float s = pictureScale();
    if (!r.contains(pos) || s <= 0)
        return 0;
    return S9xPadPictureButtonAt((pos.x() - r.left()) / s, (pos.y() - r.top()) / s, PADPIC_ALL);
}

void PadPictureWidget::setHover(int button)
{
    if (button == hover)
        return;
    hover = button;
    if (hover)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    render();
    update();
}

// The picture scaled to the widget in device pixels, so the lights stay sharp on high-DPI screens.
void PadPictureWidget::rescale()
{
    base = QImage();
    base_dpr = devicePixelRatioF();
    const QRectF r = pictureRect();
    const int w = (int)std::lround(r.width() * base_dpr), h = (int)std::lround(r.height() * base_dpr);
    if (!source.isNull() && w > 0 && h > 0)
        base = source.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    render();
}

void PadPictureWidget::render()
{
    shown = QImage();
    if (base.isNull())
        return;
    shown = base.copy();
    const float s = (float)shown.width() / S9X_PADPIC_WIDTH;
    S9xPadPictureDraw((uint32_t *)shown.bits(), shown.width(), shown.height(), shown.bytesPerLine() / 4,
                      s, pad_style, lit, marked | hover, true);
    shown.setDevicePixelRatio(base_dpr);
}

void PadPictureWidget::paintEvent(QPaintEvent *)
{
    if (base_dpr != devicePixelRatioF())
        rescale();
    if (shown.isNull())
        return;
    QPainter painter(this);
    painter.drawImage(pictureRect().topLeft(), shown);
}

void PadPictureWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        loadSource();
        rescale();
        update();
    }
}

void PadPictureWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rescale();
}

void PadPictureWidget::mouseMoveEvent(QMouseEvent *event)
{
    setHover(buttonAt(event->position()));
    QWidget::mouseMoveEvent(event);
}

void PadPictureWidget::mousePressEvent(QMouseEvent *event)
{
    const int button = event->button() == Qt::LeftButton ? buttonAt(event->position()) : 0;
    if (button && buttonClicked)
    {
        buttonClicked(button);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PadPictureWidget::leaveEvent(QEvent *event)
{
    setHover(0);
    QWidget::leaveEvent(event);
}

void PadPictureWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!pictureRect().contains(event->pos()))
        return;

    QMenu menu(this);
    const QString names[S9X_PADPIC_NUM_STYLES] = {
        QObject::tr("USA Controller"), QObject::tr("European Controller"), QObject::tr("Japanese Controller")
    };
    QAction *actions[S9X_PADPIC_NUM_STYLES];
    for (int i = 0; i < S9X_PADPIC_NUM_STYLES; i++)
    {
        actions[i] = menu.addAction(names[i]);
        actions[i]->setCheckable(true);
        actions[i]->setChecked(i == pad_style);
    }

    QAction *chosen = menu.exec(event->globalPos());
    for (int i = 0; i < S9X_PADPIC_NUM_STYLES; i++)
        if (chosen == actions[i] && i != pad_style)
        {
            setPadStyle(i);
            if (styleChosen)
                styleChosen(i);
        }
}
