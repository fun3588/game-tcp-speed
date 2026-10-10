@echo off
chcp 65001 >nul
cd /d "%~dp0c"
start "" "%~dp0c\launcher_gui.exe"
