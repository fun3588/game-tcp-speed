@echo off
chcp 65001 >nul
taskkill /f /im sing-box.exe >nul 2>&1
echo [OK] Sing-box 核心进程已停止。
