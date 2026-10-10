@echo off
chcp 65001 >nul
cd /d "%~dp0"
title Sing-box 核心
echo [*] 正在启动 Sing-box 专线核心...
sing-box.exe run -c config.json
