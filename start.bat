@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Install first: py -3.12 -m venv .venv
  echo Then: .venv\Scripts\python.exe -m pip install .
  exit /b 1
)
".venv\Scripts\python.exe" -m studio.server %*
