@echo off
rem MSVC NRD gateway DLL. Writes bin64\nrd\sauer_nrd.dll.
rem Does not replace bin64\ngx-sr or the live / skyage clients.
setlocal EnableExtensions
cd /d "%~dp0"
call build-nrd-lib.bat
if errorlevel 1 exit /b 1
call ..\ngx_gateway\find-msvc.bat
where cl >nul 2>&1
if errorlevel 1 exit /b 2
set "ROOT=%~dp0..\.."
set "NRD=%ROOT%\third_party\NRD"
set "BUILD=%NRD%\_build_sauer"
set "INCVK=%ROOT%\src\include"
set "OUT=%ROOT%\bin64\nrd"
mkdir "%OUT%" 2>nul
if exist "%OUT%\sauer_nrd.dll" copy /Y "%OUT%\sauer_nrd.dll" "%OUT%\sauer_nrd.prev.dll" >nul
set "NRDLIBDIR=%BUILD%"
if not exist "%NRDLIBDIR%\NRD.lib" if exist "%NRD%\_Bin\NRD.lib" set "NRDLIBDIR=%NRD%\_Bin"
if not exist "%NRDLIBDIR%\NRD.lib" if exist "%NRD%\_Bin\Release\NRD.lib" set "NRDLIBDIR=%NRD%\_Bin\Release"
set "BLOBDIR=%BUILD%\_deps\shadermake-build"
if not exist "%NRDLIBDIR%\ShaderMakeBlob.lib" if exist "%BLOBDIR%\ShaderMakeBlob.lib" copy /Y "%BLOBDIR%\ShaderMakeBlob.lib" "%NRDLIBDIR%\ShaderMakeBlob.lib" >nul
cl /nologo /LD /O2 /MD /EHsc /std:c++17 /W3 /DWIN32 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DVK_NO_PROTOTYPES /DNRD_STATIC_LIBRARY=1 /I"%NRD%\Include" /I"%INCVK%" /Fo"%OUT%\sauer_nrd.obj" /Fe"%OUT%\sauer_nrd.dll" sauer_nrd.cpp /link /DLL /DEF:sauer_nrd.def /MACHINE:X64 /INCREMENTAL:NO /LIBPATH:"%NRDLIBDIR%" /LIBPATH:"%BLOBDIR%" NRD.lib ShaderMakeBlob.lib kernel32.lib user32.lib
if errorlevel 1 (
    echo retry with MathLib.lib
    cl /nologo /LD /O2 /MD /EHsc /std:c++17 /W3 /DWIN32 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DVK_NO_PROTOTYPES /DNRD_STATIC_LIBRARY=1 /I"%NRD%\Include" /I"%INCVK%" /Fo"%OUT%\sauer_nrd.obj" /Fe"%OUT%\sauer_nrd.dll" sauer_nrd.cpp /link /DLL /DEF:sauer_nrd.def /MACHINE:X64 /INCREMENTAL:NO /LIBPATH:"%NRDLIBDIR%" /LIBPATH:"%BLOBDIR%" NRD.lib ShaderMakeBlob.lib kernel32.lib user32.lib
    if errorlevel 1 exit /b 1
)
copy /Y "%NRD%\LICENSE.txt" "%OUT%\LICENSE-NRD.txt" >nul
echo OK: %OUT%\sauer_nrd.dll
dir "%OUT%\sauer_nrd.dll"
endlocal
exit /b 0
