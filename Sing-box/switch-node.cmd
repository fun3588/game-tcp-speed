@echo off
chcp 65001 >nul
setlocal

rem ============================================================
rem  切换 sing-box 当前出站节点（在线热切换）
rem
rem  用法:
rem    switch-node.cmd 0001            切换为节点 0001
rem    switch-node.cmd 0709            切换为节点 0709
rem    switch-node.cmd hy2             切换为 Hysteria 2 直连专线
rem    switch-node.cmd vless           切换为 VLESS Reality 专线
rem    switch-node.cmd 0001 backup     作用于 backup 备用实例
rem
rem  不带参数 = 打印用法及当前状态
rem ============================================================

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"

set "PROF=%~2"
if /i "%PROF%"=="backup" goto prof_ok
set "PROF=main"
:prof_ok

set "CLASH_PORT="
for /f "usebackq delims=" %%i in (`""%PY%" "%~dp0portof.py" clash "" %PROF%"`) do set "CLASH_PORT=%%i"
if "%CLASH_PORT%"=="" (
  echo [X] 找不到 %PROF% 实例的控制端口，请确认实例是否已运行。
  echo     启动命令: run-singbox.cmd %PROF%
  echo.
  pause
  exit /b 1
)
set "API=http://127.0.0.1:%CLASH_PORT%/proxies/node"

if /i "%~1"=="0709"  set NODE=qiniu-0709
if /i "%~1"=="0001"  set NODE=qiniu-0001
if /i "%~1"=="hy2"   set NODE=hy2-va1
if /i "%~1"=="vless" set NODE=vl-reality-va1

if "%NODE%"=="" (
  echo 用法: switch-node.cmd  0709 ^| 0001 ^| hy2 ^| vless   [main^|backup]
  echo.
  echo   0709  = 备用 0709 专线   ^(Cloudflare 穿透^)
  echo   0001  = 主用 0001 专线   ^(Cloudflare 穿透^)
  echo   hy2   = Hysteria 2 极速专线 ^(UDP/QUIC 抗丢包^)
  echo   vless = VLESS Reality 专线  ^(TCP 直连^)
  echo.
  echo 当前 %PROF% 实例状态 (控制接口 127.0.0.1:%CLASH_PORT%):
  curl -s "%API%"
  echo.
  exit /b 1
)

curl -s -X PUT "%API%" -H "Content-Type: application/json" -d "{\"name\":\"%NODE%\"}" >nul
if errorlevel 1 (
  echo [X] 切换失败，请确认 %PROF% 实例是否正在运行 (控制接口 http://127.0.0.1:%CLASH_PORT%)
  exit /b 1
)

echo 已成功切换 %PROF% 实例节点为: %NODE%
echo.
echo 当前状态:
curl -s "%API%"
echo.
