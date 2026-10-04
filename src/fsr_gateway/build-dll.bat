@echo off
rem FSR 3.1 gateway (ABI 1): bin64\fsr\sauer_fsr.dll, separate from every NGX folder.
rem Contains the AMD FidelityFX SDK v1.1.4 FSR 3.1 upscaler + Vulkan backend only
rem (no frame generation, no frame interpolation swap chain, no optical flow).
rem Needs third_party\FidelityFX-SDK (third_party\fetch-fidelityfx-sdk.ps1) and an MSVC
rem x64 toolchain (portable tree third_party\msvc accepted). Generated shader headers
rem and objects go to third_party\FidelityFX-SDK\sauer-build (outside git).
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "SRC=%~dp0.."
set "ROOT=%~dp0..\.."
set "SDK=%ROOT%\third_party\FidelityFX-SDK\sdk"
set "GEN=%ROOT%\third_party\FidelityFX-SDK\sauer-build"
set "SHADERS=%GEN%\fsr-shaders"
set "OBJ=%GEN%\obj"
set "OUT=%ROOT%\bin64\fsr"
if not exist "%SDK%\include\FidelityFX\host\ffx_fsr3upscaler.h" goto :nosdk
if not exist "%SRC%\include\vulkan\vulkan.h" goto :novk
where cl >nul 2>&1
if errorlevel 1 if exist "%ROOT%\third_party\msvc\setup_x64.bat" call "%ROOT%\third_party\msvc\setup_x64.bat"
where cl >nul 2>&1
if errorlevel 1 if exist "%ROOT%\..\..\third_party\msvc\setup_x64.bat" call "%ROOT%\..\..\third_party\msvc\setup_x64.bat"
where cl >nul 2>&1
if errorlevel 1 goto :nomsvc
mkdir "%SHADERS%" 2>nul
mkdir "%OBJ%" 2>nul
mkdir "%OUT%" 2>nul

rem 1. Vulkan shader permutations of the upscaler, as sdk\src\backends\vk\CMakeShadersFSR3Upscaler.txt
rem    and include\FidelityFX\gpu\fsr3upscaler\CMakeCompileFSR3UpscalerShaders.txt do it.
set "SC=%SDK%\tools\binary_store\FidelityFX_SC.exe"
set "GPU=%SDK%\include\FidelityFX\gpu"
set "BASE=-reflection -deps=gcc -DFFX_GPU=1 -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2"
set "API=-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1"
set "PERM=-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1} -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1} -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1} -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1} -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
set "INC=-I%GPU% -I%GPU%\fsr3upscaler"
set "GL=-glslangexe=%SDK%\tools\binary_store\glslangValidator.exe"
for %%S in ("%SDK%\src\backends\vk\shaders\fsr3upscaler\*.glsl") do (
    set "N=%%~nS"
    "%SC%" %GL% %BASE% %API% %PERM% -name=!N! -DFFX_HALF=0 %INC% -output=%SHADERS% "%%S" >nul || goto :shaderfail
    "%SC%" %GL% %BASE% %API% %PERM% -name=!N!_wave64 -DFFX_HALF=0 %INC% -output=%SHADERS% "%%S" >nul || goto :shaderfail
    "%SC%" %GL% %BASE% %API% %PERM% -name=!N!_16bit -DFFX_HALF=1 %INC% -output=%SHADERS% "%%S" >nul || goto :shaderfail
    "%SC%" %GL% %BASE% %API% %PERM% -name=!N!_wave64_16bit -DFFX_HALF=1 %INC% -output=%SHADERS% "%%S" >nul || goto :shaderfail
)

rem 2. Gateway DLL: our C ABI + upscaler + Vulkan backend + shared helpers, static CRT.
set "FFXSRC=%SDK%\src\components\fsr3upscaler\ffx_fsr3upscaler.cpp %SDK%\src\shared\ffx_assert.cpp %SDK%\src\shared\ffx_message.cpp %SDK%\src\shared\ffx_object_management.cpp %SDK%\src\shared\ffx_breadcrumbs_list.cpp %SDK%\src\backends\shared\ffx_shader_blobs.cpp %SDK%\src\backends\shared\blob_accessors\ffx_fsr3upscaler_shaderblobs.cpp %SDK%\src\backends\vk\ffx_vk.cpp"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /DWIN32 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /DFFX_FSR3UPSCALER ^
  /I"%SRC%\include" /I"%SDK%\include" /I"%SDK%\src\shared" /I"%SDK%\src\backends\shared" /I"%SDK%\src\components" /I"%SHADERS%" ^
  /Fo"%OBJ%\\" /Fe"%OUT%\sauer_fsr.dll" sauer_fsr.cpp %FFXSRC% ^
  /link /DLL /DEF:sauer_fsr.def /IMPLIB:"%OBJ%\sauer_fsr.lib" /MACHINE:X64 /INCREMENTAL:NO /OPT:REF /OPT:ICF kernel32.lib user32.lib
if errorlevel 1 exit /b 1
copy /Y "%ROOT%\third_party\FidelityFX-SDK\LICENSE.txt" "%OUT%\LICENSE-FidelityFX.txt" >nul
echo OK: %OUT%\sauer_fsr.dll
endlocal
exit /b 0

:nomsvc
echo MSVC x64 toolchain not found (third_party\msvc\setup_x64.bat).
exit /b 2
:nosdk
echo Missing %SDK% : run third_party\fetch-fidelityfx-sdk.ps1
exit /b 1
:novk
echo Missing Vulkan headers at %SRC%\include\vulkan\vulkan.h
exit /b 1
:shaderfail
echo FidelityFX_SC failed on %N%
exit /b 1
