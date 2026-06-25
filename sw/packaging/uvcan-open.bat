@echo off
rem ===========================================================
rem  uvcan - open handler for .uvsys system packages
rem
rem  With a file argument (double-clicking a .uvsys file):
rem      uvcan.exe --sys "<file>" --ui
rem  With no argument (launched on its own):
rem      uvcan.exe --ui
rem
rem  cd into this folder first so the fonts\ directory and
rem  PCANBasic.dll are found no matter where this is launched from.
rem ===========================================================
cd /d "%~dp0"
if "%~1"=="" (
	start "" "uvcan.exe" --ui
) else (
	start "" "uvcan.exe" --sys "%~1" --ui
)
