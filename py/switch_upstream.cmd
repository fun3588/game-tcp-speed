@echo off
setlocal
title Firestorm Accel - switch upstream

rem ============================================================
rem  切换加速器的上游 SOCKS5（需要管理员权限）
rem
rem  用法:
rem    switch_upstream.cmd            切到主实例 sing-box（端口自动读）
rem    switch_upstream.cmd backup     切到备实例 sing-box（端口自动读）
rem    switch_upstream.cmd main       同上（切回主实例）
rem    switch_upstream.cmd throne     切回 Throne  (127.0.0.1:2080)
rem    switch_upstream.cmd 12345      切到指定端口 (127.0.0.1:12345)
rem    switch_upstream.cmd 1.2.3.4:1080  切到指定地址
rem
rem  注意: sing-box 的入口端口是随机的，所以默认分支必须现读
rem        Sing-box\runtime.json / runtime.backup.json
rem ============================================================

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
set "ARG=%~1"

rem 先把参数归到 main / backup / throne / explicit 四类
set "WANT="
if not defined ARG set "WANT=main"
if /i "%ARG%"=="main"    set "WANT=main"
if /i "%ARG%"=="sb"      set "WANT=main"
if /i "%ARG%"=="singbox" set "WANT=main"
if /i "%ARG%"=="backup"  set "WANT=backup"
if /i "%ARG%"=="throne"  set "WANT=throne"
if not defined WANT if defined ARG set "WANT=explicit"

if /i "%WANT%"=="explicit" goto explicit
if /i "%WANT%"=="throne"   goto use_throne

rem 从该 profile 自己的 runtime 文件读入口端口
rem 注意: usebackq 里命令两端的 "" 不能省（cmd 会剥掉最外层引号）
set "SP="
for /f "usebackq delims=" %%i in (`""%PY%" "%SBX%\portof.py" socks "" %WANT%"`) do set "SP=%%i"
if not defined SP (
    echo [X] 读不到 %WANT% 实例的端口（runtime 文件不存在或已被清理）。
    echo     先启动它:  %SBX%\run-singbox.cmd %WANT%
    echo.
    pause
    exit /b 1
)
set "SOCKS=127.0.0.1:%SP%"
goto have_socks

:use_throne
set "SOCKS=127.0.0.1:2080"
goto have_socks

:explicit
rem 带冒号当完整地址，否则当端口
echo %ARG% | findstr ":" >nul
if errorlevel 1 (
    set "SOCKS=127.0.0.1:%ARG%"
) else (
    set "SOCKS=%ARG%"
)
goto have_socks

:have_socks
echo ============================================================
echo   切换加速器上游
echo   新上游: SOCKS5 %SOCKS%
echo ------------------------------------------------------------
echo   这会断开游戏当前连接（客户端一般会自动重连）
echo ============================================================
echo.

echo [1/3] 停止现有中继进程...
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*firestorm_accel*' } | ForEach-Object { Write-Host ('    停止 PID ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force }"
timeout /t 2 /nobreak >nul

echo.
echo [2/3] 检查副 IP 与上游连通性...
"%PY%" "%SCR%" status

echo.
echo [3/3] 启动中继（上游 %SOCKS%）...
echo      关掉本窗口即停止并自动清理副 IP。
echo.
"%PY%" "%SCR%" setup --socks %SOCKS%

echo.
echo [exited] 若副 IP 仍残留，运行 stop_accel.cmd 清理。
pause
