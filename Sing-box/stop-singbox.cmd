@echo off
setlocal

rem 用法:
rem   stop-singbox.cmd          停主实例（main）
rem   stop-singbox.cmd backup   停备实例
rem   stop-singbox.cmd all      两个都停

set "PY=C:\Users\junjun\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"

set "PROF=main"
set "ARG=%~1"
if /i "%ARG%"=="backup" set "PROF=backup"
if /i "%ARG%"=="all" goto stop_all

"%PY%" "%~dp0stop_singbox.py" --profile %PROF%
goto done

:stop_all
"%PY%" "%~dp0stop_singbox.py" --all

:done
echo.
pause
