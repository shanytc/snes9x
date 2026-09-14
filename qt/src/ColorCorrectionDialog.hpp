/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QSlider>

#include "EmuApplication.hpp"

/* win32-style Video->Color Correction dialog: an accurate-colors toggle for
 * whichever screen is being emulated, plus optional gamma/contrast/saturation
 * adjustments, which are not a console model and so apply to any picture.
 * Values apply on OK. */
class ColorCorrectionDialog : public QDialog
{
    Q_OBJECT

  public:
    ColorCorrectionDialog(EmuApplication *app, QWidget *parent);

  private:
    void setDefaults();
    void accept() override;

    EmuApplication *app;
    QCheckBox *color_correction_checkbox;
    QCheckBox *adjustments_checkbox;
    QSlider *gamma_slider;
    QSlider *contrast_slider;
    QSlider *saturation_slider;
};
