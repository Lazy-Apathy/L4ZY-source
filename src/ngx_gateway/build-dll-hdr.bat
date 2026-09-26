@echo off
rem Builds the NGX gateway the released client loads: bin64\ngx-hdr\sauer_ngx.dll
rem (SAUER_NGX_ABI_VERSION 5) plus nvngx_dlss.dll from the NVIDIA DLSS SDK.
rem Needs MSVC x64 (third_party\fetch-msvc.ps1 or Visual Studio Build Tools)
rem and the DLSS SDK (third_party\fetch-ngx-sdk.ps1).
setlocal
set "SAUER_NGX_OUTDIR=ngx-hdr"
call "%~dp0build-dll-sr.bat"
exit /b %ERRORLEVEL%
