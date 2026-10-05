@echo off
setlocal
cd /d "%~dp0"

rem 一键把两个实例都起来（各自一个最小化的窗口，关掉窗口即停该实例）
rem   主实例 main   -> 走 Cloudflare 隧道节点
rem   备实例 backup -> 走直连 VPS 节点

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"

if not exist "%~dp0sing-box.exe" (
  echo [X] 找不到 sing-box.exe
  pause
  exit /b 1
)

echo ============================================================
echo   启动两个独立 sing-box 实例
echo     主实例 main   （隧道节点）
echo     备实例 backup （直连节点）
echo ------------------------------------------------------------
echo   会弹出两个最小化的窗口，不要关掉它们
echo   端口都是随机的，看窗口输出或 runtime 文件
echo ============================================================
echo.

start "sing-box 主实例" /min "%PY%" "%~dp0start_singbox.py" --profile main
timeout /t 3 /nobreak >nul
start "sing-box 备实例" /min "%PY%" "%~dp0start_singbox.py" --profile backup
timeout /t 5 /nobreak >nul

echo 当前状态:
"%PY%" "%~dp0portof.py" list
echo.
echo (格式: profile  入口端口  控制端口  默认节点  PID)
echo.
pause
