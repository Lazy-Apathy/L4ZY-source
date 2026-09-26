# Clone NVIDIA NRD v4.17.3 (the version L4ZY ships) into third_party\NRD.
# NRD is licensed under the NVIDIA RTX SDKs licence: it is fetched here, never
# committed to this repository. Needs git. Read third_party\NRD\LICENSE.txt.
# Then build the gateway with src\nrd_gateway\build-dll.bat (MSVC + CMake,
# see third_party\fetch-msvc.ps1 and third_party\fetch-cmake.ps1).
$ErrorActionPreference = "Stop"
$dest = Join-Path $PSScriptRoot "NRD"
if (-not (Test-Path (Join-Path $dest "CMakeLists.txt"))) {
    Write-Host "Cloning NVIDIA-RTX/NRD v4.17.3"
    git clone --depth 1 --branch v4.17.3 --recursive https://github.com/NVIDIA-RTX/NRD.git $dest
    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
}
$tag = git -C $dest describe --tags 2>$null
Write-Host "NRD ready: $dest ($tag)"
if ($tag -ne "v4.17.3") { Write-Warning "Expected v4.17.3; the gateway was written against that version." }
