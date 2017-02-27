#include "candev.h"
#include <iostream>
#include <thread>
#include <string.h>
#include <unistd.h>
#include <qdebug.h>


#define UV_PREFIX 0x580
#define UV_TX_PREFIX 0x600
#define UV_EXT_PREFIX 0x15560000
#define RX_BUFFER_MAX_LEN   0x1000

void rx() {
    CanDev::instance()->_rxTask();

}

void CanDev::_rxTask()
{
    qDebug() << "Initializing CAN rx thread handle...";


    this->rxHandle = canOpenChannel(this->channel, canOPEN_REQUIRE_EXTENDED | canOPEN_ACCEPT_VIRTUAL);
    if (this->rxHandle < 0) {
        qDebug() << "rxThread handle opening failed. Unable to receive messages";
        return;
    }

    qDebug() << "RX handle initialized";

    while (true) {
        unsigned int dlc, flag;
        long id;
        unsigned long time;
        char msg[8];
        canStatus stat = canReadWait(this->rxHandle, &id, &msg, &dlc, &flag, &time, 1000);

        if (this->terminate) {
            qDebug() << "Putting off rx CAN handle";
            canBusOff(this->rxHandle);
            qDebug() << "Closing rx CAN handle";
            canClose(this->rxHandle);
            return;
        }

        if (stat != canOK && stat != canERR_NOMSG) {
            qDebug() << "Error in rx thread. Kvaser status: " << stat;
        }
        else if (stat == canOK) {
            CanMsg_st mesg;
            mesg.time = QTime::currentTime().toString("hh:mm:ss:zzz").toStdString();
            mesg.dataLen = dlc;
            mesg.id = id;
            mesg.type = (flag & canMSG_EXT) ? CAN_EXT : CAN_STD;
            memcpy(mesg.data, msg, dlc);
            this->mutex.lock();
            this->rxBuffer.push_back(mesg);
            if (this->rxBuffer.size() > RX_BUFFER_MAX_LEN) {
                this->rxBuffer.pop_front();
            }
            this->mutex.unlock();
        }

    }
}

std::vector<CanDev::canDev_st> CanDev::getDevices()
{
    std::vector<canDev_st> devs;
    int count;
    canStatus stat = canGetNumberOfChannels(&count);
    if (stat != canOK) {
        std::cout << "Problem in Kvaser CAN drivers. Status: " << stat << std::endl;
        return devs;
    }
    for (int i = 0; i < count; i++) {
        char name[256];
        stat = canGetChannelData(i, canCHANNELDATA_DEVDESCR_ASCII, name, sizeof(name));
        if (stat == canOK) {
            std::cout << "Can device name: " << name << std::endl;
            canDev_st d;
            d.channel = i;
            d.name = std::string(name);
            devs.push_back(d);
        }
    }
    return devs;
}


CanDev::CanDev():
    connected(false), terminate(false), uvProtocol(UV_SDO), rxThread(nullptr), mutex()
{
    std::cout << "Initializing CAN dev" << std::endl;

}
CanDev *CanDev::instance()
{
    static CanDev d;
    return &d;
}

void CanDev::open(unsigned int baudrate, canDeviceDescriptor devName)
{
    if (this->connected) {
        std::cout << "Already connected. Disconnecting from the previous CAN device..." << std::endl;
        canBusOff(this->txHandle);
        canClose(this->txHandle);
        std::cout << "Disconnected" << std::endl;
    }

    std::cout << "Opening device '" << devName << "' with baudrate " << baudrate << "..." << std::endl;

    this->channel = devName;
    this->baudrate = baudrate;

    this->txHandle = canOpenChannel(devName, canOPEN_REQUIRE_EXTENDED | canOPEN_ACCEPT_VIRTUAL);
    if (this->txHandle < 0) {
        std::cout << "problem in Kvaser handle " << this->txHandle << std::endl;
        return;
    }
    this->connected = true;

    canStatus stat = canSetBusParams(this->txHandle, baudrate, 5, 2, 1, 1, 0);
    if (stat != canOK) {
        std::cout << "Setting Kvaser CAN bus params failed: " << stat << std::endl;
        return;
    }
    unsigned int tseg1, tseg2, sjw, nosamp, syncmode;
    long freq;
    canGetBusParams(this->txHandle, &freq, &tseg1, &tseg2, &sjw, &nosamp, &syncmode);
    std::cout << "freq: " << freq << ", tseg1: " << tseg1 << ", tseg2: " << tseg2
              << ", sjw: " << sjw << ", nosamp: " << nosamp << ", syncmode: " << syncmode << std::endl;

//    note: canSetNotify causes the program to freeze and the OS unable to reboot
//    stat = canSetNotify(this->txHandle, rx, canNOTIFY_RX | canNOTIFY_TX | canNOTIFY_ERROR, NULL);
//    if (stat != canOK) {
//        std::cout << "Setting Kvaser CAN notify callback failed: " << stat << std::endl;
//        return;
//    }

    stat = canBusOn(this->txHandle);
    if (stat != canOK) {
        std::cout << "Unable to set the Kvaser CAN bus on. Status: " << stat << std::endl;
        return;
    }

    this->rxThread = new std::thread(rx);

//    stat = canWrite(this->txHandle, 10000, (void*) "Kvaser!", 8, 0);

    std::cout << "Connected" << std::endl;
}

void CanDev::send(unsigned int id, CanDev::canMsgType_e type, unsigned int dataLen, void *data)
{
    std::cout << "Sending id: " << id << ", type: " << type << ", Len: " << dataLen << ", data: ";
    for (unsigned int i = 0; i < dataLen; i++) {
        char str[20];
        sprintf(str, "%02x", ((unsigned char*)data)[i]);
        std::cout << str;
        if (i != dataLen - 1) {
            std::cout << ", ";
        }
    }
    std::cout << std::endl;

    if (this->connected) {
        canStatus stat = canWrite(this->txHandle, id, data, dataLen, (type == CAN_EXT) ? canMSG_EXT : canMSG_STD);
        if (stat != canOK) {
            std::cout << "CAN send Kvaser error: " << stat << std::endl;
        }
    }
}

bool CanDev::sendSync(unsigned int id, CanDev::canMsgType_e type, unsigned int dataLen, void *data, unsigned int timeout_ms)
{
//    std::cout << "Sync sending id: " << id << ", type: " << type << ", Len: " << dataLen << ", data: ";
//    for (unsigned int i = 0; i < dataLen; i++) {
//        char str[20];
//        sprintf(str, "%02x", ((unsigned char*)data)[i]);
//        std::cout << str;
//        if (i != dataLen - 1) {
//            std::cout << ", ";
//        }
//    }
//    std::cout << std::endl;

    if (this->connected) {
        canStatus stat = canWrite(this->txHandle, id, data, dataLen, (type == CAN_EXT) ? canMSG_EXT : canMSG_STD);
        if (stat != canOK) {
            std::cout << "CAN send Kvaser error: " << stat << std::endl;
        }
        stat = canWriteSync(this->txHandle, timeout_ms);
        return (stat == canOK);
    }
    else {
        return false;
    }
}

void CanDev::sendUvTerminal(std::__cxx11::string str, unsigned int nodeID)
{
    if (uvProtocol == UV_SDO) {
        int count = 0;
        CanDev::CanMsg_st msg;
        msg.id = UV_TX_PREFIX + nodeID;
        msg.type = CAN_STD;
        msg.dataLen = 8;
        msg.data[0] = 0x22;
        msg.data[1] = 0xFF;
        msg.data[2] = 0x5F;
        msg.data[3] = 0;

        for (unsigned int i = 0; i < str.size(); i++) {
            if (count < 4) {
                msg.data[4 + count++] = str[i];
            }
            if (count == 4) {
                CanDev::instance()->send(msg.id, msg.type, 4 + count, msg.data);
                count = 0;
            }
        }
        msg.data[4 + count++] = '\n';
        CanDev::instance()->send(msg.id, msg.type, 4 + count, msg.data);
    }
    else if (uvProtocol == UV_EXT) {
        int count = 0;
        CanDev::CanMsg_st msg;
        msg.id = UV_EXT_PREFIX + nodeID;
        msg.type = CAN_EXT;

        for (unsigned int i = 0; i < str.size(); i++) {
            if (count < 8) {
                msg.data[count++] = str[i];
            }
            if (count == 8) {
                CanDev::instance()->send(msg.id, msg.type, count, msg.data);
                count = 0;
            }
        }
        msg.data[count++] = '\n';
        CanDev::instance()->send(msg.id, msg.type, count, msg.data);
    }
}

void CanDev::clearReceiveBuffer()
{
    this->mutex.lock();
    this->rxBuffer.clear();
    this->mutex.unlock();
}

bool CanDev::isUvTerminalMsg(CanMsg_st &msg, int nodeID, std::string *dest)
{
    if (uvProtocol == UV_SDO) {
        if (msg.type == CAN_STD) {
            if ((nodeID >= 0 && msg.id == (unsigned int) UV_PREFIX + nodeID) ||
                    (nodeID < 0 && msg.id >= 0x580 && msg.id <= 0x580 + 0xFF)) {
                if (msg.dataLen > 4 &&
                        msg.type == CAN_STD &&
                        msg.data[0] == 0x42 &&
                        msg.data[1] == 0xFF &&
                        msg.data[2] == 0x5F &&
                        msg.data[3] == 0) {
                    if (dest) {
                        *dest = std::string((char*) &msg.data[4], msg.dataLen - 4);
                    }
                    return true;
                }
            }
        }
    }
    else if (uvProtocol == UV_EXT) {
        if (msg.type == CAN_EXT) {
            if ((nodeID >= 0 && msg.id == (unsigned int) 0x15560000 + nodeID) ||
                    (nodeID < 0 && (msg.id & 0xFFFF0000) == 0x15560000)) {
                if (dest) {
                    *dest = std::string((char*) msg.data, msg.dataLen);
                }
                return true;
            }
        }
    }
    return false;
}

unsigned int CanDev::getUvTerminalNodeID(CanMsg_st &msg)
{
    if (uvProtocol == UV_SDO) {
        return msg.id & 0x7F;
    }
    else if (uvProtocol == UV_EXT) {
        return msg.id & 0xFFFF;
    }
    return 0;
}


CanDev::~CanDev()
{
    if (this->connected) {
        std::cout << "Disconnecting from the CAN adapter..." << std::endl;

        this->terminate = true;
        qDebug() << "Joining rx thread";
        this->rxThread->join();
        qDebug() << "Destroying rx tread";
        delete this->rxThread;

        std::cout << "Putting CAN bus off" << std::endl;

        canBusOff(this->txHandle);

        usleep(100000);
        std::cout << "Closing connection to CAN adapter" << std::endl;
        canClose(this->txHandle);
        std::cout << "Disconnected" << std::endl;
    }
}

CanDev::uvTerminalProtocol CanDev::getUvProtocol() const
{
    return uvProtocol;
}

void CanDev::setUvProtocol(const uvTerminalProtocol &value)
{
    uvProtocol = value;
}


bool CanDev::getConnected() const
{
    return connected;
}

bool CanDev::receive(CanDev::CanMsg_st& msg)
{
    if (!this->rxBuffer.size()) {
        return false;
    }
    this->mutex.lock();
    msg = this->rxBuffer.front();
    this->rxBuffer.pop_front();
    this->mutex.unlock();
    return true;
}


