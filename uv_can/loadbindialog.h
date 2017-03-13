#ifndef LOADBINDIALOG_H
#define LOADBINDIALOG_H

#include <QDialog>
#include <string>
#include <qbytearray.h>
#include "candev.h"

namespace Ui {
class LoadBinDialog;
}

class LoadBinDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoadBinDialog(QWidget *parent = 0);
    ~LoadBinDialog();

private slots:
    void on_browse_clicked();

    void on_flash_clicked();

    void on_nodeid_valueChanged(int arg1);

private:

    enum state_e {
        STATE_NONE,
        STATE_DEV_INIT,
        STATE_BLOCK_INIT,
        STATE_BLOCK_DOWNLOAD,
        STATE_BLOCK_END
    };

    static std::string path;
    static unsigned int deviceID;
    static const int terminalMaxLen;

    state_e state;

    Ui::LoadBinDialog *ui;


    virtual void timerEvent(QTimerEvent *e);

    int timerId;
    unsigned int nodeId;
    unsigned int dataCount;
    unsigned int dataIndex;
    bool localEcho;
    CanDev::CanMsg_st lastMsg;
    QByteArray data;
    uint16_t crc;

    int sendingDelay;

    void log(std::string str);

    /// @brief: Sends the next block of data
    bool sendBlock(CanDev::CanMsg_st *rx_msg);

    uint16_t calcCRC(uint8_t *data, int dataLen);

    /// @brief: Sends a packet and returns the crc from it
    bool sendPacket(uint8_t *data, unsigned int packetLen, uint16_t *crc);

    bool compareMessages(CanDev::CanMsg_st *msg1, CanDev::CanMsg_st *msg2);
};

#endif // LOADBINDIALOG_H
