@echo off
title 将 ProxifierDrv 驱动设为按需启动 (demand)

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 正在请求管理员权限... 请在弹出的 UAC 提示中点击【是】
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ============================================================
echo   正在将 ProxifierDrv 驱动配置为【按需启动 (demand)】
echo ============================================================
echo.

echo [1/2] 正在将 ProxifierDrv 服务启动类型修改为 demand (按需启动)...
sc.exe config ProxifierDrv start= demand
if %errorlevel% equ 0 (
    echo   [OK] 服务配置成功！当前启动模式: 按需启动 (demand)
) else (
    echo   [!] 修改服务启动类型失败。
)

echo.
echo [2/2] 正在停止当前正在运行的 ProxifierDrv 驱动...
sc.exe stop ProxifierDrv
if %errorlevel% equ 0 (
    echo   [OK] ProxifierDrv 驱动已成功停止！
) else (
    echo   [!] 驱动可能未在运行或已停止。
)

echo.
echo ============================================================
echo   【调整完成！】
echo   1. 平时不运行 Proxifier 时：驱动不会随开机加载，ping/connect 完全恢复真值！
echo   2. 主动打开 Proxifier 软件时：系统会自动按需拉起驱动，Proxifier 照样正常使用！
echo ============================================================
echo.
pause
