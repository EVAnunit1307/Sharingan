<#
.SYNOPSIS
Build the editor target and run all Wallhack.Spatial tests with offscreen GPU rendering.
.DESCRIPTION
No headset or visible Unreal window is required. Success is determined from the
fresh automation JSON report, not just the Unreal process exit code.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [ValidateRange(12, 10000)]
    [int]$MinimumExpectedTests = 20,
    [ValidateRange(1, 180)]
    [int]$TimeoutMinutes = 20,
    [string]$EngineDirectory = 'C:\Program Files\Epic Games\UE_5.7'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Join-NativeArguments([string[]]$Arguments) {
    # Start-Process joins its ArgumentList without preserving argument boundaries.
    # These arguments contain fixed commands and Windows paths (no literal quotes).
    return (($Arguments | ForEach-Object {
        if ($_ -match '["\r\n]') { throw 'A native argument contains an unsupported quote or newline.' }
        '"' + $_ + '"'
    }) -join ' ')
}

$trackingRoot = Split-Path -Parent $PSScriptRoot
$trackingProject = Join-Path $trackingRoot 'HandoffQuestHUD.uproject'
$trackingEngine = Join-Path $EngineDirectory 'Engine'
$trackingEditor = Join-Path $trackingEngine 'Binaries\Win64\UnrealEditor-Cmd.exe'
$trackingDotnet = Join-Path $trackingEngine 'Binaries\ThirdParty\DotNet\8.0.412\win-x64\dotnet.exe'
$trackingUBT = Join-Path $trackingEngine 'Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll'
$trackingLogs = Join-Path $trackingRoot 'Saved\Logs'
$trackingReports = Join-Path $trackingRoot 'Saved\TrackingVerification\LocalTests'
$trackingRun = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$trackingBuildLog = Join-Path $trackingLogs "LocalTrackingBuild-$trackingRun.log"
$trackingTestLog = Join-Path $trackingLogs "LocalTrackingTests-$trackingRun.log"
$trackingStdout = Join-Path $trackingLogs "LocalTrackingTests-$trackingRun-stdout.log"
$trackingStderr = Join-Path $trackingLogs "LocalTrackingTests-$trackingRun-stderr.log"
$trackingIndex = Join-Path $trackingReports 'index.json'
$trackingLock = $null
$trackingProcess = $null

try {
    foreach ($trackingRequired in @($trackingProject, $trackingEditor)) {
        if (-not (Test-Path -LiteralPath $trackingRequired -PathType Leaf)) {
            throw "Required file missing: $trackingRequired"
        }
    }
    $trackingExisting = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($trackingExisting.Count -gt 0) {
        throw ('Close existing Unreal editor/preview/test processes before running this isolated build and test command. PIDs: ' + (($trackingExisting | Select-Object -ExpandProperty Id) -join ', '))
    }
    New-Item -ItemType Directory -Path $trackingLogs, $trackingReports -Force | Out-Null
    $trackingLockPath = Join-Path (Split-Path -Parent $trackingReports) 'local-tracking-tests.lock'
    try {
        $trackingLock = [System.IO.File]::Open($trackingLockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }
    catch {
        throw 'Another local tracking test runner is active. Wait for it to finish before starting another run.'
    }

    if (-not $SkipBuild) {
        foreach ($trackingRequired in @($trackingDotnet, $trackingUBT)) {
            if (-not (Test-Path -LiteralPath $trackingRequired -PathType Leaf)) {
                throw "Required build tool missing: $trackingRequired"
            }
        }
        Write-Host "Building editor target. Log: $trackingBuildLog"
        & $trackingDotnet $trackingUBT HandoffQuestHUDEditor Win64 Development "-Project=$trackingProject" -NoHotReloadFromIDE *> $trackingBuildLog
        if ($LASTEXITCODE -ne 0) {
            throw "Editor compilation failed (exit $LASTEXITCODE). See $trackingBuildLog"
        }
    }

    # Preserve previous results but require a newly generated report for this run.
    if (Test-Path -LiteralPath $trackingIndex) {
        Rename-Item -LiteralPath $trackingIndex -NewName "index.previous-$trackingRun.json"
    }
    $trackingArguments = @(
        $trackingProject, '-unattended', '-nop4', '-RenderOffscreen', '-nocef',
        '-nosplash', '-nosound', '-nohmd',
        '-ExecCmds=Automation RunTests Wallhack.Spatial',
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$trackingReports", "-abslog=$trackingTestLog"
    )
    Write-Host "Running all Wallhack.Spatial tests with GPU rendering. Log: $trackingTestLog"
    $trackingStarted = [DateTime]::UtcNow
    $trackingProcess = Start-Process -FilePath $trackingEditor -ArgumentList (Join-NativeArguments $trackingArguments) `
        -WorkingDirectory $trackingRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $trackingStdout -RedirectStandardError $trackingStderr
    $trackingNextUpdate = [DateTime]::UtcNow.AddSeconds(30)
    while (-not $trackingProcess.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -gt $trackingStarted.AddMinutes($TimeoutMinutes)) {
            # Stop only the process this runner created; never enumerate and kill others.
            $trackingProcess.Kill()
            throw "Tests exceeded $TimeoutMinutes minutes. Inspect $trackingTestLog"
        }
        if ([DateTime]::UtcNow -ge $trackingNextUpdate) {
            Write-Host "Tests still running (PID $($trackingProcess.Id)); output remains in $trackingTestLog"
            $trackingNextUpdate = [DateTime]::UtcNow.AddSeconds(30)
        }
    }
    $trackingProcess.Refresh()
    if ($trackingProcess.ExitCode -ne 0) {
        throw "Unreal exited with code $($trackingProcess.ExitCode). See $trackingTestLog"
    }
    if (-not (Test-Path -LiteralPath $trackingIndex -PathType Leaf)) {
        throw "Unreal produced no fresh index.json report. See $trackingTestLog"
    }
    $trackingResult = Get-Content -LiteralPath $trackingIndex -Raw | ConvertFrom-Json
    foreach ($trackingField in @('succeeded', 'succeededWithWarnings', 'failed', 'notRun', 'inProcess', 'tests', 'devices')) {
        if ($null -eq $trackingResult.PSObject.Properties[$trackingField]) {
            throw "Automation report is missing '$trackingField': $trackingIndex"
        }
    }
    $trackingPassed = [int]$trackingResult.succeeded + [int]$trackingResult.succeededWithWarnings
    $trackingFailures = @($trackingResult.tests | Where-Object { $_.state -ne 'Success' })
    $trackingNames = @($trackingResult.tests | Select-Object -ExpandProperty fullTestPath -Unique)
    $trackingRequiredNames = @(
        'Wallhack.Spatial.Render.DotVisibilityAndParallax',
        'Wallhack.Spatial.Render.MinimapTracksActualViewer',
        'Wallhack.Spatial.Runtime.HUDPlacementAndVisibilityControls'
    )
    if ($MinimumExpectedTests -ge 13) {
        $trackingRequiredNames += 'Wallhack.Spatial.Runtime.DesktopKeyboardMovement'
    }
    if ($MinimumExpectedTests -ge 20) {
        $trackingRequiredNames += @(
            'Wallhack.Spatial.Runtime.MultipleContactsWorldMotion',
            'Wallhack.Spatial.Runtime.PausedMotionVersusStaleUpdates',
            'Wallhack.Spatial.Runtime.MultipleContactsLifecycle',
            'Wallhack.Spatial.Runtime.MultiContactHUDControls',
            'Wallhack.Spatial.Offscreen.CardinalDirections',
            'Wallhack.Spatial.Offscreen.ViewerPoseAndInvalidData',
            'Wallhack.Spatial.Render.MultipleContactsStaleAndRecovery'
        )
    }
    $trackingMissing = @($trackingRequiredNames | Where-Object { $_ -notin $trackingNames })
    $trackingRHIs = @($trackingResult.devices | ForEach-Object {
        if ($null -ne $_.PSObject.Properties['rHI']) { [string]$_.rHI }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and $_ -notmatch '(?i)null' })

    if ([int]$trackingResult.failed -gt 0 -or [int]$trackingResult.notRun -gt 0 -or
        [int]$trackingResult.inProcess -gt 0 -or $trackingPassed -lt $MinimumExpectedTests -or
        $trackingNames.Count -lt $MinimumExpectedTests -or $trackingFailures.Count -gt 0 -or
        $trackingMissing.Count -gt 0 -or $trackingRHIs.Count -eq 0) {
        foreach ($trackingFailure in $trackingFailures) {
            Write-Host "FAIL $($trackingFailure.fullTestPath): $($trackingFailure.state)"
        }
        if ($trackingMissing.Count -gt 0) { Write-Host "Missing required suites: $($trackingMissing -join ', ')" }
        if ($trackingRHIs.Count -eq 0) { Write-Host 'The report does not establish a non-null rendering backend.' }
        throw "Automation failed: $trackingPassed passed, $($trackingResult.failed) failed, $($trackingResult.notRun) not run; minimum $MinimumExpectedTests required. Report: $trackingIndex"
    }
    Write-Host "PASS: $trackingPassed tests; $($trackingResult.succeededWithWarnings) with warnings. RHI: $($trackingRHIs -join ', ')"
    Write-Host "Report: $trackingIndex"
    Write-Host 'Local tests do not establish headset stereo/passthrough visibility or physical room-anchor stability.'
}
catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
finally {
    if ($null -ne $trackingLock) { $trackingLock.Dispose() }
    if ($null -ne $trackingProcess) { $trackingProcess.Dispose() }
}
exit 0
