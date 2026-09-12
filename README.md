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


# License
*Uvcan* is free software, licensed under the **GNU General Public License,
version 3** — see [LICENSE](LICENSE). Every source file under `src/` and `inc/`
carries the matching notice, except `inc/thirdparty/`, which holds third-party
code under its own terms.

Two things worth knowing about that choice:

* The `hal/` submodule (`uv_hal`) is a separate project under the MIT license.
  It is shared with the embedded firmware, where the GPL would not be wanted,
  and MIT code may be used in a GPL work.
* *Uvcan* links GNU readline, which is GPLv3. A binary linking it has to be
  distributed under GPLv3-compatible terms, so this is not only a preference.
  The top-level `LICENSE` file said MIT until 2026-09 — that was boilerplate
  from the repository's first commit, predating uvcan's own sources, and it
  never matched the file headers.
