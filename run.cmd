@echo off
chcp 65001 >nul

:: 检查是否具备管理员权限
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] 正在以管理员权限启动专线加速器...
    powershell -Command "Start-Process '%~dp0run.cmd' -Verb RunAs"
    exit /b
)

cd /d "%~dp0c"
start "" "%~dp0c\launcher_gui.exe"
