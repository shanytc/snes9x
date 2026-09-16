#include "EmuCanvas.hpp"
#include "EmuConfig.hpp"
#include "common/video/screen_content.hpp"
#include "snes9x.h"
#include "ppu.h"
#include <qnamespace.h>
#include <qwidget.h>

void S9xQtDisplayAspect(EmuConfig *config, int *num, int *den)
{
    if (S9xContentIsGameBoy())
    {
        // 8:7 is this port's square-pixel choice for the SNES; the Game Boy's
        // own grid is square already, so that one leaves it at 10:9. The rest
        // name a display shape, which the Game Boy takes as it stands.
        bool square = config->aspect_ratio_numerator == 8 &&
                      config->aspect_ratio_denominator == 7;
        *num = square ? 10 : config->aspect_ratio_numerator;
        *den = square ? 9 : config->aspect_ratio_denominator;
        return;
    }

    *num = config->aspect_ratio_numerator;
    *den = config->aspect_ratio_denominator;

    // The aspect names the SNES's shape at its nominal 224 lines; an overscan
    // frame is 239 lines of the same picture, so it is that much taller.
    if (config->show_overscan)
    {
        *num *= 224;
        *den *= 239;
    }

    // Widescreen hands us more columns for the same scanlines, and they are
    // meant to be seen rather than squeezed back into the SNES's shape.
    if (IPPU.WideExtent)
    {
        *num *= S9xWideWidth();
        *den *= SNES_WIDTH;
    }
}

EmuCanvas::EmuCanvas(EmuConfig *config, QWidget *main_window)
    : output_data{}, main_window(main_window), config(config)
{
    setFocus();
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void EmuCanvas::output(uint8_t *buffer, int width, int height, QImage::Format format, int bytes_per_line, double frame_rate)
{
    output_data.buffer = buffer;
    output_data.width = width;
    output_data.height = height;
    output_data.format = format;
    output_data.bytes_per_line = bytes_per_line;
    output_data.frame_rate = frame_rate;
    output_data.ready = true;

    if (get_late_frames() >= 1.0)
    {
        throttle_object.advance();
        if (get_late_frames() >= 1.0)
            throttle_object.reset();
        return;
    }

    draw();
}

double EmuCanvas::get_late_frames()
{
    if (config->speed_sync_method != EmuConfig::eTimerWithFrameskip)
        return 0.0;

    throttle_object.set_frame_rate(config->fixed_frame_rate == 0.0 ? output_data.frame_rate : config->fixed_frame_rate);

    return throttle_object.get_late_frames();
}

void EmuCanvas::throttle()
{
    if (config->speed_sync_method != EmuConfig::eTimer && config->speed_sync_method != EmuConfig::eTimerWithFrameskip)
        return;

    throttle_object.set_frame_rate(config->fixed_frame_rate == 0.0 ? output_data.frame_rate : config->fixed_frame_rate);
    throttle_object.wait_for_frame_and_rebase_time();
}

QRect EmuCanvas::applyAspect(const QRect &viewport)
{
    if (!config->scale_image)
    {
        return { (viewport.width() - output_data.width) / 2,
                 (viewport.height() - output_data.height) / 2,
                 output_data.width,
                 output_data.height };
    }
    if (!config->maintain_aspect_ratio)
        return viewport;

    int num, den;
    S9xQtDisplayAspect(config, &num, &den);

    if (config->use_integer_scaling)
    {
        int max_scale = 1;

        for (int i = 2; i < 20; i++)
        {
            int scaled_height = output_data.height * i;
            int scaled_width = scaled_height * num / den;
            if (scaled_width <= viewport.width() && scaled_height <= viewport.height())
                max_scale = i;
            else
                break;
        }

        int new_height = output_data.height * max_scale;
        int new_width = new_height * num / den;
        return { (viewport.width() - new_width) / 2,
                 (viewport.height() - new_height) / 2,
                 new_width,
                 new_height };
    }

    double canvas_aspect = (double)viewport.width() / viewport.height();
    double new_aspect = (double)num / den;

    if (canvas_aspect > new_aspect)
    {
        int new_width = viewport.height() * num / den;
        int new_x = (viewport.width() - new_width) / 2;

        return { new_x,
                 viewport.y(),
                 new_width,
                 viewport.height() };
    }

    int new_height = viewport.width() * den / num;
    int new_y = (viewport.height() - new_height) / 2;

    return { viewport.x(),
             new_y,
             viewport.width(),
             new_height };
}