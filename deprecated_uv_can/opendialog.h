#ifndef OPENDIALOG_H
#define OPENDIALOG_H

#include <QDialog>
#include "string"
#include "candev.h"

namespace Ui {
class OpenDialog;
}

class OpenDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OpenDialog(QWidget *parent = 0);
    ~OpenDialog();

    unsigned int getBaudrate();

    canDeviceDescriptor getDev();

private:
    Ui::OpenDialog *ui;
};

#endif // OPENDIALOG_H
