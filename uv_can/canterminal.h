#ifndef CAN_TERMINAL_H
#define CAN_TERMINAL_H

#include <QWidget>
#include "candev.h"

namespace Ui {
class can_terminal;
}

class CanTerminal : public QWidget
{
    Q_OBJECT

public:
    explicit CanTerminal(QWidget *parent = 0);
    ~CanTerminal();


    /// @brief: Should be called when a CAN message is received
    void canReceive(CanDev::CanMsg_st &msg);

private slots:
    void on_devNew_clicked();

    void on_devDel_clicked();

    void on_terminal_send_clicked();

    void on_devs_currentCellChanged(int currentRow, int currentColumn, int previousRow, int previousColumn);

    void on_canSend_clicked();

private:
    Ui::can_terminal *ui;

    int getActiveDev();

    void canTableInsert(CanDev::CanMsg_st &msg);
};

#endif // CAN_TERMINAL_H
