# Download a portable MSVC x64 toolchain + Windows SDK into third_party\msvc.
# Not a system Visual Studio install. Does not change git config.
# Source: mmozeiko portable-msvc.py (same method as PortableBuildTools).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $here "portable-msvc.py"
$url = "https://gist.githubusercontent.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977/raw/portable-msvc.py"
if (-not (Test-Path $py)) {
    Write-Host "Fetching portable-msvc.py"
    Invoke-WebRequest -Uri $url -OutFile $py -UseBasicParsing
}
$python = Get-Command py -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python) { throw "Python is required to fetch MSVC. py.exe / python.exe not found." }
Push-Location $here
try {
    & $python.Source $py --show-versions
    Write-Host "Installing latest MSVC + Windows SDK for host/target x64 into third_party\msvc"
    & $python.Source $py --accept-license --vs 2022 --host x64 --target x64
    if ($LASTEXITCODE -ne 0) { throw "portable-msvc.py failed with $LASTEXITCODE" }
    if (-not (Test-Path (Join-Path $here "msvc\setup_x64.bat"))) {
        throw "setup_x64.bat missing after install"
    }
    Write-Host "OK: third_party\msvc\setup_x64.bat"
} finally {
    Pop-Location
}
