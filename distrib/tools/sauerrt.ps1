# Point d'entree des outils de distribution sur un PC de developpement.
# Aucun Python a installer : telecharge le Python embarque epingle
# (pins.json, empreinte verifiee) dans distrib\.cache, puis lance l'outil.
#
#   powershell -ExecutionPolicy Bypass -File distrib\tools\sauerrt.ps1 fetch
#   powershell -ExecutionPolicy Bypass -File distrib\tools\sauerrt.ps1 release distrib\recipes\<version>.json --reuse <manifeste precedent>
#   powershell -ExecutionPolicy Bypass -File distrib\tools\sauerrt.ps1 publish --version <v> --channel test --target <dossier du site>
#   powershell -ExecutionPolicy Bypass -File distrib\tools\sauerrt.ps1 verify --url https://.../channels/test/latest.json
$ErrorActionPreference = 'Stop'
$Tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$Distrib = Split-Path -Parent $Tools
$Cache = Join-Path $Distrib '.cache'
$pins = Get-Content (Join-Path $Distrib 'pins.json') -Raw | ConvertFrom-Json
$zip = Join-Path $Cache $pins.python.file
$pyDir = Join-Path $Cache 'python-tools'
$py = Join-Path $pyDir 'python.exe'
if (-not (Test-Path $py)) {
    New-Item -ItemType Directory -Force $Cache | Out-Null
    if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $pins.python.sha256) {
        Write-Host "Telechargement de $($pins.python.url)"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing $pins.python.url -OutFile $zip
        if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $pins.python.sha256) { Remove-Item $zip; throw 'empreinte Python incorrecte' }
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $pyDir -Force
}
$cmd = $args[0]
switch ($cmd) {
    { $_ -in 'fetch', 'release', 'seed' } { & $py (Join-Path $Tools 'sauerrt_build.py') @args; exit $LASTEXITCODE }
    { $_ -in 'publish', 'publish-github', 'readme-github', 'verify' } { & $py (Join-Path $Tools 'sauerrt_publish.py') @args; exit $LASTEXITCODE }
    default { Write-Host 'commandes : fetch | release | publish | verify'; exit 2 }
}
