@echo off
setlocal
set "TM_BROWSER_OUT=%~dp0..\..\out\boxedwine"
if not exist "%TM_BROWSER_OUT%" mkdir "%TM_BROWSER_OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++23preview /W4 /MT /O2 /D_WINSOCK_DEPRECATED_NO_WARNINGS "%~dp0socket_probe.cpp" /Fo:"%TM_BROWSER_OUT%\socket_probe.obj" /Fe:"%TM_BROWSER_OUT%\socket_probe.exe" /link /MACHINE:X86 /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ws2_32.lib
