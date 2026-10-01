@echo off
rem Compile le jeu L4ZY depuis src\ vers distrib\.cache\game\sauerbraten.exe.
rem Ne touche PAS bin64\sauerbraten.exe (le client de l'atelier).
rem Requiert w64devkit (%LOCALAPPDATA%\w64devkit ou variable W64DEVKIT).
setlocal
if "%W64DEVKIT%"=="" set "W64DEVKIT=%LOCALAPPDATA%\w64devkit\w64devkit\bin"
set "PATH=%PATH%;%W64DEVKIT%"
set "OUT=%~dp0..\.cache\game"
if not exist "%OUT%" mkdir "%OUT%"
for %%I in ("%OUT%") do set "OUT=%%~fI"
rem make (sh) veut des / : C:/.../game
set "OUTF=%OUT:\=/%"
cd /d "%~dp0..\..\src"
make -j8 PLATFORM=MINGW64 hdrint HDRINT_OUT="%OUTF%/sauerbraten.exe" || exit /b 1
if not exist "%OUT%\sauerbraten.exe" (echo ECHEC: pas de sauerbraten.exe & exit /b 1)
strip "%OUT%\sauerbraten.exe"
echo OK: %OUT%\sauerbraten.exe
endlocal
