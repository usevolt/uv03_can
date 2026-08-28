@echo off
rem ===========================================================
rem  uvcan - open handler for .uvsys and .uvdev packages
rem
rem  With a file argument (double-clicking a package), the file's
rem  extension picks the option it is handed to:
rem      *.uvsys ->  uvcan.exe --sys "<file>" --ui
rem      *.uvdev ->  uvcan.exe --dev "<file>" --ui
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
	rem an unknown extension is opened as a system package, which is what
	rem every earlier version of this script did with any argument
	if /i "%~x1"==".uvdev" (
		start "" "uvcan.exe" --dev "%~1" --ui
	) else (
		start "" "uvcan.exe" --sys "%~1" --ui
	)
)
