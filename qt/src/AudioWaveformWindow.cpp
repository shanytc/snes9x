#include "AudioWaveformWindow.hpp"
#include "EmuApplication.hpp"
#include "EmuConfig.hpp"
#include "EmuMainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QVBoxLayout>
#include <QtEvents>
#include <algorithm>

namespace aw = audiowave;

namespace
{

QColor toQColor(aw::Color c)
{
    return QColor(c.r, c.g, c.b);
}

/* [R]ecord, [M]ute and [S]olo button rects inside a row: the right side of
 * the header, vertically centered. Shared by the painter and the hit test. */
QRect recRect(const QRect &row)
{
    return QRect(row.left() + aw::kHeaderWidth - 84, row.top() + row.height() / 2 - 9, 22, 18);
}

QRect muteRect(const QRect &row)
{
    return QRect(row.left() + aw::kHeaderWidth - 58, row.top() + row.height() / 2 - 9, 22, 18);
}

QRect soloRect(const QRect &row)
{
    return QRect(row.left() + aw::kHeaderWidth - 32, row.top() + row.height() / 2 - 9, 22, 18);
}

} // namespace

class AudioWaveformWindow::TrackArea : public QWidget
{
  public:
    explicit TrackArea(AudioWaveformWindow *owner)
        : QWidget(owner), owner(owner)
    {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        buffer.resize(aw::kSnapshotFrames * 2);
    }

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    /* Rows split the height evenly; the last one takes the remainder. */
    QRect rowRect(int index, int panels) const
    {
        const int h = height() / panels;
        return QRect(0, index * h, width(), (index == panels - 1) ? height() - index * h : h);
    }
    void drawRow(QPainter &p, const QRect &r, int src, const int16_t *lr, int n, bool show_axis);

    AudioWaveformWindow *owner;
    std::vector<int16_t> buffer;
    std::vector<int> y_min, y_max;
    std::vector<QLine> lines;
};

void AudioWaveformWindow::TrackArea::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 18, 20));
    QFont f = font();
    f.setPixelSize(12);
    p.setFont(f);

    auto &viewer = owner->viewer;
    int order[aw::kSourceCount];
    const int panels = viewer.build_order(order);
    if (height() / panels <= 0)
        return;

    viewer.begin_paint();
    for (int i = 0; i < panels; i++)
    {
        const int src = order[i];
        const int n = viewer.fetch(src, buffer.data(), aw::kSnapshotFrames);
        drawRow(p, rowRect(i, panels), src, buffer.data(), n, i == panels - 1);
    }
    viewer.end_paint();
}

void AudioWaveformWindow::TrackArea::drawRow(QPainter &p, const QRect &r, int src,
                                             const int16_t *lr, int n, bool show_axis)
{
    auto &viewer = owner->viewer;
    const aw::TrackInfo &track = aw::kTracks[src];
    const QColor color = toQColor(track.color);
    const bool group = aw::is_group(src);
    const bool expanded = (src == aw::kSPC) ? viewer.spc_open : viewer.gb_open;
    const bool user_muted = aw::is_muted(src);
    const bool soloed = aw::is_soloed(src);
    const bool audible = aw::is_audible(src);
    const int cy = r.top() + r.height() / 2;

    // Header strip: color tab, disclosure triangle on group rows, name.
    const QRect header(r.left(), r.top(), aw::kHeaderWidth, r.height());
    p.fillRect(header, group ? QColor(56, 56, 60) : QColor(40, 40, 44));
    p.fillRect(QRect(header.left(), header.top(), 5, header.height()), color);

    if (group)
    {
        const int ix = header.left() + 10;
        QPolygon triangle;
        if (expanded)
            triangle << QPoint(ix, cy - 3) << QPoint(ix + 10, cy - 3) << QPoint(ix + 5, cy + 4);
        else
            triangle << QPoint(ix + 2, cy - 5) << QPoint(ix + 2, cy + 5) << QPoint(ix + 9, cy);
        const QColor tc = toQColor(aw::tint(track.color, 30));
        p.setPen(tc);
        p.setBrush(tc);
        p.drawPolygon(triangle);
        p.setBrush(Qt::NoBrush);
    }

    p.setPen(audible ? QColor(225, 225, 228) : QColor(130, 130, 135));
    p.drawText(QRect(header.left() + (group ? 26 : 30), cy - 8, 40, 16),
               Qt::AlignLeft | Qt::AlignVCenter, track.name);

    auto button = [&p](const QRect &b, const char *label, bool on,
                       QColor fill_on, QColor frame_on, QColor text_on) {
        p.fillRect(b, on ? fill_on : QColor(52, 52, 56));
        p.setPen(on ? frame_on : QColor(80, 80, 86));
        p.drawRect(b.adjusted(0, 0, -1, -1));
        p.setPen(on ? text_on : QColor(150, 150, 156));
        p.drawText(b, Qt::AlignCenter, label);
    };
    const bool rec_this = viewer.recorder.source() == src;
    button(recRect(r), "R", rec_this, QColor(200, 40, 40), QColor(240, 90, 90), Qt::white);
    button(muteRect(r), "M", user_muted, QColor(96, 150, 250), QColor(140, 180, 255), QColor(20, 30, 60));
    // The MIX row is the master bus, so it has no solo.
    if (src != aw::kMix)
        button(soloRect(r), "S", soloed, QColor(230, 192, 62), QColor(250, 220, 120), QColor(60, 45, 10));

    // Waveform lane, dimmed when the track doesn't reach the output, whether
    // by its own mute or by someone else's solo.
    const int axis_h = show_axis ? 16 : 0;
    const QRect lane(r.left() + aw::kHeaderWidth, r.top(), r.width() - aw::kHeaderWidth, r.height());
    const QRect body(lane.left(), lane.top(), lane.width(), lane.height() - axis_h);
    p.fillRect(body, toQColor(aw::shade(track.color, audible ? 28 : 14)));
    const int mid_y = body.top() + body.height() / 2;
    p.setPen(toQColor(aw::shade(track.color, audible ? 44 : 24)));
    p.drawLine(body.left(), mid_y, body.right(), mid_y);

    viewer.build_envelope(lr, n, body.width(), body.height(), y_min, y_max);
    if (!y_min.empty())
    {
        lines.resize(y_min.size());
        for (size_t i = 0; i < y_min.size(); i++)
        {
            const int x = body.left() + (int)i;
            lines[i] = QLine(x, body.top() + y_min[i], x, body.top() + y_max[i]);
        }
        p.setPen(toQColor(audible ? aw::tint(track.color, 55) : aw::shade(track.color, 52)));
        p.drawLines(lines.data(), (int)lines.size());
    }

    // L/R meters under the buttons: -48..0 dB with 1 s peak-hold ticks.
    if (r.height() >= 46)
    {
        float level[2], peak[2];
        viewer.meter_levels(src, lr, n, level, peak);
        const int mx0 = header.left() + 8;
        const int mw = header.width() - 16;
        for (int ch = 0; ch < 2; ch++)
        {
            const int y0 = cy + 13 + ch * 4;
            p.fillRect(QRect(mx0, y0, mw, 3), QColor(30, 30, 32));
            float from = 0.0f;
            for (int s = 0; s < 3 && from < level[ch]; s++)
            {
                const float to = std::min(level[ch], aw::kMeterSegments[s].to);
                if (to > from)
                {
                    const int x0 = mx0 + (int)(from * mw);
                    const int x1 = mx0 + (int)(to * mw);
                    p.fillRect(QRect(x0, y0, x1 - x0, 3), toQColor(aw::kMeterSegments[s].color));
                }
                from = aw::kMeterSegments[s].to;
            }
            if (peak[ch] > 0.01f)
                p.fillRect(QRect(mx0 + (int)(peak[ch] * (mw - 2)), y0, 2, 3), QColor(220, 220, 224));
        }
    }

    // Clip-label style info at the top-left of the lane, status at the right.
    const std::string info = viewer.info_text(src);
    if (!info.empty())
    {
        p.setPen(toQColor(aw::tint(track.color, audible ? 70 : 25)));
        p.drawText(QRect(lane.left() + 6, lane.top() + 2, lane.width() - 12, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(info));
    }
    if (user_muted)
    {
        p.setPen(toQColor(aw::tint(track.color, 40)));
        p.drawText(QRect(lane.right() - 52, lane.top() + 2, 50, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, tr("muted"));
    }
    if (rec_this)
    {
        const int seconds = viewer.recorder.elapsed_seconds();
        p.setPen(QColor(235, 80, 80));
        p.drawText(QRect(lane.right() - 140, lane.top() + 2, 86, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QString("REC %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0')));
    }

    if (show_axis)
    {
        const QRect axis(lane.left(), body.top() + body.height(), lane.width(), axis_h);
        p.fillRect(axis, QColor(18, 18, 20));
        p.setPen(QColor(140, 140, 145));
        const int rate = viewer.sample_rate(src);
        for (int i = 1; i < 7; i++)
        {
            const int x = lane.left() + (lane.width() * i) / 7;
            p.drawText(QRect(x - 22, axis.top() + 1, 44, 14), Qt::AlignCenter,
                       QString::fromStdString(aw::Viewer::axis_label(i, n, rate)));
        }
        p.drawText(QRect(lane.left() + 2, axis.top() + 1, 20, 14), Qt::AlignLeft | Qt::AlignVCenter, "0");
    }

    p.setPen(QColor(20, 20, 22));
    p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
}

void AudioWaveformWindow::TrackArea::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    auto &viewer = owner->viewer;
    int order[aw::kSourceCount];
    const int panels = viewer.build_order(order);
    const int h = height() / panels;
    if (h <= 0)
        return;
    const QPoint pos = event->position().toPoint();
    const int index = std::clamp(pos.y() / h, 0, panels - 1);
    const int src = order[index];
    const QRect row = rowRect(index, panels);

    if (recRect(row).contains(pos))
        owner->toggleRecording(src);
    else if (muteRect(row).contains(pos))
        aw::toggle_mute(src);
    else if (src != aw::kMix && soloRect(row).contains(pos))
        aw::toggle_solo(src);
    // Anywhere else on a group row toggles its expansion.
    else if (src == aw::kSPC)
        viewer.spc_open = !viewer.spc_open;
    else if (src == aw::kGB)
        viewer.gb_open = !viewer.gb_open;
    else
        return;
    update();
}

AudioWaveformWindow::AudioWaveformWindow(EmuMainWindow *parent, EmuApplication *app_)
    : QWidget(parent, Qt::Window), main_window(parent), app(app_)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Audio Waveform  (click SPC/GB to expand channels)"));
    resize(800, 560);

    // The MIX row's [M] is the Sound panel's "Mute all sound".
    aw::set_master_mute_hooks(
        [app_] { return app_->config->mute_audio; },
        [app_](bool muted) {
            app_->config->mute_audio = muted;
            app_->updateSettings();
        });

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tracks = new TrackArea(this);
    layout->addWidget(tracks, 1);

    auto footer = new QHBoxLayout;
    footer->setContentsMargins(8, 4, 8, 4);
    footer->setSpacing(8);

    auto reset_box = new QCheckBox(tr("reset channels on close"));
    reset_box->setChecked(viewer.reset_on_close);
    connect(reset_box, &QCheckBox::toggled, [this](bool on) { viewer.reset_on_close = on; });
    footer->addWidget(reset_box);

    footer->addSpacing(12);
    footer->addWidget(new QLabel(tr("zoom")));
    auto zoom = new QComboBox;
    for (int i = 0; i < aw::kScaleCount; i++)
        zoom->addItem(aw::kScaleLabels[i]);
    zoom->setCurrentIndex(viewer.scale);
    connect(zoom, &QComboBox::currentIndexChanged, [this](int index) {
        viewer.scale = index;
        tracks->update();
    });
    footer->addWidget(zoom);

    footer->addSpacing(12);
    footer->addWidget(new QLabel(tr("rate")));
    auto rate = new QComboBox;
    for (int i = 0; i < aw::kRefreshCount; i++)
        rate->addItem(aw::kRefreshOptions[i].label);
    rate->setCurrentIndex(viewer.refresh);
    connect(rate, &QComboBox::currentIndexChanged, [this](int index) {
        viewer.refresh = index;
        applyRefresh();
    });
    footer->addWidget(rate);

    footer->addSpacing(12);
    auto nerd_box = new QCheckBox(tr("stats for nerds"));
    nerd_box->setChecked(viewer.nerd_stats);
    connect(nerd_box, &QCheckBox::toggled, [this](bool on) {
        viewer.nerd_stats = on;
        tracks->update();
    });
    footer->addWidget(nerd_box);
    footer->addStretch(1);
    layout->addLayout(footer);

    connect(&timer, &QTimer::timeout, [this] {
        viewer.recorder.pump();
        if (!isMinimized())
            tracks->update();
    });

    viewer.open();
    applyRefresh();
}

AudioWaveformWindow::~AudioWaveformWindow()
{
    timer.stop();
    viewer.close();
}

void AudioWaveformWindow::closeEvent(QCloseEvent *event)
{
    timer.stop();
    viewer.close();
    event->accept();
}

bool AudioWaveformWindow::event(QEvent *event)
{
    // For "pause emulation when unfocused" this window is part of the main
    // window: moving between the two doesn't pause, leaving from here does.
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

void AudioWaveformWindow::applyRefresh()
{
    timer.start(aw::kRefreshOptions[viewer.refresh].ms);
}

void AudioWaveformWindow::toggleRecording(int src)
{
    auto &recorder = viewer.recorder;
    // One track at a time; clicking the armed one stops and saves.
    if (recorder.source() == src)
    {
        recorder.stop();
        auto filename = QFileDialog::getSaveFileName(
            this, tr("Save Recording"), QString("%1.wav").arg(aw::kTracks[src].name),
            tr("WAV audio (*.wav)"));
        if (filename.isEmpty())
            recorder.discard();
        else if (!recorder.write_wav(filename.toStdString()))
            QMessageBox::warning(this, tr("Save Recording"),
                                 tr("Couldn't write %1").arg(filename));
    }
    else if (recorder.source() < 0)
    {
        recorder.start(src);
    }
}
