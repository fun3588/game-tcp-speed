@echo off
setlocal
title Firestorm Accel - GUI

rem ============================================================
rem  加速器 图形面板（原生窗口 / tkinter）
rem
rem  用法:
rem    run-gui.cmd              起面板（会请求管理员权限）
rem    run-gui.cmd no-elevate   不提权。sing-box 启停和节点管理照常用；
rem                             加速器启停/切换上游会失败，面板里会再问你要不要提权。
rem
rem  为什么要提权: 加速器要挂/摘副 IP 141.95.207.63 并改路由，没有管理员
rem  权限做不了。先提权一次，面板里所有按钮就都能用（和 firestorm_tray.cmd 一致）。
rem
rem  为什么不用项目里的托管 Python:
rem  tkinter 是 CPython 编译期模块，没有 pip 包可装 —— 托管那个精简构建
rem  没编译它。所以这里专门去挑一个"带 tkinter 的" Python，挑不到就给提示。
rem
rem  关掉窗口 = 关掉面板。sing-box 实例和加速器都不受影响。
rem ============================================================

set "ARGS=%*"

rem 参数里带 no-elevate 就跳过提权（调试用）
set "SKIP="
if not defined ARGS goto pick
echo %ARGS% | findstr /i "no-elevate" >nul
if not errorlevel 1 set "SKIP=1"

rem ---- 挑一个带 tkinter 的 Python（用 pythonw 起，不留黑框）----
:pick
set "GUIDIR=%~dp0"
set "PYW="
set "PY="
call :try "D:\py3-12-1\pythonw.exe"
if not defined PYW if defined LOCALAPPDATA call :try "%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe"
if not defined PYW if defined LOCALAPPDATA call :try "%LOCALAPPDATA%\Programs\Python\Python313\pythonw.exe"
if not defined PYW call :try "%ProgramFiles%\Python312\pythonw.exe"
if not defined PYW call :try "%ProgramFiles%\Python313\pythonw.exe"

if not defined PYW (
    echo [X] 没找到带 tkinter 的 Python。
    echo     本面板需要 tkinter（标准库里的窗口库，没有 pip 包可装）。
    echo     请到 python.org 装一个官方 CPython 后重试。
    pause
    exit /b 1
)

rem ---- 提权 ----
if defined SKIP goto run
rem 沿用 py 下那几个脚本的写法（net session），保持一致。
rem 它依赖 LanmanServer 服务；若该系统把服务停了，会出现
rem 「提权后仍判定未提权、反复弹 UAC」，那时换成:
rem     whoami /groups | findstr /C:S-1-16-12288 >nul 2>&1
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights... please click YES on the UAC dialog.
    if defined ARGS (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%ARGS%' -Verb RunAs"
    ) else (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    )
    exit /b
)

:run
set "SCR=%GUIDIR%firestorm_gui.py"
if not exist "%SCR%" (
    echo [X] 找不到 %SCR%
    pause
    exit /b 1
)

rem ---- 起飞前自检：把模块 import 一遍（不建窗口），错了当场显示 ----
"%PY%" -c "import os,sys; sys.path.insert(0, os.environ['GUIDIR']); import firestorm_gui" 2>&1
if errorlevel 1 (
    echo.
    echo [X] 面板自检失败，上面是错误信息。
    pause
    exit /b 1
)

start "" "%PYW%" "%SCR%" %ARGS%
exit /b 0

:try
rem %~1 = 候选 pythonw.exe；同目录的 python.exe 用来探测 tkinter
set "CAND=%~1"
if not exist "%CAND%" exit /b
set "T=%CAND:pythonw.exe=python.exe%"
if not exist "%T%" set "T=%CAND%"
"%T%" -c "import tkinter" >nul 2>&1
if errorlevel 1 exit /b
set "PYW=%CAND%"
set "PY=%T%"
exit /b
