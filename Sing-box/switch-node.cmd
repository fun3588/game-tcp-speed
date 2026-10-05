@echo off
setlocal

rem ============================================================
rem  热切换 sing-box 当前节点（不断线）
rem
rem  用法:
rem    switch-node.cmd 0001            主实例切到 七牛0001
rem    switch-node.cmd 0709            主实例切到 七牛0709
rem    switch-node.cmd hy2             切到 Hysteria 2 直连
rem    switch-node.cmd vless           切到 VLESS Reality 直连
rem    switch-node.cmd 0001 backup     对备实例操作
rem
rem  不带参数 = 打印用法和当前状态
rem ============================================================

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"

set "PROF=%~2"
if /i "%PROF%"=="backup" goto prof_ok
set "PROF=main"
:prof_ok

rem 控制接口端口是随机的，从 runtime 文件取
rem 注意: usebackq 里命令两端的 "" 不能省 —— cmd 会剥掉最外层引号，
rem       只写 "%PY%" "..." 会变成裸路径导致 "文件名语法不正确"。
set "CLASH_PORT="
for /f "usebackq delims=" %%i in (`""%PY%" "%~dp0portof.py" clash "" %PROF%"`) do set "CLASH_PORT=%%i"
if "%CLASH_PORT%"=="" (
  echo [X] 读不到 %PROF% 实例的控制端口，它是不是没在运行？
  echo     启动: run-singbox.cmd %PROF%
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
  echo   0709  = 七牛 0709 新加坡  ^(Cloudflare 隧道^)
  echo   0001  = 七牛 0001 新加坡  ^(Cloudflare 隧道^)
  echo   hy2   = Hysteria 2 直连 66.154.126.18 ^(UDP/QUIC，抗丢包，实测最稳^)
  echo   vless = VLESS Reality 直连 66.154.126.18 ^(TCP，实测抖动偏大^)
  echo.
  echo 当前 %PROF% 实例状态（控制接口 127.0.0.1:%CLASH_PORT%）:
  curl -s "%API%"
  echo.
  exit /b 1
)

curl -s -X PUT "%API%" -H "Content-Type: application/json" -d "{\"name\":\"%NODE%\"}" >nul
if errorlevel 1 (
  echo [X] 调用失败，%PROF% 实例是否在运行？^(控制接口 http://127.0.0.1:%CLASH_PORT%^)
  exit /b 1
)

echo 已切换 %PROF% 实例到: %NODE%
echo.
echo 当前状态:
curl -s "%API%"
echo.
