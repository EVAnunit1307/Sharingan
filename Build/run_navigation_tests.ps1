[CmdletBinding()]
param([switch]$SkipBuild,[string]$EngineDirectory='C:\Program Files\Epic Games\UE_5.7')
$ErrorActionPreference='Stop'
$navRoot=Split-Path -Parent $PSScriptRoot
$navProject=Join-Path $navRoot 'HandoffQuestHUD.uproject'
$navEvidence=Join-Path $navRoot ('Saved\NavigationVerification\TestRuns\'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $navEvidence | Out-Null
if(-not $SkipBuild){
    & (Join-Path $EngineDirectory 'Engine\Binaries\ThirdParty\DotNet\8.0.412\win-x64\dotnet.exe') `
        (Join-Path $EngineDirectory 'Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll') `
        HandoffQuestHUDEditor Win64 Development "-Project=$navProject" -NoHotReloadFromIDE '-ModuleWithSuffix=HandoffQuestHUD,0919' "-Log=$navEvidence\Build.log"
    if($LASTEXITCODE -ne 0){throw 'Editor build failed'}
}
& (Join-Path $EngineDirectory 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $navProject `
    -unattended -nop4 -RenderOffscreen -nocef -nosplash -nosound -nohmd `
    '-ExecCmds=Automation RunTests Wallhack.' '-TestExit=Automation Test Queue Empty' `
    "-ReportExportPath=$navEvidence\Report" "-abslog=$navEvidence\Tests.log" *> "$navEvidence\stdout.log"
$navReport=Get-Content -LiteralPath "$navEvidence\Report\index.json" -Raw | ConvertFrom-Json
if($LASTEXITCODE -ne 0 -or $navReport.failed -gt 0 -or $navReport.tests.Count -lt 79 -or @($navReport.tests | Where-Object state -ne 'Success').Count){throw "Tests failed or incomplete: $navEvidence"}
$navReport | Select-Object succeeded,succeededWithWarnings,failed
Write-Host "Reports: $navEvidence"
