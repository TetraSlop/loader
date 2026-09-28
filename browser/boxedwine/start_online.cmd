@echo off
setlocal
cd /d "%~dp0..\.."
set "TM_BROWSER_SERVER=%~1"
if not defined TM_BROWSER_SERVER if exist "out\boxedwine\server\tmserver\__init__.py" set "TM_BROWSER_SERVER=out\boxedwine\server"
if not defined TM_BROWSER_SERVER set "TM_BROWSER_SERVER=..\tmserver\src"
"out\boxedwine-venv\Scripts\python.exe" -B "browser\boxedwine\serve_online.py" --server-dir "%TM_BROWSER_SERVER%"
