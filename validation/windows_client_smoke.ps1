# Run on a Windows 10/11 x64 client with Python, 7-Zip and the .NET SDK.
# Exercises release files in a temporary directory without installing Start menu or registry entries.
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Sha256,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
if ($Sha256 -notmatch '^[0-9a-fA-F]{64}$' -or (Get-FileHash $Installer -Algorithm SHA256).Hash -ne $Sha256) {
    throw 'The installer does not match the release checksum.'
}
$hostOS = Get-CimInstance Win32_OperatingSystem
if ($hostOS.ProductType -ne 1) { throw 'This test requires a Windows client.' }
foreach ($port in 18764,18765) {
    if (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue) {
        throw "Port $port is already in use. Leave the existing workshop running."
    }
}
$archiveTool = Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $archiveTool) { $archiveTool = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
if (-not (Test-Path $archiveTool)) { throw '7-Zip is required to exercise the package without installing it.' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('kerfkite-client-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stage | Out-Null
$previousCache = $env:KERFKITE_NATIVE_BUILD_CACHE
try {
    $env:KERFKITE_NATIVE_BUILD_CACHE = Join-Path $stage 'build-cache'
    $setup = Start-Process $Installer -ArgumentList '/S /CHECKPLATFORM' -PassThru -Wait
    if ($setup.ExitCode -ne 0) { throw "The installer's client check returned $($setup.ExitCode)." }
    $payload = Join-Path $stage 'payload'
    & $archiveTool x -y "-o$payload" $Installer | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the signed-off release payload.' }
    foreach ($name in 'KerfKite.exe','KerfKiteNative.exe') {
        $check = Start-Process (Join-Path $payload $name) -ArgumentList '--check-windows' -PassThru -Wait
        if ($check.ExitCode -ne 0) { throw "$name rejected the Windows client." }
    }
    Write-Output ("Accepted {0}, version {1}" -f $hostOS.Caption,$hostOS.Version)
    & $Python (Join-Path $PSScriptRoot 'installer_smoke.py') (Join-Path $payload 'KerfKite.exe') --step --native
    if ($LASTEXITCODE -ne 0) { throw 'Windows client simulation failed.' }
} finally {
    $env:KERFKITE_NATIVE_BUILD_CACHE = $previousCache
    Remove-Item -LiteralPath $stage -Recurse -Force
}
