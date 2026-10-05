@echo off
title Firestorm Accel (IP alias + SOCKS5 relay)

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... please click YES on the UAC dialog.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"
set "SCR=%~dp0firestorm_accel.py"

echo ============================================================
echo   Firestorm accelerator  (IP alias + local SOCKS5 relay)
echo   Script : %SCR%
echo   Target : 141.95.207.63  ports 1119 / 8285 / 8286
echo   Proxy  : SOCKS5 127.0.0.1:2080  (Throne)
echo ------------------------------------------------------------
echo   CLOSING THIS WINDOW = stop + auto restore (alias removed)
echo   Prefer Ctrl+C to stop cleanly.
echo ============================================================
echo.

"%PY%" "%SCR%" setup

echo.
echo [exited]  If the alias is still present, run stop_accel.cmd
pause
