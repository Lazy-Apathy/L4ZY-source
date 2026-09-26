@echo off
rem Rebuild the daily Sauer-RT client (x64). Requires w64devkit.
rem Output: ..\bin64\sauerbraten.exe
rem Uses the public NGX Vulkan gateway (dlaa-ngx.o / HWRT_NGX_GATEWAY),
rem not Streamline. Does not rebuild bin64\ngx-sr or bin64\nrd DLLs.
setlocal
set "PATH=%LOCALAPPDATA%\w64devkit\w64devkit\bin;%PATH%"
cd /d "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 (
    echo g++ introuvable. Installe w64devkit dans %%LOCALAPPDATA%%\w64devkit
    exit /b 1
)
if not exist "enet\libenet.a" (
    echo Building enet...
    pushd enet
    gcc -O3 -Iinclude -c callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c
    ar rcs libenet.a callbacks.o compress.o host.o list.o packet.o peer.o protocol.o unix.o win32.o
    popd
)
make -j8 PLATFORM=MINGW64 client
if errorlevel 1 exit /b 1
strip ..\bin64\sauerbraten.exe
echo OK: ..\bin64\sauerbraten.exe
endlocal
