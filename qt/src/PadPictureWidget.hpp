#pragma once
#include <QWidget>
#include <QImage>
#include <functional>

// The Controllers panel's pad picture: held buttons lit, the hovered and edited ones ringed
// (padpicture.cpp draws them), a click picks a button and the right-click menu the pad style.
class PadPictureWidget : public QWidget
{
  public:
    explicit PadPictureWidget(QWidget *parent = nullptr);
    void setPadStyle(int style);
    void setLit(int buttons);
    void setMarked(int buttons);

    std::function<void(int button)> buttonClicked;
    std::function<void(int style)> styleChosen;

  protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    float pictureScale() const;
    QRectF pictureRect() const;
    int buttonAt(QPointF pos) const;
    void setHover(int button);
    void loadSource();
    void rescale();
    void render();

    int pad_style = 0;
    int lit = 0;
    int marked = 0;
    int hover = 0;
    QImage source;	// the style's picture, full size, its surround transparent
    QImage base;	// scaled to the widget's device pixels, unlit
    QImage shown;	// base with the buttons lit
    qreal base_dpr = 0;
};
