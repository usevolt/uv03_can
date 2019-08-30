#include "canterminal.h"
#include "ui_can_terminal.h"
#include "candev.h"
#include "opendialog.h"
#include <iostream>
#include <QTime>
#include <qregexp.h>
#include <qscrollbar.h>
#include <string.h>

const int CanTerminal::terminalMaxLen = 10000;

CanTerminal::CanTerminal(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::can_terminal)
{
    ui->setupUi(this);
    ui->splitter_2->setStretchFactor(0, 1);
    ui->can_table->setColumnWidth(0, 100);
    ui->can_table->setColumnWidth(1, 70);
    ui->can_table->setColumnWidth(2, 50);
    ui->can_table->setColumnWidth(3, 50);
}

CanTerminal::~CanTerminal()
{
    delete ui;
}

void CanTerminal::canReceive(CanDev::CanMsg_st &msg)
{
    unsigned int nodeID;
    std::string str;
    if (getActiveDev() != -1 && CanDev::instance()->isUvTerminalMsg(msg, getActiveDev(), &str)) {
        ui->terminal->insertPlainText(QString::fromStdString(str));
        // terminal clearing
        bool clear = false;
        if (str[0] == 'c' &&
                ui->terminal->toPlainText()[ui->terminal->toPlainText().size() - 1] == '\033') {
            clear = true;
        }
        for (unsigned int i = 1; i < str.size(); i++) {
            if (str[i] == 'c' && str[i - 1] == '\033') {
                clear = true;
                break;
            }
        }
        if (clear) {
            ui->terminal->clear();
        }
//        while (ui->terminal->toPlainText().size() > this->terminalMaxLen) {
//            QTextCursor c = ui->terminal->textCursor();
//            c.setPosition(1);
//            c.deleteChar();
//        }
        ui->terminal->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
    }
    else if (CanDev::instance()->isUvTerminalMsg(msg, -1)) {
        nodeID = CanDev::instance()->getUvTerminalNodeID(msg);
        bool match = false;
        for (int i = 0; i < ui->devs->rowCount(); i++) {
            if (strtol(ui->devs->item(i, 0)->text().toStdString().c_str(), NULL, 0) == nodeID) {
                match = true;
                break;
            }
        }
        if (!match) {
            std::cout << "new device found with nodeID " << nodeID << std::endl;
            ui->devs->insertRow(ui->devs->rowCount());
            QTableWidgetItem *item = new QTableWidgetItem();
            ui->devs->setItem(ui->devs->rowCount() - 1, 0, item);
            ui->devs->item(ui->devs->rowCount() - 1, 0)->setText(QString("0x") + QString::number(nodeID, 16));
        }
    }

    canTableInsert(msg);

}

void CanTerminal::on_devNew_clicked()
{
    ui->devs->insertRow(ui->devs->rowCount());
    QTableWidgetItem *item = new QTableWidgetItem();
    ui->devs->setItem(ui->devs->rowCount() - 1, 0, item);
    ui->devs->item(ui->devs->rowCount() - 1, 0)->setText("0xD");

    if (ui->devs->currentRow() == -1) {
        ui->devs->setCurrentCell(ui->devs->rowCount() - 1, 0);
    }
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

void CanTerminal::on_terminal_send_clicked()
{

    if (!CanDev::instance()->getConnected()) {
        std::cout << "Connect to CAN adapter first" << std::endl;
        OpenDialog d;
        int ret = d.exec();
        if (ret) {
            CanDev::instance()->open(d.getBaudrate(), d.getDev());
        }
        if (!CanDev::instance()->getConnected()) {
            return;
        }
    }

    int dev = getActiveDev();
    if (dev == -1) {
        std::cout << "Select active device first" << std::endl;
        return;
    }
    std::string str = ui->terminal_cmd->text().toStdString();
    ui->terminal_cmd->clear();
    std::cout << "Sending '" << str << "' to " << dev << std::endl;

    CanDev::instance()->sendUvTerminal(str, dev);

}

int CanTerminal::getActiveDev()
{
    if (ui->devs->currentRow() == -1) {
        return -1;
    }
    return strtol(ui->devs->item(ui->devs->currentRow(), 0)->text().toStdString().c_str(), NULL, 0);
}

void CanTerminal::canTableInsert(CanDev::CanMsg_st &msg)
{
    int row = ui->can_table->rowCount();
    for (int i = 0; i < ui->can_table->rowCount(); i++) {
        if ((ui->can_table->item(i, 1)->text().toInt(nullptr, 16) == (int) msg.id) &&
            ui->can_table->item(i, 2)->text().toStdString() ==
                ((msg.type == CanDev::CAN_EXT) ? std::string("x") : std::string("s"))) {
            row = i;
            break;
        }
    }

    if (row == ui->can_table->rowCount()) {
        // insert new row
        ui->can_table->insertRow(row);
        QTableWidgetItem *time = new QTableWidgetItem();
        QTableWidgetItem *id = new QTableWidgetItem();
        QTableWidgetItem *type = new QTableWidgetItem();
        QTableWidgetItem *len = new QTableWidgetItem();
        QTableWidgetItem *data = new QTableWidgetItem();
        ui->can_table->setItem(row, 0, time);
        ui->can_table->setItem(row, 1, id);
        ui->can_table->setItem(row, 2, type);
        ui->can_table->setItem(row, 3, len);
        ui->can_table->setItem(row, 4, data);
    }
    ui->can_table->item(row, 0)->setText(QString::fromStdString(msg.time));
    ui->can_table->item(row, 1)->setText(QString::number(msg.id, 16));
    ui->can_table->item(row, 2)->setText((msg.type == CanDev::CAN_EXT) ? "x" : "s");
    ui->can_table->item(row, 3)->setText(QString::number(msg.dataLen));
    QString str;
    for (unsigned int i = 0; i < msg.dataLen; i++) {
        char c[10];
        sprintf(c, "%02x ", msg.data[i]);
        str.append(c);
        if (i != msg.dataLen - 1) {
            str += QString(" ");
        }
    }
    ui->can_table->item(row, 4)->setText(str);
}

void CanTerminal::on_devs_currentCellChanged(int currentRow, int currentColumn,
                                             int previousRow, int previousColumn)
{
    if (currentRow || currentColumn || previousColumn || previousRow) {

    }
    ui->terminal->clear();
}

void CanTerminal::on_canSend_clicked()
{
    CanDev::CanMsg_st msg;
    msg.id = ui->canID->value();
    msg.type = ui->canSTD->isChecked() ? CanDev::CAN_STD : CanDev::CAN_EXT;
    QString str = ui->canData->text();
    QRegExp exp("\\s*([0-9a-fA-F]+)");
    int offset = 0;
    int dlc = 0;
    while (dlc < 8) {
        offset = exp.indexIn(str, offset) + exp.matchedLength();
        if (offset < 0) {
            break;
        }
        msg.data[dlc] = exp.cap(0).toInt(nullptr, 16);
        dlc++;
    }
    msg.dataLen = dlc;
    CanDev::instance()->send(msg.id, msg.type, msg.dataLen, msg.data);
}

void CanTerminal::on_sdoProt_toggled(bool checked)
{
    if (checked) {
        CanDev::instance()->setUvProtocol(CanDev::UV_SDO);
    }
    else {
        CanDev::instance()->setUvProtocol(CanDev::UV_EXT);
    }
}

void CanTerminal::on_canClear_clicked()
{
    while (ui->can_table->rowCount()) {
        ui->can_table->removeRow(0);
    }
}
