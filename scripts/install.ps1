# Builds Studio's programs from this checkout into build\native and runs their self-tests. Does not configure
# services, drivers or hardware. Needs Visual Studio 2022 with the C++ workload (its CMake is used); with Go
# installed it also builds the TLS relay (HTTPS pairing, hosted transfers).
param([switch]$Web, [switch]$Mcp)  # the hosted editor and the assistant server are always built
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake was not found. Run this from a Developer PowerShell for Visual Studio 2022, or install CMake 3.16 or newer.'
}
cmake -S native -B build\native -A x64
if ($LASTEXITCODE -ne 0) { throw 'Configuring the build failed.' }
cmake --build build\native --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'The build failed.' }
ctest --test-dir build\native -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'The self-tests failed.' }
Write-Output 'Built. Start the simulator: .\start.bat --port 8765'
Write-Output 'Then open http://127.0.0.1:8765'
