#include "canterminal.h"
#include "ui_can_terminal.h"
#include <iostream>

CanTerminal::CanTerminal(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::can_terminal)
{
    ui->setupUi(this);
    ui->splitter_2->setStretchFactor(0, 1);
}

CanTerminal::~CanTerminal()
{
    delete ui;
}

void CanTerminal::on_devNew_clicked()
{
    ui->devs->insertRow(ui->devs->rowCount());
    QTableWidgetItem *item = new QTableWidgetItem();
    ui->devs->setItem(ui->devs->rowCount() - 1, 0, item);
    ui->devs->item(ui->devs->rowCount() - 1, 0)->setText("0xD");
}

void CanTerminal::on_devDel_clicked()
{
    if (!ui->devs->rowCount()) {
        return;
    }
    int row = ui->devs->currentRow();
    if (row == -1) {
        row = 0;
    }

    ui->devs->removeRow(row);
    if (row >= ui->devs->rowCount()) {
        row--;
    }
    ui->devs->selectRow(row);
}
