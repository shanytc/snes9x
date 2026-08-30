#pragma once

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <vector>

#include "biosmanager.h"

class EmuApplication;

// File -> BIOS Manager: one row per supported BIOS. Paths are applied to the
// core on close; the console for GB content lives in
// Emulation -> Game Boy Model.
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
};
