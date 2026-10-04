$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $scriptDir

$buildDir = Join-Path $scriptDir "build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
}
Set-Location $buildDir

Write-Host "==> [1/2] Configuring with CMake & Ninja..."
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..

Write-Host "==> [2/2] Building targets..."
ninja

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "[OK] Build succeeded!" -ForegroundColor Green
Write-Host "Artifacts located in: E:\code\game-tcp-speed\bin" -ForegroundColor Green
Write-Host "  - bin\hook_proxy.dll"
Write-Host "  - bin\launcher_gui.exe"
Write-Host "============================================" -ForegroundColor Green
