# Download a portable CMake 3.31.6 (Windows x64) into third_party\cmake.
# Only needed to build NVIDIA NRD for the NRD gateway (src\nrd_gateway\build-nrd-lib.bat).
# Not a system install; delete the folder to remove it.
$ErrorActionPreference = "Stop"
$ver = "3.31.6"
$dest = Join-Path $PSScriptRoot "cmake"
if (Test-Path (Join-Path $dest "bin\cmake.exe")) { Write-Host "CMake already in $dest"; exit 0 }
$dl = Join-Path $PSScriptRoot "downloads"
New-Item -ItemType Directory -Force -Path $dl | Out-Null
$zip = Join-Path $dl "cmake-$ver-windows-x86_64.zip"
$url = "https://github.com/Kitware/CMake/releases/download/v$ver/cmake-$ver-windows-x86_64.zip"
if (-not (Test-Path $zip)) {
    Write-Host "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
}
$tmp = Join-Path $dl "cmake-extract"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
Move-Item $inner.FullName $dest
Remove-Item -Recurse -Force $tmp
& (Join-Path $dest "bin\cmake.exe") --version
