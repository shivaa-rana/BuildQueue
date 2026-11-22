@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 goto failed
echo Starting BuildQueue in Ubuntu. The first launch compiles the desktop app.
wsl.exe -d Ubuntu --exec /bin/sh ./run.sh
set "result=%ERRORLEVEL%"
popd
if "%result%"=="0" exit /b 0
:failed
echo.
echo BuildQueue did not start. The launcher requires Ubuntu on WSL 2
echo with the Qt development packages and WSL GUI support.
echo See readme.txt for setup and the error above for details.
pause
exit /b 1
