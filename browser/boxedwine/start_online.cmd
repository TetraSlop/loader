@echo off
setlocal
cd /d "%~dp0..\.."
if "%~3"=="" goto usage
if not "%~4"=="" goto usage
"out\boxedwine-venv\Scripts\python.exe" -B "browser\boxedwine\network_gateway.py" --server "%~1" --game-port "%~2" --profile-port "%~3"
exit /b %errorlevel%
:usage
echo Usage: start_online.cmd SERVER_HOST GAME_PORT PROFILE_PORT
echo Connects the browser gateway to an already running, separate game server.
exit /b 2
