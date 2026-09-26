$ErrorActionPreference = "Stop"
$dest = $PSScriptRoot
$extract = Join-Path $dest "NVIDIA-DLSS"
if (-not (Test-Path (Join-Path $extract "include\nvsdk_ngx_vk.h"))) {
    Write-Host "Cloning official NVIDIA/DLSS SDK v310.9.1"
    git clone --depth 1 --branch v310.9.1 https://github.com/NVIDIA/DLSS.git $extract
}
$lib = Join-Path $extract "lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
$dll = Join-Path $extract "lib\Windows_x86_64\rel\nvngx_dlss.dll"
if (-not (Test-Path $lib)) { throw "Missing $lib (git clone NVIDIA/DLSS; binaries are in-tree, not LFS)" }
if (-not (Test-Path $dll)) { throw "Missing $dll" }
Write-Host "NGX SDK ready (NVIDIA DLSS 310.9.1, NVSDK_NGX_Version_API 0x15):"
Get-Item $lib, $dll, (Join-Path $extract "lib\Windows_x86_64\x64\nvsdk_ngx_d.lib") | ForEach-Object { "{0}  {1} bytes" -f $_.FullName, $_.Length }
Write-Host "Gateway DLL: nvsdk_ngx_s.lib + MSVC /MT."
Write-Host "nvsdk_ngx_d.lib only if the DLL is built with /MD."
Write-Host "Do not use vs2010/vs2012/vs2013 or khr trees."
Write-Host "Runtime nvngx.dll comes from the NVIDIA driver."
