# uv03_can
UVcan CAN tool for flashing the firmware to Usevolt devices

# Prerequisites
The uvcan tool uses the following libraries underneath:

* GTK3
* GDK3
* ncurses
* pthread
* SocketCAN
* jq

Especially the GTK3 libraries have to be installed to compile the uvcan. On Ubuntu Linux, run the following commands to install the prerequisites:

`sudo apt-get install git build-essential libncurses5 libncurses5-dev libgtk-3-0 libgtk-3-dev libgtk-3-common libxml2-utils jq`

#Compiling the uvcan
The uvcan makefile detects the operating system automatically. Compiling on Linux & Windows is supported.

* Make sure that the uv_hal library is fetched from the version control with `git submodule update --init`
* go to the *uvcan* directory and compile with `make`

#Using the uvcan

*Uvcan* was originally meant to be used from command-line. However, currently it also has a GUI version included, which doesn't have all the same functionalities as the command line version, but can be used to flash firmware to devices, communicate with their terminal interface and modify CANopen object dictionary parameters. On Windows the GUI mode is the primary interface.

When running the *uvcan*, if no arguments are given, the GUI mode is started. If any arguments are given, the program will run on command-line mode. Run `uvcan --help` for more information or what kind of commands are available.


#Using without root permissions
To connect to the CAN-bus without root permissions, append these to the end of /etc/sudoers file with command `sudo visudo`:

```
<user> <comp_name> = (root) NOPASSWD: /sbin/ip
```
Where `<user>` is the logged in user's name and `<com_name>` is the computer's name.
