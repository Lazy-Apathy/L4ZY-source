$ErrorActionPreference = "Stop"
# AMD FidelityFX SDK v1.1.4 (FSR 3.1.4 upscaler, Vulkan backend), MIT licence.
# Pinned by tag AND commit: v1.1.4 = c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 (2025-05-08).
# The SDK 2.x line (FSR 4, Redstone) is DirectX 12 only; 1.1.4 is the last one
# shipping the Vulkan backend. Only sdk/, ffx-api/, docs/ and LICENSE.txt are
# checked out: no samples, no media, no prebuilt signed DLLs (they also contain
# frame generation, which this project never uses).
# Lot copy: shots\fsr-lot1\third_party. On integration this script moves to third_party\.
$tag = "v1.1.4"
$commit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"
$dest = if ($args.Count -gt 0) { $args[0] } else { Join-Path $PSScriptRoot "FidelityFX-SDK" }
if (-not (Test-Path (Join-Path $dest "sdk\include\FidelityFX\host\ffx_fsr3upscaler.h"))) {
    Write-Host "Cloning AMD FidelityFX SDK $tag (sparse: sdk, ffx-api, docs, LICENSE.txt)"
    git clone --depth 1 --branch $tag --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $dest
    if ($LASTEXITCODE) { throw "git clone failed" }
    git -C $dest sparse-checkout set --no-cone /sdk/ /ffx-api/ /docs/ /LICENSE.txt /readme.md
    if ($LASTEXITCODE) { throw "sparse-checkout failed" }
}
$head = (git -C $dest rev-parse HEAD).Trim()
if ($head -ne $commit) { throw "FidelityFX SDK HEAD $head != pinned $commit" }
# AMD's own FSR 3.1.5 fix "possible negative rcas output" (SDK v2.0.0, DirectX 12
# only) back-ported to the RCAS limiter in fsr1/ffx_fsr1.h: 3 functional lines per
# variant, identical to SDK v2.0.0 f4c1da8e Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h
# except the copyright year. Applied once, checked by hash (line endings ignored).
# SHA-256 of the text with CRLF turned into LF: git may check the file out either way.
function NormHash([string]$path) {
    $t = [System.IO.File]::ReadAllText($path) -replace "`r`n", "`n"
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($t)))).Replace('-', '') }
    finally { $sha.Dispose() }
}
$rcas = Join-Path $dest "sdk\include\FidelityFX\gpu\fsr1\ffx_fsr1.h"
$rcasOrig = "5D608D9D0645DA03A3AA6C729C5AD990C6F222053EAB850B8E218607C1BAAA9D"
$rcasPatched = "E9964CB8BF62CBEEF5F4B203D330ACBEB9178E204B09497C714DB86D22559A62"
$h = (NormHash $rcas)
if ($h -eq $rcasOrig) {
    git -C $dest apply --whitespace=nowarn (Join-Path $PSScriptRoot "patches\fsr-3.1.5-rcas-lower-limiter.patch")
    if ($LASTEXITCODE) { throw "RCAS patch failed" }
    $h = (NormHash $rcas)
}
if ($h -ne $rcasPatched) { throw "ffx_fsr1.h hash $h is neither the v1.1.4 original nor the patched file" }
$lic = Join-Path $dest "LICENSE.txt"
$hdr = Join-Path $dest "sdk\include\FidelityFX\host\ffx_fsr3upscaler.h"
foreach ($f in @($lic, $hdr)) { if (-not (Test-Path $f)) { throw "Missing $f" } }
Write-Host "FidelityFX SDK ready: $tag commit $head"
Get-FileHash -Algorithm SHA256 $lic, $hdr | ForEach-Object { "{0}  {1}" -f $_.Hash, $_.Path }
Write-Host "Gateway: src\fsr_gateway\build-dll.bat (portable MSVC, upscaler + Vulkan backend only)."
