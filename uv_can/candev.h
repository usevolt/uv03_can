#ifndef CANDEV_H
#define CANDEV_H

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <canlib.h>
#include <canstat.h>


typedef unsigned int canDeviceDescriptor;

class CanDev
{
public:

    struct canDev_st {
        std::string name;
        unsigned int channel;
    };

    static CanDev *instance();

    void open(unsigned int baudrate, canDeviceDescriptor devName);

    /// @brief: Should only be called from rx thread
    void _rxTask();

    std::vector<canDev_st> getDevices();

private:
    CanDev();
    ~CanDev();
    bool connected;
    bool terminate;
    std::thread *rxThread;
    std::mutex mutex;

    unsigned int channel;
    unsigned int baudrate;


    CanHandle rxHandle;

    CanHandle txHandle;
};

#endif // CANDEV_H
