@echo off
rem ============================================================
rem  GameTcpSpeed v3.1.5 - Build & Package Script (Pure Win32 GDI)
rem  1) Builds the C module via c\build.bat
rem  2) Packages artifacts into bin\v3.1 (self-contained)
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "DIST=%ROOT%bin\v3.1"

echo ============================================================
echo   GameTcpSpeed v3.1.5 - Build ^& Package Script (Pure Win32 GDI)
echo ============================================================

echo.
echo [1/2] Building Pure Win32 targets...
call "%ROOT%c\build.bat"
if errorlevel 1 (
    echo [FAILED] Compile step failed, packaging aborted.
    exit /b 1
)

echo.
echo [2/2] Packaging to bin\v3.1...
if not exist "%DIST%" mkdir "%DIST%"

call :CopyWithBackup "%ROOT%c\GameTcpSpeed.exe" "%DIST%\GameTcpSpeed.exe"
if errorlevel 1 goto :fail
call :CopyWithBackup "%ROOT%c\hook_proxy.dll" "%DIST%\hook_proxy.dll"
if errorlevel 1 goto :fail

rem Copy Sing-box payload by content: xcopy merges into the existing folder,
rem so it never produces the nested Sing-box\Sing-box directory.
xcopy "%ROOT%Sing-box\*" "%DIST%\Sing-box\" /E /I /Y /Q >nul
if errorlevel 1 (
    echo [FAILED] Copying Sing-box payload failed.
    goto :fail
)

rem Remove a stale legacy GUI binary if an old layout left one behind.
if exist "%DIST%\launcher_gui.exe" del /Q "%DIST%\launcher_gui.exe"

echo.
echo ============================================================
echo   [OK] v3.1.5 Pure Win32 Build ^& Packaging Succeeded!
echo   Output Directory: %DIST%
echo   - GameTcpSpeed.exe (Pure Win32 API + GDI, ~336KB, 3~5MB RAM, Bilingual)
echo   - hook_proxy.dll   (Minimal pure hook + module pinning + drain barrier)
echo   - Sing-box/        (Proxy core ^& config)
echo ============================================================
exit /b 0

:fail
echo.
echo [FAILED] Packaging aborted.
exit /b 1

rem ------------------------------------------------------------
rem :CopyWithBackup <src> <dst>
rem Copy src over dst; if dst is locked by a running process,
rem move it aside as <dst>.old and retry once.
rem ------------------------------------------------------------
:CopyWithBackup
if not exist "%~1" (
    echo   [FAILED] Source missing: %~1
    exit /b 1
)
copy /Y "%~1" "%~2" >nul 2>&1
if not errorlevel 1 (
    echo   [OK] %~nx2
    exit /b 0
)
echo   [!] %~nx2 is locked, moving aside and retrying...
if exist "%~2.old" del /Q "%~2.old" >nul 2>&1
move /Y "%~2" "%~2.old" >nul 2>&1
copy /Y "%~1" "%~2" >nul 2>&1
if errorlevel 1 (
    echo   [FAILED] Cannot replace %~nx2
    exit /b 1
)
echo   [OK] %~nx2
exit /b 0
