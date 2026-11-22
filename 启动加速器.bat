@echo off
rem Prefer packaged dist (bin\v3.1) which bundles the Sing-box core; fall back to dev build dir (c)
setlocal
set "DIST=%~dp0bin\v3.1"
set "DEV=%~dp0c"
if exist "%DIST%\GameTcpSpeed.exe" (
    cd /d "%DIST%"
    start "" GameTcpSpeed.exe
    goto :eof
)
if exist "%DEV%\GameTcpSpeed.exe" (
    cd /d "%DEV%"
    start "" GameTcpSpeed.exe
    goto :eof
)
echo [ERROR] GameTcpSpeed.exe not found. Run build_all.bat first.
pause
