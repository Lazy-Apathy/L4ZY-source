@echo off
rem Builds the NGX gateway (SAUER_NGX_ABI_VERSION from include\ngx_gateway\sauer_ngx.h, now 5)
rem into bin64\%SAUER_NGX_OUTDIR% (default ngx-hdr, the folder the client loads).
rem bin64\ngx-sr (ABI 4) and bin64\ngx (ABI 1) are older reference DLLs: never build into them.
rem Archives the previous live DLL aside before overwriting.
setlocal EnableExtensions
cd /d "%~dp0"
call find-msvc.bat
where cl >nul 2>&1
if errorlevel 1 goto :nomsvc
set "ROOT=%~dp0..\.."
set "DLSS=%ROOT%\third_party\NVIDIA-DLSS"
set "LIBDIR=%DLSS%\lib\Windows_x86_64\x64"
set "INCDLSS=%DLSS%\include"
set "INCVK=%ROOT%\src\include"
if "%SAUER_NGX_OUTDIR%"=="" set "SAUER_NGX_OUTDIR=ngx-hdr"
if /I "%SAUER_NGX_OUTDIR%"=="ngx-sr" goto :refuse
if /I "%SAUER_NGX_OUTDIR%"=="ngx" goto :refuse
set "OUT=%ROOT%\bin64\%SAUER_NGX_OUTDIR%"
if not exist "%LIBDIR%\nvsdk_ngx_s.lib" goto :nolib
if not exist "%INCVK%\vulkan\vulkan.h" goto :novk
mkdir "%OUT%" 2>nul
mkdir "%OUT%\logs" 2>nul
if exist "%OUT%\sauer_ngx.dll" copy /Y "%OUT%\sauer_ngx.dll" "%OUT%\sauer_ngx.prev.dll" >nul
echo gateway -> %OUT%\sauer_ngx.dll  (bin64\ngx-sr ABI 4 and bin64\ngx ABI 1 are not touched)
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /DWIN32 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%INCDLSS%" /I"%INCVK%" /Fo"%OUT%\sauer_ngx.obj" /Fe"%OUT%\sauer_ngx.dll" sauer_ngx.cpp /link /DLL /DEF:sauer_ngx.def /MACHINE:X64 /INCREMENTAL:NO /LIBPATH:"%LIBDIR%" nvsdk_ngx_s.lib kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
if exist "%DLSS%\lib\Windows_x86_64\rel\nvngx_dlss.dll" copy /Y "%DLSS%\lib\Windows_x86_64\rel\nvngx_dlss.dll" "%OUT%\" >nul
echo OK: %OUT%\sauer_ngx.dll
dir "%OUT%\sauer_ngx.dll"
endlocal
exit /b 0

:refuse
echo Refused: bin64\%SAUER_NGX_OUTDIR% holds an older ABI kept for reference clients.
exit /b 2

:nomsvc
echo MSVC x64 toolchain not found. See REQUIREMENTS.txt
exit /b 2

:nolib
echo Missing %LIBDIR%\nvsdk_ngx_s.lib
exit /b 1

:novk
echo Missing Vulkan headers at %INCVK%\vulkan\vulkan.h
exit /b 1
