# Windows source installation. Does not configure services, drivers or hardware.
param([switch]$Web, [switch]$Mcp, [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
& $Python -c "import sys; sys.exit('Python 3.10 or newer is required') if sys.version_info < (3,10) else None"
if ($LASTEXITCODE -ne 0) { throw 'Python version check failed. Install 64-bit Python 3.12 and pass -Python with its executable path.' }
$CncVenvPython = Join-Path (Get-Location) '.venv\Scripts\python.exe'
if (-not (Test-Path $CncVenvPython)) {
    & $Python -m venv .venv
    if ($LASTEXITCODE -ne 0) { throw 'Virtual environment creation failed.' }
}
& $CncVenvPython -c "import sys; sys.exit('Existing .venv requires Python 3.10 or newer') if sys.version_info < (3,10) else None"
if ($LASTEXITCODE -ne 0) { throw 'Existing virtual environment uses an unsupported Python.' }
$CncExtras = @()
if ($Web) { $CncExtras += 'web' }
if ($Mcp) { $CncExtras += 'mcp' }
$CncPackage = '.'
if ($CncExtras.Count -gt 0) { $CncPackage = '.[' + ($CncExtras -join ',') + ']' }
& $CncVenvPython -m pip install $CncPackage
if ($LASTEXITCODE -ne 0) { throw 'Package installation failed.' }
& $CncVenvPython -c "from studio.relief import _relief_native; print('Relief engine:', 'C++ available' if _relief_native is not None else 'Python fallback (optional C++ compiler not available)')"
if ($LASTEXITCODE -ne 0) { throw 'Installed package import failed.' }
Write-Output 'Installed. Start simulator: .\start.bat --port 8765'
Write-Output 'Then open http://127.0.0.1:8765'
