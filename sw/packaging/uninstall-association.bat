@echo off
rem ===========================================================
rem  Removes the .uvsys and .uvdev file associations for the
rem  current user that install-association.bat created. Leaves
rem  uvcan.exe and your package files untouched.
rem ===========================================================
setlocal

echo Removing the .uvsys and .uvdev associations for the current user...

call :unregister "Usevolt.uvsys" ".uvsys"
call :unregister "Usevolt.uvdev" ".uvdev"

ie4uinit.exe -show >nul 2>&1

echo Done.
pause
goto :eof

rem --- unregister <progid> <extension> ------------------------------------
:unregister
reg delete "HKCU\Software\Classes\%~1" /f >nul 2>&1
reg delete "HKCU\Software\Classes\%~2" /f >nul 2>&1
rem drop any per-user "open with" choice Explorer may have recorded
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\%~2" /f >nul 2>&1
goto :eof
