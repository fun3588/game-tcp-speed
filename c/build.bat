@echo off
rem ============================================================
rem  GameTcpSpeed v3.1.3 - C module build (CMake + Ninja)
rem  Outputs: GameTcpSpeed.exe / hook_proxy.dll in this folder
rem  Requires: cmake, ninja, g++ (MinGW-w64 x64) on PATH
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist "build" mkdir "build"
pushd "build"

rem NOTE: the '^>' below is mandatory - a bare '>' inside echo is a redirection
rem operator and cmd will try to create a file named "= [1/2] ..." instead.
echo ==^> [1/2] Configuring with CMake ^& Ninja...
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..
if errorlevel 1 goto :fail

echo ==^> [2/2] Building targets...
ninja
if errorlevel 1 goto :fail

popd
echo.
echo ============================================
echo [OK] Build succeeded!
echo Artifacts located in: %~dp0
echo   - GameTcpSpeed.exe
echo   - hook_proxy.dll
echo ============================================
exit /b 0

:fail
popd
echo.
echo [FAILED] Build aborted.
exit /b 1
