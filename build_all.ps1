# GameTcpSpeed v3.0 一键编译发布脚本 (纯经典 Win32 API + GDI 双语版)
$ErrorActionPreference = "Stop"
$rootDir = $PSScriptRoot

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  GameTcpSpeed v3.0 - Build & Package Script (Pure Win32 GDI)  " -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. 编译纯 Win32 模块
Write-Host "`n[1/2] Building Pure Win32 targets..." -ForegroundColor Yellow
$cBuildScript = Join-Path $rootDir "c\build.ps1"
powershell -ExecutionPolicy Bypass -File $cBuildScript

# 2. 组装至 bin/v3.0
Write-Host "`n[2/2] Packaging to bin/v3.0..." -ForegroundColor Yellow
$distDir = Join-Path $rootDir "bin\v3.0"
if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
}

$srcExe = Join-Path $rootDir "c\GameTcpSpeed.exe"
$dstExe = Join-Path $distDir "GameTcpSpeed.exe"
try {
    Copy-Item $srcExe $dstExe -Force
} catch {
    Rename-Item $dstExe "GameTcpSpeed.exe.old" -Force -ErrorAction SilentlyContinue
    Copy-Item $srcExe $dstExe -Force
}

$srcDll = Join-Path $rootDir "c\hook_proxy.dll"
$dstDll = Join-Path $distDir "hook_proxy.dll"
try {
    Copy-Item $srcDll $dstDll -Force
} catch {
    Rename-Item $dstDll "hook_proxy.dll.old" -Force -ErrorAction SilentlyContinue
    Copy-Item $srcDll $dstDll -Force
}

$srcSb = Join-Path $rootDir "Sing-box"
$dstSb = Join-Path $distDir "Sing-box"
Copy-Item -Recurse $srcSb $dstSb -Force

# 清理 bin/v3.0 中之前可能遗留的旧版 exe
$oldGui = Join-Path $distDir "launcher_gui.exe"
if (Test-Path $oldGui) { Remove-Item $oldGui -Force -ErrorAction SilentlyContinue }

Write-Host "`n============================================================" -ForegroundColor Green
Write-Host "  [OK] v3.0 Pure Win32 Build & Packaging Succeeded!" -ForegroundColor Green
Write-Host "  Output Directory: $distDir" -ForegroundColor Green
Write-Host "  - GameTcpSpeed.exe (Pure Win32 API + GDI, ~175KB, 3~5MB RAM, Bilingual)"
Write-Host "  - hook_proxy.dll   (Winsock hook with ConnectEx & unhook support)"
Write-Host "  - Sing-box/        (Proxy core & config)"
Write-Host "============================================================" -ForegroundColor Green
