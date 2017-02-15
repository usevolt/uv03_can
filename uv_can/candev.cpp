#include "candev.h"
#include <iostream>
#include <thread>
#include <string.h>
#include <unistd.h>
#include <qdebug.h>



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

        if (stat != canOK && stat != canERR_NOMSG) {
            qDebug() << "Error in rx thread. Kvaser status: " << stat;
        }

        if (this->terminate) {
            canBusOff(this->rxHandle);
            canClose(this->rxHandle);
            return;
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
    connected(false), terminate(false), rxThread(nullptr), mutex()
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


CanDev::~CanDev()
{
    if (this->connected) {
        std::cout << "Disconnecting from the CAN adapter..." << std::endl;

        this->terminate = true;
        this->rxThread->join();
        delete this->rxThread;

        canBusOff(this->txHandle);
        canClose(this->txHandle);
        std::cout << "Disconnected" << std::endl;
    }
}


