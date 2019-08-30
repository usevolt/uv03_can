#include "opendialog.h"
#include "ui_opendialog.h"
#include "candev.h"

OpenDialog::OpenDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::OpenDialog)
{
    ui->setupUi(this);

    std::vector<CanDev::canDev_st> devs = CanDev::instance()->getDevices();

    for (unsigned int i = 0; i < devs.size(); i++) {
        ui->candevs->insertItem(i, QString::fromStdString(devs[i].name) + " (" + QString::number(devs[i].channel) + ")");
    }
}

OpenDialog::~OpenDialog()
{
    delete ui;
}

unsigned int OpenDialog::getBaudrate()
{
    return ui->baudrate->value();
}

canDeviceDescriptor OpenDialog::getDev()
{
    return ui->candevs->currentIndex();
}
