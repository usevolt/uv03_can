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
        STATE_CONNECTING,
        STATE_CONNECTED,
        STATE_DOWNLOADING
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

    void log(std::string str);

    uint16_t calcCRC(uint8_t *data, int dataLen);

    /// @brief: Sends a packet and returns the crc from it
    bool sendPacket(uint8_t *data, unsigned int packetLen, uint16_t *crc);

    bool compareMessages(CanDev::CanMsg_st *msg1, CanDev::CanMsg_st *msg2);
};

#endif // LOADBINDIALOG_H
