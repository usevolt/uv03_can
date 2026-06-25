uvcan for Windows
=================

Contents
--------
  uvcan.exe                          the uvcan command-line / UI tool
  PCANBasic.dll                      PEAK PCAN-Basic API (CAN interface)
  fonts\LiberationSans-Regular.ttf   font used by the graphical UI
  uvcan-ui.bat                       double-click to open the configuration UI
  uvcan-open.bat                     open handler for .uvsys files
  uvsys.ico                          icon shown for .uvsys files
  install-association.bat            register the .uvsys file association
  uninstall-association.bat          remove the .uvsys file association

Requirements
------------
  * A PEAK PCAN-USB adapter, with the PEAK PCAN driver installed on this PC.
    PCANBasic.dll is only the API shim - it needs the kernel-mode driver.
    Get it from https://www.peak-system.com/  ->  PCAN-USB drivers.

  * Windows 10 (1803 / April 2018) or newer. uvcan extracts .uvsys and .uvdev
    packages with the built-in tar.exe (in C:\Windows\System32), which ships
    with that release on. Nothing extra to install.

Open the graphical UI
---------------------
  Double-click  uvcan-ui.bat

Make .uvsys files double-clickable
----------------------------------
  Double-click  install-association.bat  once. After that, double-clicking any
  .uvsys system package opens it directly in uvcan, and .uvsys files show the
  Usevolt icon. This is a per-user setting and needs no administrator rights.

  Keep this folder where it is after running the script. If you move it, run
  install-association.bat again from the new location. To undo the association,
  run uninstall-association.bat.

Run command-line commands
-------------------------
  Open a Command Prompt in this folder, e.g.:
    uvcan.exe --help
    uvcan.exe --sys "C:\path\to\system.uvsys" --ui
    uvcan.exe --can PCAN_USBBUS1 --listen 10

Keep uvcan.exe, PCANBasic.dll and the fonts\ folder together in one folder.
