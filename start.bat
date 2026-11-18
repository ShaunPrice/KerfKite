@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Install using scripts\install.ps1 on a Windows 10 or 11 x64 PC.
  exit /b 1
)
".venv\Scripts\python.exe" -B studio\windows_support.py
if errorlevel 1 exit /b %errorlevel%
".venv\Scripts\python.exe" -m studio.server %*
