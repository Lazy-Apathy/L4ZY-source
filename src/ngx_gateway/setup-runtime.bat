@echo off
rem Runtime folder for the isolated NGX client: nvngx_dlss.dll next to sauer_ngx.dll.
rem Does not touch bin64\sauerbraten.exe or bin64\streamline.
setlocal
cd /d "%~dp0..\.."
set "OUT=bin64\ngx"
set "DLSS=third_party\NVIDIA-DLSS\lib\Windows_x86_64"
mkdir "%OUT%\logs" 2>nul
if exist "%DLSS%\rel\nvngx_dlss.dll" (
    copy /Y "%DLSS%\rel\nvngx_dlss.dll" "%OUT%\" >nul
) else if exist "bin64\streamline\nvngx_dlss.dll" (
    copy /Y "bin64\streamline\nvngx_dlss.dll" "%OUT%\" >nul
) else (
    echo Missing nvngx_dlss.dll. Clone NVIDIA/DLSS or run src\setup-ngx.bat
    exit /b 1
)
if exist "%OUT%\sauer_ngx.dll" (
    echo Runtime: %OUT%\sauer_ngx.dll + nvngx_dlss.dll
) else (
    echo Runtime: %OUT%\nvngx_dlss.dll
    echo Build the gateway with src\ngx_gateway\build-dll.bat once MSVC is installed.
)
dir "%OUT%\nvngx_dlss.dll"
if exist "%OUT%\sauer_ngx.dll" dir "%OUT%\sauer_ngx.dll"
echo nvngx.dll comes from the NVIDIA driver, not this folder.
echo Override search path with SAUER_NGX_DIR.
endlocal
