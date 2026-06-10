@echo off
rem ===========================================================
rem  uvcan - opens the graphical configuration UI (uvcan --ui)
rem ===========================================================
rem  cd into this folder first so the fonts\ directory and
rem  PCANBasic.dll are found no matter where this is launched from.
cd /d "%~dp0"
start "" "uvcan.exe" --ui
