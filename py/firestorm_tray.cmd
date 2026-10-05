@echo off
setlocal

set "DIR=%~dp0"
set "PYW=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\pythonw.exe"
set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PYW%" set "PYW="
if not exist "%PY%" set "PY=python"

if defined PYW (
    start "" "%PYW%" "%DIR%firestorm_tray.py"
) else (
    start "" "%PY%" "%DIR%firestorm_tray.py"
)
exit /b
