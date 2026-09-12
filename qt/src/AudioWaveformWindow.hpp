#pragma once
#include <QTimer>
#include <QWidget>
#include <functional>
#include <vector>

#include "common/audio/audio_waveform.hpp"

class EmuApplication;
class EmuMainWindow;

/* Sound > Show Audio Waveform, as on win32: Logic-style track rows for the
 * SPC and GB pre-mixes (each expands to its voices / channels) and the final
 * mix, with record/mute/solo buttons and L/R level meters in each header,
 * and a footer for the reset-on-close, zoom, refresh rate and nerd stats
 * choices. */
class AudioWaveformWindow : public QWidget
{
  public:
    AudioWaveformWindow(EmuMainWindow *parent, EmuApplication *app);
    ~AudioWaveformWindow() override;

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;

  private:
    class TrackArea;
    friend class TrackArea;

    void applyRefresh();
    void toggleRecording(int src);

    EmuMainWindow *main_window;
    EmuApplication *app;
    audiowave::Viewer viewer;
    TrackArea *tracks = nullptr;
    QTimer timer;
};
