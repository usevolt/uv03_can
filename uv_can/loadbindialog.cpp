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


#define PACKET_LEN 0x100U



const int LoadBinDialog::terminalMaxLen = 1000U;
std::string LoadBinDialog::path = QDir::homePath().toStdString();
unsigned int LoadBinDialog::deviceID = 0;

LoadBinDialog::LoadBinDialog(QWidget *parent) :
    QDialog(parent),
    state(STATE_NONE),
    ui(new Ui::LoadBinDialog)
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
    if (!CanDev::instance()->getConnected()) {
        OpenDialog d;
        int ret = d.exec();
        if (!ret) {
            log("CAN adapter required to flash devices\n");
            return;
        }
        CanDev::instance()->open(d.getBaudrate(), d.getDev());
    }

    ui->flash->setEnabled(false);
    ui->browse->setEnabled(false);
    ui->path->setEnabled(false);

    this->nodeId = ui->nodeid->value();

    QFile file(QString::fromStdString(this->path));
    if (file.open(QIODevice::ReadOnly)) {
        this->data = file.readAll();
        this->dataCount = this->data.size();
        this->dataIndex = 0;
        log("Flashing...");
        log("Waiting for target response...");
        this->localEcho = false;
        CanDev::instance()->sendUvTerminal("\nreset\n", this->nodeId);
//        CanDev::instance()->clearReceiveBuffer();
        this->state = STATE_CONNECTING;
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
    while (CanDev::instance()->receive(msg)) {

        if ((msg.id & 0xFFFF0000) == 0x15560000 && (msg.id & 0xFFFF) == this->nodeId) {

            // local echo filters the first accepted message out.
            // This has to be done because Kvaser adapter receives transmitted messages
            // by default.
            if (this->localEcho) {
                this->localEcho = false;
                continue;
            }

            if (this->state == STATE_CONNECTING) {
                if (msg.data[0] == 'C') {

                    // note: Works only on little-endian architectures
                    std::cout << "Downloading " << this->dataCount << " bytes" << std::endl;

                    msg.data[0] = 'C';
                    msg.dataLen = 8;
                    *((uint32_t*) &msg.data[1]) = this->dataCount;
                    msg.data[5] = 0;
                    msg.data[6] = 0;
                    msg.data[7] = 0;
                    this->localEcho = true;
                    CanDev::instance()->send(0x15560000 + this->nodeId,
                                             CanDev::CAN_EXT, msg.dataLen, msg.data);
                    this->lastMsg = msg;
                    this->state = STATE_CONNECTED;
                }
            }
            else if (this->state == STATE_CONNECTED) {
                if (!compareMessages(&msg, &this->lastMsg)) {
                    log("\n*****\nDownloaded data mismatch when connecting. "
                        "Flashing failed.\nReset the device and try again.\n*****");
                    this->state = STATE_NONE;
                    this->killTimer(this->timerId);
                    return;
                }
                log("Target connected. Flashing...");
                if(!sendPacket(&((uint8_t*)(this->data.data()))[this->dataIndex],
                        std::min(this->dataCount - this->dataIndex, PACKET_LEN), &this->crc)) {
                    log("\n*****\nError when receiving the first packet\n*****");
                    this->killTimer(this->timerId);
                    this->state = STATE_NONE;
                    return;
                }
                this->dataIndex += std::min(this->dataCount - this->dataIndex, PACKET_LEN);

                CanDev::instance()->clearReceiveBuffer();
                this->localEcho = false;
                this->state = STATE_DOWNLOADING;
            }
            else if (this->state == STATE_DOWNLOADING) {

                // compare crc
                std::ostringstream oss;
                oss << "Comparing received crc " << std::hex << *((uint16_t*) msg.data)
                    << " to calculated crc " << this->crc;
                log(oss.str());
                if (this->crc != *((uint16_t*) msg.data)) {
                    std::ostringstream os;
                    os << "\n*****\nCRC doesn't match on data " << std::hex << this->dataIndex << "\n*****";
                    log(os.str());
                    this->killTimer(this->timerId);
                    this->state = STATE_NONE;
                    return;
                }
                log("OK");

                if (!sendPacket(&((uint8_t*)(this->data.data()))[this->dataIndex],
                        std::min(this->dataCount - this->dataIndex, PACKET_LEN), &this->crc)) {
                    std::ostringstream os;
                    os << "\n*****\nError when sending packet. Data index: " << this->dataIndex
                       << ", packet length: " << PACKET_LEN << "\n*****";
                    log(os.str());
                    this->killTimer(this->timerId);
                    this->state = STATE_NONE;
                    return;
                }
                this->dataIndex += std::min(this->dataCount - this->dataIndex, PACKET_LEN);

                CanDev::instance()->clearReceiveBuffer();
                this->localEcho = false;


                if (this->dataIndex == this->dataCount) {
                    log("Download successful");
                    this->killTimer(this->timerId);
                    this->state = STATE_NONE;
                    ui->progress->setValue(100);
                    if (ui->close->isChecked()) {
                        this->close();
                    }
                    return;
                }

                ui->progress->setValue(this->dataIndex * 100 / this->dataCount);
                ui->flash->setEnabled(true);
                ui->browse->setEnabled(true);
                ui->path->setEnabled(true);
            }
        }
    }
}

void LoadBinDialog::log(std::__cxx11::string str)
{
    ui->terminal->insertPlainText(QString::fromStdString(str) + "\n");
    ui->terminal->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
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
