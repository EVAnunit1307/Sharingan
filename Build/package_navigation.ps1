[CmdletBinding()]
param([string]$EngineDirectory='C:\Program Files\Epic Games\UE_5.7')
$ErrorActionPreference='Stop'
$navRoot=Split-Path -Parent $PSScriptRoot
$navProject=Join-Path $navRoot 'HandoffQuestHUD.uproject'
$navOutput=Join-Path $navRoot 'Saved\NavigationVerification\Package'
$navLog=Join-Path $navRoot 'Saved\NavigationVerification\FullPackage.log'
New-Item -ItemType Directory -Force $navOutput | Out-Null
# Deliberately invokes a full cook. The older native-only shortcut cannot stage
# MRUK scene configuration, new module dependencies, or the trail material.
& (Join-Path $EngineDirectory 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun `
    "-project=$navProject" -nop4 -utf8output -unattended -nocompileeditor `
    -platform=Android -cookflavor=ASTC -clientconfig=Development -build -cook `
    -stage -pak -iostore -package -archive "-archivedirectory=$navOutput" `
    '-map=/Engine/Maps/Entry' *> $navLog
if($LASTEXITCODE -ne 0){throw "Full Android cook/package failed. See $navLog"}
Get-ChildItem -LiteralPath $navOutput -Recurse -Filter '*.apk' | Get-FileHash -Algorithm SHA256
