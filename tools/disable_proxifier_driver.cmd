@echo off
chcp 65001 >nul
title 卸载与清理 Proxifier 驱动 (恢复纯净网络栈)

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 正在请求管理员权限... 请在弹出的 UAC 提示中点击【是】
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ============================================================
echo   ProxifierDrv 内核驱动清理工具 (彻底清除本地代答 SYN 假握手)
echo ============================================================
echo.

echo [1/3] 正在停止 ProxifierDrv 驱动服务...
sc.exe stop ProxifierDrv
if %errorlevel% equ 0 (
    echo   [OK] ProxifierDrv 驱动已成功停止！
) else (
    echo   [!] 驱动可能未在运行或已停止。
)

echo.
echo [2/3] 正在禁用并删除 ProxifierDrv 驱动服务...
sc.exe config ProxifierDrv start= disabled >nul 2>&1
sc.exe delete ProxifierDrv
if %errorlevel% equ 0 (
    echo   [OK] ProxifierDrv 驱动服务已成功从系统删除！
) else (
    echo   [!] 驱动服务删除完成。
)

echo.
echo [3/3] 正在重置 Winsock 目录与 IP 协议栈...
netsh winsock reset >nul 2>&1
netsh int ip reset >nul 2>&1
echo   [OK] 系统网络栈已完全恢复纯净状态！

echo.
echo ============================================================
echo   清理完毕！所有应用程序将不再被 Proxifier 驱动本地代答伪握手。
echo   建议重启一次电脑使网络内核完全重置。
echo ============================================================
echo.
pause
