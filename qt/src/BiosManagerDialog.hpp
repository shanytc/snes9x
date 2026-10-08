#pragma once

#include <QDialog>
#include <vector>

#include "biosmanager.h"

class EmuApplication;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// File -> BIOS Manager, laid out as win32's: a sidebar of families, and a card per BIOS for
// the sidebar's pick. A chip's card picks how it runs (Legacy, Native or Firmware) and opens
// its speed chart. Paths and modes reach the core on Save Changes; the console for GB content
// lives in Emulation -> Game Boy Model.
class BiosManagerDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit BiosManagerDialog(QWidget *parent, EmuApplication *app);

  private:
    struct Card
    {
        QWidget     *frame  = nullptr;
        QLabel      *label  = nullptr;
        QLineEdit   *edit   = nullptr;
        QLabel      *mark   = nullptr;   // the red cross before trouble
        QLabel      *status = nullptr;
        QPushButton *browse = nullptr;
        QPushButton *clear  = nullptr;
        QComboBox   *mode   = nullptr;   // a chip's only
    };

    void addCard(int slot, QVBoxLayout *list);
    void showPick(QTreeWidgetItem *item);
    void browse(int slot);
    void refreshCard(int slot);
    int chipMode(int slot) const;
    void applyAndClose();

    EmuApplication   *app;
    QTreeWidget      *sidebar;
    std::vector<Card> cards;
};
