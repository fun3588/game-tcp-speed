@echo off
setlocal
title Firestorm Accel - independent sing-box

rem 用法:
rem   run_accel_singbox.cmd           走主实例 sing-box
rem   run_accel_singbox.cmd backup    走备实例 sing-box

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... please click YES on the UAC dialog.
    powershell -NoProfile -Command "$a = '%*'; if ($a) { Start-Process -FilePath '%~f0' -ArgumentList $a -Verb RunAs } else { Start-Process -FilePath '%~f0' -Verb RunAs }"
    exit /b
)

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"
set "SCR=%~dp0firestorm_accel.py"
set "SBX=%~dp0..\Sing-box"

set "PROF=%~1"
if /i "%PROF%"=="backup" goto prof_ok
set "PROF=main"
:prof_ok

rem sing-box 的入口端口是随机的，从该 profile 的 runtime 文件取
rem 注意: usebackq 里命令两端的 "" 不能省（cmd 会剥掉最外层引号）
set "SOCKS_PORT="
for /f "usebackq delims=" %%i in (`""%PY%" "%SBX%\portof.py" socks "" %PROF%"`) do set "SOCKS_PORT=%%i"
if "%SOCKS_PORT%"=="" (
    echo [!] 读不到 %PROF% 实例的端口，它是不是还没启动？
    echo     启动:  %SBX%\run-singbox.cmd %PROF%
    echo.
    pause
    exit /b 1
)
set "SOCKS=127.0.0.1:%SOCKS_PORT%"

echo ============================================================
echo   Firestorm accelerator  (IP alias + local SOCKS5 relay)
echo   Script : %SCR%
echo   Target : 141.95.207.63  ports 1119 / 3724 / 8285 / 8286
echo   Proxy  : SOCKS5 %SOCKS%   [%PROF% 实例]
echo ------------------------------------------------------------
echo   Requires that sing-box instance already running:
echo       %SBX%\run-singbox.cmd %PROF%
echo ------------------------------------------------------------
echo   CLOSING THIS WINDOW = stop + auto restore (alias removed)
echo   Prefer Ctrl+C to stop cleanly.
echo ============================================================
echo.

"%PY%" "%SCR%" setup --socks %SOCKS%

echo.
echo [exited]  If the alias is still present, run stop_accel.cmd
pause
