#include "loadbindialog.h"
#include "ui_loadbindialog.h"
#include "candev.h"
#include "opendialog.h"
#include <qfile.h>
#include <qdir.h>
#include <qfiledialog.h>
#include <qdebug.h>
#include <iostream>
#include <sstream>
#include <algorithm>



#define BLOCK_SIZE  256
#define SENDING_DELAY_MS    1000


const int LoadBinDialog::terminalMaxLen = 1000U;
std::string LoadBinDialog::path = QDir::homePath().toStdString();
unsigned int LoadBinDialog::deviceID = 0;

LoadBinDialog::LoadBinDialog(QWidget *parent) :
    QDialog(parent),
    state(STATE_NONE),
    ui(new Ui::LoadBinDialog),
    timerId(-1)
{
    ui->setupUi(this);
    ui->path->setText(QString::fromStdString(this->path));
    ui->nodeid->setValue(this->deviceID);
    ui->flash->setFocus();
}

LoadBinDialog::~LoadBinDialog()
{
    delete ui;
}

void LoadBinDialog::on_browse_clicked()
{
    QString p = QFileDialog::getOpenFileName(this, "Select binary", QString::fromStdString(this->path),
                                              "Binary (*.bin)");
    if (p.size()) {
        this->path = p.toStdString();
    }
    else {
        return;
    }
    ui->path->setText(QString::fromStdString(this->path));
    QFile file(QString::fromStdString(this->path));
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        file.close();
        log(std::string("File ") + this->path + " selected.\nFile size: " +
            std::to_string(data.size()));
        for (unsigned int i = 0x160; i < 0x170; i++) {
            char str[128];
            sprintf(str, ", 0x%02x", (uint8_t) data.data()[i]);
            std::cout << str;
        }
    }
}

void LoadBinDialog::on_flash_clicked()
{
    if (this->timerId != -1) {
        this->killTimer(this->timerId);
    }

    if (!CanDev::instance()->getConnected()) {
        OpenDialog d;
        int ret = d.exec();
        if (!ret) {
            log("CAN adapter required to flash devices\n");
            return;
        }
        CanDev::instance()->open(d.getBaudrate(), d.getDev());
    }

    this->nodeId = ui->nodeid->value();
    this->sendingDelay = SENDING_DELAY_MS;

    QFile file(QString::fromStdString(this->path));
    if (file.open(QIODevice::ReadOnly)) {
        this->data = file.readAll();
        this->dataCount = this->data.size();
        this->dataIndex = 0;
        log("Flashing...");
        log("Waiting for target response...");
        this->localEcho = false;
//        CanDev::instance()->sendUvTerminal("\nreset\n", this->nodeId);
        uint8_t d[2] = { 129, (uint8_t) this->nodeId };
        CanDev::instance()->send(0x0, CanDev::CAN_STD, 2, d);
        this->state = STATE_DEV_INIT;
        this->timerId = this->startTimer(1);
    }
    else {
        log(std::string("Couldn't open file") + this->path);
    }
}

void LoadBinDialog::timerEvent(QTimerEvent *e)
{
    if (e) {

    }
    CanDev::CanMsg_st msg;

    if (this->sendingDelay > 0) {
        this->sendingDelay--;
    }
    else {
        log("No response, restarting the download");
        killTimer(this->timerId);
        on_flash_clicked();
    }

    while (CanDev::instance()->receive(msg)) {

        if (msg.id == 0x700 + this->nodeId || msg.id == 0x580 + this->nodeId) {
            this->sendingDelay = SENDING_DELAY_MS;
        }

        if (msg.id == 0x700 + this->nodeId && msg.data[0] == 0x0) {
            this->dataIndex = 0;
            CanDev::instance()->clearReceiveBuffer();
            this->state = STATE_BLOCK_INIT;
        }



        if (!sendBlock(&msg)) {
            if (this->dataIndex >= BLOCK_SIZE) {
                this->dataIndex -= BLOCK_SIZE;
            }
            // first packet failed, end the transfer
            if (this->dataIndex == 0) {
                log("*****************************\n"
                    "Flashing ended because the first block transfer failed\n"
                    "*****************************\n");
                this->state = STATE_DEV_INIT;
                killTimer(this->timerId);
            }
        }

//        ui->progress->setValue(this->dataIndex * 100 / this->dataCount);
//        ui->flash->setEnabled(true);
//        ui->browse->setEnabled(true);
//        ui->path->setEnabled(true);
    }
    if (!sendBlock(NULL)) {
        if (this->dataIndex >= BLOCK_SIZE) {
            this->dataIndex -= BLOCK_SIZE;
        }
        // first packet failed, end the transfer
        if (this->dataIndex == 0) {
            log("*****************************\n"
                "Flashing ended because the first block transfer failed\n"
                "*****************************\n");
            this->state = STATE_DEV_INIT;
            killTimer(this->timerId);
        }
    }

    // data transfer is finnished, restart the node
    if (this->dataIndex == this->dataCount) {
        msg.data[0] = 0x81;
        msg.data[1] = this->nodeId;
        CanDev::instance()->send(0, CanDev::CAN_STD, 2, msg.data);
        this->state = STATE_NONE;
        killTimer(this->timerId);
        std::stringstream ss;
        ss << "********************\nFlashed '" <<
              this->dataCount << "' bytes succesfully\n********************";
        log(ss.str());
        ui->progress->setValue(100);
        if (ui->close->isChecked()) {
            this->close();
        }
        return;
    }
    ui->progress->setValue(this->dataIndex * 100 / this->dataCount);
}

void LoadBinDialog::log(std::__cxx11::string str)
{
    ui->terminal->insertPlainText(QString::fromStdString(str) + "\n");
    ui->terminal->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
}

bool LoadBinDialog::sendBlock(CanDev::CanMsg_st *rx_msg)
{
    CanDev::CanMsg_st tx_msg;
    if (rx_msg && ui->rx_log->isChecked()) {
        std::cout << "rx msg id: " << std::hex << rx_msg->id << std::endl;
    }

    if (this->state == STATE_BLOCK_INIT) {
        // send a SDO write block request to index 0x1F50
        tx_msg.data[0] = 0x62;
        tx_msg.data[1] = 0x50;
        tx_msg.data[2] = 0x1F;
        tx_msg.data[3] = 1;
        memset(&tx_msg.data[4], 0, 4);
        CanDev::instance()->send(0x600 + this->nodeId, CanDev::CAN_STD, 8, tx_msg.data);
        this->localEcho = true;
        this->state = STATE_BLOCK_DOWNLOAD;
        return true;
    }
    else if (this->state == STATE_BLOCK_DOWNLOAD) {
        if (rx_msg && rx_msg->id == 0x580 + this->nodeId && rx_msg->data[0] == 0xA4) {
            int msgCount = 0;
            int dataByte = 1;
            int startIndex = this->dataIndex;
            for (int i = 0; i < BLOCK_SIZE; i++) {
                if (this->dataIndex == this->dataCount) {
                    break;
                }
                rx_msg->data[dataByte++] = this->data.data()[this->dataIndex++];
                if (dataByte == 8) {
                    if (this->dataIndex == this->dataCount || i == BLOCK_SIZE - 1) {
                        rx_msg->data[0] = (1 << 7) + msgCount++;
                    }
                    else {
                        rx_msg->data[0] = msgCount++;
                    }
                    dataByte = 1;
                    CanDev::instance()->send(0x600 + this->nodeId, CanDev::CAN_STD, 8, rx_msg->data);
                }
            }
            if (dataByte != 1) {
                rx_msg->data[0] = (1 << 7) + msgCount;
                CanDev::instance()->send(0x600 + this->nodeId, CanDev::CAN_STD, 8, rx_msg->data);
            }

            // send end block message
            rx_msg->data[0] = 0xC1 + ((7 - (dataByte - 1)) << 2);
            uint16_t crc = calcCRC((uint8_t*) &this->data.data()[startIndex], this->dataIndex - startIndex);
            rx_msg->data[1] = (uint8_t) crc;
            rx_msg->data[2] = (crc >> 8);
            memset(&rx_msg->data[3], 0, 5);
            CanDev::instance()->send(0x600 + this->nodeId, CanDev::CAN_STD, 8, rx_msg->data);
            CanDev::instance()->clearReceiveBuffer();
            this->localEcho = true;
            std::stringstream ss;
            ss << "Block sent with CRC 0x" << std::hex << crc;
            log(ss.str());

            this->state = STATE_BLOCK_END;

        }
        return true;
    }
    else if (this->state == STATE_BLOCK_END) {
        if (this->localEcho) {
            this->localEcho = false;
            return true;
        }
        if (rx_msg && rx_msg->id == 0x580 + this->nodeId && rx_msg->data[0] == 0x80) {
            // crc didn't match
            std::stringstream ss;
            ss << "*********************\nDevice aborted the block "
                  " in CRC check\n*********************";
            log(ss.str());
            this->state = STATE_BLOCK_INIT;
            return false;
        }
        else if (rx_msg && rx_msg->id == 0x580 + this->nodeId && rx_msg->data[0] == 0xA1) {
            // crc did match, proceed
            this->state = STATE_BLOCK_INIT;
            return true;
        }
    }

    return true;
}

uint16_t LoadBinDialog::calcCRC(uint8_t *data, int dataLen)
{
    uint8_t i;
    uint16_t u16CRC = 0;

    while(--dataLen >= 0)
    {
        i = 8;
        u16CRC = u16CRC ^ (((uint16_t)*data++) << 8);

        do
        {
            if (u16CRC & 0x8000)
            {
                u16CRC = u16CRC << 1 ^ 0x1021;
            }
            else
            {
                u16CRC = u16CRC << 1;
            }
        }
        while(--i);
    }
    return u16CRC;
}

bool LoadBinDialog::sendPacket(uint8_t *data, unsigned int packetLen, uint16_t *crc)
{
    CanDev::CanMsg_st msg;
    msg.dataLen = 8U;
    msg.id = 0x15560000 + this->nodeId;
    msg.type = CanDev::CAN_EXT;
    int count = 0;
    int canCount = 0;

    std::ostringstream os;
    os << "Sending packet... Len: " << packetLen;
    log(os.str());
    unsigned int i;
    for (i = 0; i < packetLen; i++) {
        if (count < 8) {
            msg.data[count++] = data[i];
        }
        if (count == 8) {
            if(!CanDev::instance()->sendSync(msg.id, msg.type, msg.dataLen, msg.data, 5000)) {
                log("\n*****\nSending data failed for unknown reason.\n*****");
                return false;
            }
            canCount++;
            count = 0;
        }
    }
    if (count != 0) {
        if(!CanDev::instance()->sendSync(msg.id, msg.type, msg.dataLen, msg.data, 5000)) {
            log("\n*****\nSending data failed for unknown reason.\n*****");
            return false;
        }
        canCount++;
    }
    if (crc) {
        *crc = calcCRC(data, packetLen);
    }

    std::stringstream ss;
    ss << "Packet sent as " << canCount << " CAN frames.";
    log(ss.str());

    return true;
}

bool LoadBinDialog::compareMessages(CanDev::CanMsg_st *msg1, CanDev::CanMsg_st *msg2)
{
    bool ret = true;
    if (msg1->id == msg2->id && msg1->type == msg2->type && msg1->dataLen == msg2->dataLen) {
        for (unsigned int i = 0; i < msg1->dataLen; i++) {
            if (msg1->data[i] != msg2->data[i]) {
                ret = false;
                break;
            }
        }
    }

    if (!ret) {
        std::ostringstream os;
        os << "Received: ";
        for (unsigned int i = 0; i < msg1->dataLen; i++) {
            char str[20];
            sprintf(str, "%02x ", msg1->data[i]);
            os << str;
        }
        os << "\nexpected: ";
        for (unsigned int i = 0; i < msg2->dataLen; i++) {
            char str[20];
            sprintf(str, "%02x ", msg2->data[i]);
            os << str;
        }
        log(os.str());
    }

    return ret;
}

void LoadBinDialog::on_nodeid_valueChanged(int arg1)
{
    this->deviceID = arg1;
}
