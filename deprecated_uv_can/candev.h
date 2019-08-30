#ifndef CANDEV_H
#define CANDEV_H

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <canlib.h>
#include <canstat.h>
#include <deque>
#include <QTime>


typedef unsigned int canDeviceDescriptor;



class CanDev
{
public:

    enum canMsgType_e {
        CAN_STD,
        CAN_EXT
    };

    enum uvTerminalProtocol {
        UV_SDO,
        UV_EXT
    };

    struct CanMsg_st {
        std::string time;
        unsigned int id;
        unsigned int dataLen;
        unsigned char data[8];
        canMsgType_e type;
    };

    struct canDev_st {
        std::string name;
        unsigned int channel;
    };

    static CanDev *instance();

    void open(unsigned int baudrate, canDeviceDescriptor devName);

    void send(unsigned int id, canMsgType_e type, unsigned int dataLen, void *data);

    bool sendSync(unsigned int id, canMsgType_e type, unsigned int dataLen, void *data, unsigned int timeout_ms);

    void sendUvTerminal(std::string str, unsigned int nodeID);

    void clearReceiveBuffer();

    /// @brief: Constructs and returns the CAN msg ID which can be used to communicate with
    /// usevolt terminal interface
    unsigned int uvTerminalID(CanMsg_st &msg, unsigned int nodeID);

    bool isUvTerminalMsg(CanMsg_st &msg, int nodeID, std::string *dest = nullptr);

    unsigned int getUvTerminalNodeID(CanMsg_st &msg);

    /// @brief: Should only be called from rx thread
    void _rxTask();

    std::vector<canDev_st> getDevices();

    bool getConnected() const;

    bool receive(CanMsg_st &msg);



    uvTerminalProtocol getUvProtocol() const;
    void setUvProtocol(const uvTerminalProtocol &value);

private:
    CanDev();
    ~CanDev();
    bool connected;
    bool terminate;
    uvTerminalProtocol uvProtocol;
    std::deque<CanMsg_st> rxBuffer;

    std::thread *rxThread;
    std::mutex mutex;

    unsigned int channel;
    unsigned int baudrate;

    CanHandle rxHandle;

    CanHandle txHandle;
};

#endif // CANDEV_H
