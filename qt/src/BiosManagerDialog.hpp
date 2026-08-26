#pragma once

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <vector>

#include "biosmanager.h"

class EmuApplication;

// File -> BIOS Manager: one row per supported BIOS, plus the default boot
// mode for Game Boy content. Paths are applied to the core on close.
class BiosManagerDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit BiosManagerDialog(QWidget *parent, EmuApplication *app);

  private:
    struct Row
    {
        QLineEdit *edit;
        QLabel    *status;
    };

    void browse(int slot);
    void refreshRow(int slot);
    void applyAndClose();

    EmuApplication  *app;
    std::vector<Row> rows;
    QComboBox       *gb_default;
};
