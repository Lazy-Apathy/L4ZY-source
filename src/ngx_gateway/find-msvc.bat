@echo off
rem Locate an MSVC x64 toolchain without changing git config or installing anything.
rem Order: already-on-PATH cl.exe, vswhere, VS 2022/2019, portable tree third_party\msvc.
set "SAUER_MSVC_OK="
where cl >nul 2>&1
if not errorlevel 1 set "SAUER_MSVC_OK=1"
if defined SAUER_MSVC_OK goto :eof

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_vs2022_bt
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "SAUER_VSROOT=%%i"
if not defined SAUER_VSROOT goto :try_vs2022_bt
if not exist "%SAUER_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto :try_vs2022_bt
call "%SAUER_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_vs2022_bt
if not exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" goto :try_vs2022_com
call "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_vs2022_com
if not exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" goto :try_vs2022_pro
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_vs2022_pro
if not exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" goto :try_vs18
call "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_vs18
if not exist "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" goto :try_vs2019
call "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_vs2019
if not exist "%PF86%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" goto :try_portable
call "%PF86%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof

:try_portable
if not exist "%~dp0..\..\third_party\msvc\setup_x64.bat" goto :try_portable2
call "%~dp0..\..\third_party\msvc\setup_x64.bat"
set "SAUER_MSVC_OK=1"
goto :eof

:try_portable2
if not exist "%~dp0..\..\third_party\msvc\VC\Auxiliary\Build\vcvars64.bat" goto :eof
call "%~dp0..\..\third_party\msvc\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SAUER_MSVC_OK=1"
goto :eof
