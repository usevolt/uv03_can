@echo off
rem ===========================================================
rem  Removes the .uvsys file association for the current user
rem  that install-association.bat created. Leaves uvcan.exe and
rem  your .uvsys files untouched.
rem ===========================================================
setlocal

set "PROGID=Usevolt.uvsys"

echo Removing the .uvsys association for the current user...

reg delete "HKCU\Software\Classes\%PROGID%" /f >nul 2>&1
reg delete "HKCU\Software\Classes\.uvsys" /f >nul 2>&1

rem drop any per-user "open with" choice Explorer may have recorded
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.uvsys" /f >nul 2>&1

ie4uinit.exe -show >nul 2>&1

echo Done.
pause
