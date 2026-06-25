@echo off
rem ===========================================================
rem  Registers the .uvsys file association for the current user.
rem
rem  Double-clicking a .uvsys file will then open it with the
rem  uvcan.exe sitting in THIS folder, and .uvsys files get the
rem  Usevolt document icon. No administrator rights are needed:
rem  everything is written under HKEY_CURRENT_USER.
rem
rem  Run uninstall-association.bat to undo it. If you move this
rem  folder, just run this script again from the new location.
rem ===========================================================
setlocal

rem folder this script lives in, without the trailing backslash
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

set "PROGID=Usevolt.uvsys"

echo Registering .uvsys association for the current user...
echo   folder: %HERE%

rem --- ProgID: description, icon and open command -------------------------
reg add "HKCU\Software\Classes\%PROGID%" /ve /d "Usevolt CANopen system package" /f >nul
reg add "HKCU\Software\Classes\%PROGID%\DefaultIcon" /ve /d "\"%HERE%\uvsys.ico\"" /f >nul
reg add "HKCU\Software\Classes\%PROGID%\shell\open\command" /ve /d "\"%HERE%\uvcan-open.bat\" \"%%1\"" /f >nul

rem --- Extension -> ProgID ------------------------------------------------
reg add "HKCU\Software\Classes\.uvsys" /ve /d "%PROGID%" /f >nul

rem --- nudge Explorer to pick up the new icon/association -----------------
ie4uinit.exe -show >nul 2>&1

echo Done. Double-click any .uvsys file to open it in uvcan.
pause
