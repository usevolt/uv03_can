#ifndef CAN_TERMINAL_H
#define CAN_TERMINAL_H

#include <QWidget>

namespace Ui {
class can_terminal;
}

class CanTerminal : public QWidget
{
    Q_OBJECT

public:
    explicit CanTerminal(QWidget *parent = 0);
    ~CanTerminal();

private:
    Ui::can_terminal *ui;
};

#endif // CAN_TERMINAL_H
