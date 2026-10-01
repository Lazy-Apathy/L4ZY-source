@echo off
rem Construit L4ZY.exe (lanceur). Requiert w64devkit (g++ MinGW).
setlocal
if "%W64DEVKIT%"=="" set "W64DEVKIT=%LOCALAPPDATA%\w64devkit\w64devkit\bin"
set "PATH=%W64DEVKIT%;%PATH%"
cd /d "%~dp0"
if "%1"=="" (set "OUT=L4ZY.exe") else (set "OUT=%~1")
windres launcher.rc -O coff -o launcher.res || exit /b 1
g++ -O2 -s -Wl,--no-insert-timestamp -municode -mwindows -static -static-libgcc -static-libstdc++ -Wall -o "%OUT%" launcher.cpp launcher.res -lws2_32 -lbcrypt -lshell32 -lole32 -luuid -luser32 || exit /b 1
echo OK: %OUT%
