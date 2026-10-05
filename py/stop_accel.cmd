@echo off
title Firestorm Accel - teardown

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... please click YES on the UAC dialog.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"
set "SCR=%~dp0firestorm_accel.py"

echo Removing IP alias and restoring direct connection...
"%PY%" "%SCR%" teardown
"%PY%" "%SCR%" status
echo.
pause
