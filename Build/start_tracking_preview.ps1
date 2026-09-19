<#
.SYNOPSIS
Open the interactive, visible desktop tracking preview without a headset.
#>
[CmdletBinding()]
param(
    [string]$EngineDirectory = 'C:\Program Files\Epic Games\UE_5.7'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$trackingRoot = Split-Path -Parent $PSScriptRoot
$trackingProject = Join-Path $trackingRoot 'HandoffQuestHUD.uproject'
$trackingEditor = Join-Path $EngineDirectory 'Engine\Binaries\Win64\UnrealEditor.exe'
$trackingModule = Join-Path $trackingRoot 'Binaries\Win64\UnrealEditor-HandoffQuestHUD.dll'
$trackingLogs = Join-Path $trackingRoot 'Saved\Logs'
$trackingLog = Join-Path $trackingLogs ('TrackingPreview-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log')

try {
    foreach ($trackingRequired in @($trackingProject, $trackingEditor, $trackingModule)) {
        if (-not (Test-Path -LiteralPath $trackingRequired -PathType Leaf)) {
            throw "Required file missing: $trackingRequired. Build first with Build\run_local_tracking_tests.ps1."
        }
    }
    $trackingExisting = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($trackingExisting.Count -gt 0) {
        throw ('Close existing Unreal editor/preview/test processes first. PIDs: ' + (($trackingExisting | Select-Object -ExpandProperty Id) -join ', '))
    }
    New-Item -ItemType Directory -Path $trackingLogs -Force | Out-Null
    $trackingArguments = @(
        $trackingProject, '-game', '-WallhackTrackingPreview', '-nohmd',
        '-windowed', '-ResX=1280', '-ResY=720', '-nocef', '-nosplash', '-nosound',
        "-abslog=$trackingLog"
    )
    $trackingCommandLine = ($trackingArguments | ForEach-Object {
        if ($_ -match '["\r\n]') { throw 'A native argument contains an unsupported quote or newline.' }
        '"' + $_ + '"'
    }) -join ' '
    # This launcher explicitly opens a visible interactive window for its user.
    $trackingPreview = Start-Process -FilePath $trackingEditor -ArgumentList $trackingCommandLine `
        -WorkingDirectory $trackingRoot -WindowStyle Normal -PassThru
    Write-Host "Started desktop tracking preview (PID $($trackingPreview.Id)). Log: $trackingLog"
    Write-Host 'W/S forward/back; A/D strafe; Q/E down/up; arrow keys look; Space place; B HUD mode; X compass zero.'
    Write-Host 'Click the preview to focus keyboard input. Close its window or press Alt+F4 to exit.'
    Write-Host 'This uses the real dot/HUD rendering with a desktop viewer; Meta spatial anchors and passthrough require headset verification.'
}
catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
