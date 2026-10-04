@echo off
rem Frozen DLAA path: do not overwrite bin64\ngx\sauer_ngx.dll (ABI 1).
rem The client gateway (ABI 5) is built by build-dll-hdr.bat into bin64\ngx-hdr.
echo.
echo Do not rebuild bin64\ngx\sauer_ngx.dll — it is the DLAA ABI 1 runtime
echo for sauerbraten-ngx.exe / sauerbraten-ngx-dlaa.exe.
echo.
echo Client gateway: src\ngx_gateway\build-dll-hdr.bat  -^> bin64\ngx-hdr
echo.
call "%~dp0build-dll-sr.bat"
exit /b %ERRORLEVEL%
