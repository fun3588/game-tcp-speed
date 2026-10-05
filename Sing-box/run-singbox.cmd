@echo off
setlocal
cd /d "%~dp0"

rem 用法:
rem   run-singbox.cmd           启动主实例（main，走 Cloudflare 隧道）
rem   run-singbox.cmd backup    启动备实例（backup，走直连 VPS）
rem   run-singbox.cmd main --keep-port   沿用上次端口

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"

set "PROF=%~1"
if /i "%PROF%"=="backup" goto prof_ok
set "PROF=main"
:prof_ok

if not exist "%~dp0sing-box.exe" (
  echo [X] 找不到 sing-box.exe
  pause
  exit /b 1
)
if not exist "%~dp0config.json" (
  echo [X] 找不到 config.json
  pause
  exit /b 1
)

"%PY%" "%~dp0start_singbox.py" --profile %PROF% %2 %3 %4 %5
set "RC=%errorlevel%"

echo.
echo [i] %PROF% 实例已退出 (rc=%RC%)
pause
