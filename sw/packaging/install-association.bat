@echo off
rem ===========================================================
rem  Registers the .uvsys and .uvdev file associations for the
rem  current user.
rem
rem  Double-clicking either package will then open it with the
rem  uvcan.exe sitting in THIS folder, and the files get their
rem  Usevolt document icon (purple for a .uvsys system package,
rem  teal for a .uvdev device package). No administrator rights
rem  are needed: everything is written under HKEY_CURRENT_USER.
rem
rem  Run uninstall-association.bat to undo it. If you move this
rem  folder, just run this script again from the new location.
rem ===========================================================
setlocal

rem folder this script lives in, without the trailing backslash
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

echo Registering the .uvsys and .uvdev associations for the current user...
echo   folder: %HERE%

rem --- ProgIDs: description, icon and open command -------------------------
rem  Both extensions are opened by the same uvcan-open.bat, which looks at the
rem  extension to decide between uvcan's --sys and --dev options.
call :register "Usevolt.uvsys" ".uvsys" "uvsys.ico" "Usevolt CANopen system package"
call :register "Usevolt.uvdev" ".uvdev" "uvdev.ico" "Usevolt CANopen device package"

rem --- nudge Explorer to pick up the new icon/association -----------------
ie4uinit.exe -show >nul 2>&1

echo Done. Double-click any .uvsys or .uvdev file to open it in uvcan.
pause
goto :eof

rem --- register <progid> <extension> <icon file> <description> ------------
:register
reg add "HKCU\Software\Classes\%~1" /ve /d "%~4" /f >nul
reg add "HKCU\Software\Classes\%~1\DefaultIcon" /ve /d "\"%HERE%\%~3\"" /f >nul
reg add "HKCU\Software\Classes\%~1\shell\open\command" /ve /d "\"%HERE%\uvcan-open.bat\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\%~2" /ve /d "%~1" /f >nul
goto :eof
