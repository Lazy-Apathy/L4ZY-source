@echo off
rem Frozen DLAA path: do not overwrite bin64\ngx\sauer_ngx.dll (ABI 1).
rem ABI 2 Super Resolution DLL is built by build-dll-sr.bat into bin64\ngx-sr.
echo.
echo Do not rebuild bin64\ngx\sauer_ngx.dll — it is the DLAA ABI 1 runtime
echo for sauerbraten-ngx.exe / sauerbraten-ngx-dlaa.exe.
echo.
echo For DLSS Quality: src\ngx_gateway\build-dll-sr.bat  -^> bin64\ngx-sr
echo.
call "%~dp0build-dll-sr.bat"
exit /b %ERRORLEVEL%
