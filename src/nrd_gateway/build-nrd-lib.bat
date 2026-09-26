@echo off
rem Build NVIDIA NRD v4.17.3 as a static MSVC library with embedded SPIR-V.
rem Does not touch NGX runtimes or the live client.
setlocal EnableExtensions
cd /d "%~dp0"
call ..\ngx_gateway\find-msvc.bat
where cl >nul 2>&1
if errorlevel 1 (
    echo MSVC x64 toolchain not found.
    exit /b 2
)
set "ROOT=%~dp0..\.."
set "NRD=%ROOT%\third_party\NRD"
set "CMAKE=%ROOT%\third_party\cmake\bin\cmake.exe"
set "DXC=%ROOT%\third_party\NRD\_build_sauer\_deps\dxc-src\bin\x64\dxc.exe"
if not exist "%DXC%" set "DXC=%ROOT%\third_party\msvc\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
set "BUILD=%NRD%\_build_sauer"
if not exist "%NRD%\CMakeLists.txt" (
    echo Missing %NRD%
    exit /b 1
)
if not exist "%CMAKE%" (
    echo Missing portable CMake at %CMAKE%
    exit /b 1
)
if not exist "%DXC%" (
    echo Missing DXC at %DXC%
    exit /b 1
)
if exist "%BUILD%\NRD.lib" if exist "%BUILD%\_deps\shadermake-build\ShaderMakeBlob.lib" goto :skipnrd
mkdir "%BUILD%" 2>nul
"%CMAKE%" -S "%NRD%" -B "%BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNRD_STATIC_LIBRARY=ON -DNRD_NRI=OFF -DNRD_EMBEDS_SPIRV_SHADERS=ON -DNRD_EMBEDS_DXIL_SHADERS=OFF -DNRD_EMBEDS_DXBC_SHADERS=OFF -DNRD_NORMAL_ENCODING=3 -DNRD_ROUGHNESS_ENCODING=1 -DNRD_SUPPORTS_CHECKERBOARD=OFF -DNRD_SUPPORTS_HISTORY_CONFIDENCE=OFF -DNRD_SUPPORTS_DISOCCLUSION_THRESHOLD_MIX=OFF -DNRD_SUPPORTS_ANTIFIREFLY=ON -DNRD_SUPPORTS_VIEWPORT_OFFSET=OFF -DDXC_PATH="%DXC%" -DDXC_SPIRV_PATH="%DXC%" -DSHADERMAKE_DXC_PATH="%DXC%" -DSHADERMAKE_DXC_VK_PATH="%DXC%"
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILD%" --config Release
if errorlevel 1 exit /b 1
:skipnrd
if not exist "%BUILD%\NRD.lib" if exist "%NRD%\_Bin\NRD.lib" copy /Y "%NRD%\_Bin\NRD.lib" "%BUILD%\NRD.lib" >nul
if not exist "%BUILD%\NRD.lib" if exist "%NRD%\_Bin\Release\NRD.lib" copy /Y "%NRD%\_Bin\Release\NRD.lib" "%BUILD%\NRD.lib" >nul
if not exist "%BUILD%\ShaderMakeBlob.lib" (
    for /f "delims=" %%F in ('dir /s /b "%BUILD%\ShaderMakeBlob.lib" "%NRD%\_build_sauer\*\ShaderMakeBlob.lib" "%ROOT%\third_party\NRD\_build_sauer\_deps\*\ShaderMakeBlob.lib" 2^>nul') do (
        if not exist "%BUILD%\ShaderMakeBlob.lib" copy /Y "%%F" "%BUILD%\ShaderMakeBlob.lib" >nul
    )
)
if not exist "%BUILD%\NRD.lib" (
    echo NRD.lib was not produced
    dir /s /b "%BUILD%\*.lib" "%NRD%\_Bin\*.lib"
    exit /b 1
)
echo OK: %BUILD%\NRD.lib
endlocal
exit /b 0
