@echo off
cd /d "%~dp0"
if not exist "build\native\Release\kerfkite-studio.exe" (
  echo Build first using scripts\install.ps1 on a Windows 10 or 11 x64 PC.
  exit /b 1
)
"build\native\Release\kerfkite-studio.exe" --static studio\static %*
