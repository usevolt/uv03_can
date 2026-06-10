uvcan for Windows
=================

Contents
--------
  uvcan.exe                          the uvcan command-line / UI tool
  PCANBasic.dll                      PEAK PCAN-Basic API (CAN interface)
  fonts\LiberationSans-Regular.ttf   font used by the graphical UI
  uvcan-ui.bat                       double-click to open the configuration UI

Requirements
------------
  * A PEAK PCAN-USB adapter, with the PEAK PCAN driver installed on this PC.
    PCANBasic.dll is only the API shim - it needs the kernel-mode driver.
    Get it from https://www.peak-system.com/  ->  PCAN-USB drivers.

Open the graphical UI
---------------------
  Double-click  uvcan-ui.bat

Run command-line commands
-------------------------
  Open a Command Prompt in this folder, e.g.:
    uvcan.exe --help
    uvcan.exe --can PCAN_USBBUS1 --listen 10

Keep uvcan.exe, PCANBasic.dll and the fonts\ folder together in one folder.
