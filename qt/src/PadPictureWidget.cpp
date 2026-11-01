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

static int PictureWidth(bool pachinko)
{
    return pachinko ? S9X_PACHINKOPIC_WIDTH : S9X_PADPIC_WIDTH;
}

static int PictureHeight(bool pachinko)
{
    return pachinko ? S9X_PACHINKOPIC_HEIGHT : S9X_PADPIC_HEIGHT;
}

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
    // The Pachinko's picture already has a transparent surround
    if (pachinko)
    {
        source = QImage(":/pads/pad_pachinko.png").convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return;
    }
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

void PadPictureWidget::setPachinko(bool on)
{
    if (on == pachinko)
        return;
    pachinko = on;
    lit = marked = hover = 0;
    unsetCursor();
    loadSource();
    rescale();
    update();
}

void PadPictureWidget::setPower(float level)
{
    if (level == power)
        return;
    power = level;
    render();
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
    return std::min(kPadPictureScale, (float)width() / PictureWidth(pachinko));
}

// Centred along the top, in logical pixels.
QRectF PadPictureWidget::pictureRect() const
{
    const float s = pictureScale();
    const float w = PictureWidth(pachinko) * s, h = PictureHeight(pachinko) * s;
    return QRectF((width() - w) / 2, 0, w, h);
}

int PadPictureWidget::buttonAt(QPointF pos) const
{
    const QRectF r = pictureRect();
    const float s = pictureScale();
    if (!r.contains(pos) || s <= 0)
        return 0;
    const float x = (pos.x() - r.left()) / s, y = (pos.y() - r.top()) / s;
    return pachinko ? S9xPachinkoPictureButtonAt(x, y, PACHPIC_ALL) : S9xPadPictureButtonAt(x, y, PADPIC_ALL);
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
    const float s = (float)shown.width() / PictureWidth(pachinko);
    if (pachinko)
    {
        S9xPachinkoPictureDraw((uint32_t *)shown.bits(), shown.width(), shown.height(), shown.bytesPerLine() / 4,
                               s, lit, marked | hover, true);
        S9xPachinkoPictureDrawPower((uint32_t *)shown.bits(), shown.width(), shown.height(), shown.bytesPerLine() / 4,
                                    s, power, true);
    }
    else
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
    if (!pachinko)
        return;

    // The power as text in the picture's top-left corner, coloured as the gauge
    const QRectF r = pictureRect();
    const float s = pictureScale();
    QFont font = painter.font();
    font.setPixelSize(std::max(1, (int)std::lround(S9X_PACHINKOPIC_LABEL_SIZE * s)));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    QColor label = palette().color(QPalette::WindowText);
    label.setAlphaF(0.7);
    painter.setPen(label);
    QPointF at(r.left() + S9X_PACHINKOPIC_POWER_X * s, r.top() + S9X_PACHINKOPIC_POWER_Y * s);
    painter.drawText(at + QPointF(0, painter.fontMetrics().ascent()), "POWER");
    at.ry() += painter.fontMetrics().height();

    font.setPixelSize(std::max(1, (int)std::lround(S9X_PACHINKOPIC_VALUE_SIZE * s)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor::fromRgb(S9xPachinkoPowerColour(power)));
    painter.drawText(at + QPointF(0, painter.fontMetrics().ascent()), QString("%1%").arg((int)std::lround(power * 100)));
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
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    const int button = buttonAt(event->position());
    if (button && buttonClicked)
        buttonClicked(button);
    else if (!button && backgroundClicked)
        backgroundClicked();
    event->accept();
}

void PadPictureWidget::leaveEvent(QEvent *event)
{
    setHover(0);
    QWidget::leaveEvent(event);
}

void PadPictureWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (pachinko || !pictureRect().contains(event->pos()))
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
