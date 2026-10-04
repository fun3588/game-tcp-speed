@echo off
title 恢复 Proxifier 驱动

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] 正在请求管理员权限... 请在弹出的 UAC 提示中点击【是】
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo 正在重新启动 Proxifier 驱动服务...
sc.exe create ProxifierDrv type= kernel start= demand binPath= "C:\Program Files (x86)\Proxifier\Driver\ProxifierDrv.sys" >nul 2>&1
sc.exe start ProxifierDrv
if %errorlevel% equ 0 (
    echo [OK] Proxifier 驱动已恢复运行！
) else (
    echo [!] 启动失败，请检查驱动文件是否存在。
)
pause
