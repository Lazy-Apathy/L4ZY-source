# Download the official Steam Audio 4.8.1 release and copy phonon.dll to bin64\.
# phonon.dll is only loaded when 3D sound (son3d) is switched on in game.
# The headers in src\include\phonon come from the same release (Apache 2.0).
# The archive SHA-256 is the one recorded in distrib\docs\licenses\THIRD-PARTY-NOTICES.txt.
$ErrorActionPreference = "Stop"
$sha = "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449"
$url = "https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip"
$root = Split-Path -Parent $PSScriptRoot
$dl = Join-Path $PSScriptRoot "downloads"
New-Item -ItemType Directory -Force -Path $dl | Out-Null
$zip = Join-Path $dl "steamaudio_4.8.1.zip"
if (-not (Test-Path $zip)) {
    Write-Host "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
}
$got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($got -ne $sha) { throw "SHA-256 mismatch for $zip ($got)" }
$tmp = Join-Path $dl "steamaudio-extract"
if (-not (Test-Path $tmp)) { Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force }
$dll = Get-ChildItem $tmp -Recurse -Filter phonon.dll | Where-Object { $_.FullName -match 'windows-x64' } | Select-Object -First 1
if (-not $dll) { throw "phonon.dll (windows-x64) not found in the archive" }
$bin = Join-Path $root "bin64"
New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item $dll.FullName $bin -Force
Write-Host "OK: $bin\phonon.dll"
