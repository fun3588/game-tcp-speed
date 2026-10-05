@echo off
setlocal
title Proxifier 驱动开关（修掉假的 ping / connect 延迟）

rem ============================================================
rem  背景：
rem  ProxifierDrv 是内核驱动，装完默认 start=Automatic —— **开机就加载**，
rem  哪怕 Proxifier 界面根本没运行。它在网络栈里会本地代答 SYN/ICMP，
rem  于是 ping 任何公网 IP 都是 1~2ms、connect() 任何 IP 都是 3ms，
rem  连 192.0.2.1（IANA 保留地址，公网不该路由）都能"连上"。
rem  这些数全是假的，真实直连延迟要用"发请求 + 等首字节"才量得到。
rem
rem  用法（都要管理员，本脚本会自动提权）：
rem    proxifier-driver.cmd         停止驱动 + 改为按需加载（推荐）
rem    proxifier-driver.cmd auto    恢复开机自动加载
rem    proxifier-driver.cmd status  只看状态
rem
rem  改成 demand 之后：平时不再加载，ping/connect 恢复真值；
rem  你主动开 Proxifier 时它会按需加载，Proxifier 照样能用。
rem ============================================================

net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights... please click YES on the UAC dialog.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    exit /b
)

set "MODE=%~1"
if "%MODE%"=="" set "MODE=off"

if /i "%MODE%"=="status" goto status

if /i "%MODE%"=="off" (
    echo [1/2] 停止 ProxifierDrv ...
    sc stop ProxifierDrv
    echo.
    echo [2/2] 改为按需加载（不再开机自动加载）
    sc config ProxifierDrv start= demand
    echo.
    echo 完成。现在再 ping 8.8.8.8 / 141.95.207.63 看看，应该不再是 1~2ms 了。
    echo 注意：改完**要重启**才会真正不加载（本次只是停掉了当前这个）。
    echo 恢复开机自启请运行: %~nx0 auto
    goto status
)

if /i "%MODE%"=="auto" (
    echo 恢复为开机自动加载 ...
    sc config ProxifierDrv start= auto
    echo 完成（下次重启生效）。
    goto status
)

echo 未知参数: %MODE%
echo 用法: %~nx0 [off ^| auto ^| status]
goto end

:status
echo.
echo ===== 当前状态 =====
sc query ProxifierDrv
sc qc ProxifierDrv

:end
echo.
pause
