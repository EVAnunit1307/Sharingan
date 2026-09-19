<#
.SYNOPSIS
Build Android native code and package the existing Green Test workspace offline.
.DESCRIPTION
Reuses already cooked assets/configuration. Refuses known stale cook inputs,
verifies the candidate APK and signing certificate, then backs up/replaces the
final APK. Does not cook, download, install, or contact a headset.
#>
[CmdletBinding()]
param(
    [string]$JavaHome = 'C:\Users\kevin\AppData\Local\Programs\Quest2Dev\Java21\jdk-21.0.12.1+1',
    [string]$Python = 'C:\Python314\python.exe',
    [string]$CookProvenance = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$viewerRoot = Split-Path -Parent $PSScriptRoot
$viewerProject = Join-Path $viewerRoot 'HandoffQuestHUD.uproject'
$viewerEngine = 'C:\Program Files\Epic Games\UE_5.7\Engine'
$viewerDotnet = Join-Path $viewerEngine 'Binaries\ThirdParty\DotNet\8.0.412\win-x64\dotnet.exe'
$viewerUBT = Join-Path $viewerEngine 'Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll'
$viewerSDK = 'C:\Users\kevin\AppData\Local\Android\Sdk'
$viewerSetup = 'C:\Users\kevin\Downloads\Quest2Setup'
$viewerGradle = Join-Path $viewerSetup 'GreenTestGradle'
# Invoke the already extracted distribution directly. A wrapper can download
# its distribution before Gradle sees --offline, so it is not used here.
$viewerGradleCommand = 'C:\Users\kevin\.gradle\wrapper\dists\gradle-8.7-all\aan3ydargesu18aqyqjwhr3pc\gradle-8.7\bin\gradle.bat'
$viewerFinal = Join-Path $viewerSetup 'WallhackSpatialTracking.apk'
$viewerNative = Join-Path $viewerRoot 'Binaries\Android\HandoffQuestHUD-arm64.so'
$viewerGradleNative = Join-Path $viewerGradle 'app\src\main\jniLibs\arm64-v8a\libUnreal.so'
$viewerStripped = Join-Path $viewerGradle 'app\build\intermediates\stripped_native_libs\debug\stripDebugDebugSymbols\out\lib\arm64-v8a\libUnreal.so'
$viewerManifest = Join-Path $viewerGradle 'app\src\main\AndroidManifest.xml'
$viewerOBB = Join-Path $viewerGradle 'app\src\main\assets\main.obb.png'
$viewerVerifier = Join-Path $PSScriptRoot 'verify_tracking_package.py'
$viewerJava = Join-Path $JavaHome 'bin\java.exe'
$viewerSigner = Join-Path $viewerSDK 'build-tools\35.0.1\lib\apksigner.jar'
$viewerBaselineCertificate = '76848D5B41ACA68828D8C4B0C51580AE04B6BBCDFD42FD7FE11A583C20CE0371'
$viewerStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$viewerLogs = Join-Path $viewerRoot 'Saved\Logs'
$viewerRun = Join-Path $viewerRoot "Saved\TrackingVerification\PackageRuns\$viewerStamp"
$viewerCandidate = Join-Path $viewerRun 'WallhackSpatialTracking.candidate.apk'
$viewerBuildLog = Join-Path $viewerLogs "SpatialViewerNative-$viewerStamp.log"
$viewerGradleLog = Join-Path $viewerLogs "SpatialViewerGradle-$viewerStamp.log"
$viewerLock = $null
$viewerSavedEnvironment = @{}
if ([string]::IsNullOrWhiteSpace($CookProvenance)) {
    $CookProvenance = Join-Path $viewerRoot 'Saved\TrackingVerification\final-artifact-provenance.json'
}

# Compare known cook inputs with verified provenance and the actual existing OBB.
# Source files are snapshotted separately so concurrent changes cannot silently
# produce a package attributed to a different workspace state.
$viewerInputCheck = @'
import hashlib,json,sys
from pathlib import Path
from zipfile import ZipFile
root,gradle,provenance=map(Path,sys.argv[1:])
def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest().upper()
baseline=json.loads(provenance.read_text(encoding='utf-8-sig'))['source_config_material_sha256']
cook_inputs=sorted((root/'Config').rglob('*.ini'))+[root/'HandoffQuestHUD.uproject']
cook_inputs+=sorted((root/'Source').rglob('*.Build.cs'))+sorted((root/'Source').rglob('*.Target.cs'))
for name in baseline:
    if name.startswith('Config/') and not (root/name).is_file():
        raise SystemExit('Full cook/package required: removed configuration '+name)
for path in cook_inputs:
    name=path.relative_to(root).as_posix()
    if baseline.get(name)!=digest(path):
        raise SystemExit('Full cook/package required: changed/new cook or module input '+name)
obb=gradle/'app/src/main/assets/main.obb.png'
with ZipFile(obb) as archive:
    for path in sorted((root/'Content').rglob('*')):
        if not path.is_file():continue
        name='HandoffQuestHUD/Content/'+path.relative_to(root/'Content').as_posix()
        if name not in archive.namelist():
            raise SystemExit('Full cook/package required: content missing from current OBB: '+name)
        with archive.open(name) as f: packaged=hashlib.file_digest(f,'sha256').hexdigest().upper()
        if packaged!=digest(path):
            raise SystemExit('Full cook/package required: content changed: '+name)
sources=sorted(p for p in (root/'Source').rglob('*') if p.is_file() and p.suffix in {'.cpp','.h','.cs'} and 'graphify-out' not in p.parts)
sources+=sorted((root/'Config').rglob('*.ini'))+[root/'HandoffQuestHUD.uproject']
sources+=sorted(p for p in (root/'Content').rglob('*') if p.is_file())
snapshot={p.relative_to(root).as_posix():digest(p) for p in sources}
print(json.dumps({'inputs_sha256':snapshot,'snapshot_sha256':hashlib.sha256(json.dumps(snapshot,sort_keys=True).encode()).hexdigest().upper(),'manifest_sha256':digest(gradle/'app/src/main/AndroidManifest.xml'),'obb_sha256':digest(obb)}))
'@

function Read-ViewerInputs {
    $viewerOutput = & $Python -c $viewerInputCheck $viewerRoot $viewerGradle $CookProvenance
    if ($LASTEXITCODE -ne 0) {
        throw 'Cook-input verification failed. Recook/package and refresh the Green Test manifest/assets plus cook provenance before native-only packaging.'
    }
    return ($viewerOutput | ConvertFrom-Json)
}

try {
    foreach ($viewerRequired in @($viewerProject, $viewerDotnet, $viewerUBT, $viewerGradleCommand, $viewerFinal,
        $viewerVerifier, $viewerJava, $viewerSigner, $Python, $viewerManifest, $viewerOBB, $CookProvenance)) {
        if (-not (Test-Path -LiteralPath $viewerRequired -PathType Leaf)) {
            throw "Required existing tool/artifact missing or inaccessible: $viewerRequired. This command does not download dependencies."
        }
    }
    $viewerEditors = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd' -ErrorAction SilentlyContinue)
    if ($viewerEditors.Count -gt 0) {
        throw 'Close the local Unreal editor/preview/test process before packaging.'
    }
    $viewerBuildText = Get-Content -LiteralPath (Join-Path $viewerGradle 'app\build.gradle') -Raw
    if (-not $viewerBuildText.Contains("applicationId 'com.wallhack.questhud.greentest'")) {
        throw 'GreenTestGradle does not declare the expected greentest applicationId. Refusing to assemble another package.'
    }
    $viewerProperties = Get-Content -LiteralPath (Join-Path $viewerGradle 'gradle.properties') -Raw
    if ($viewerProperties -notmatch '(?m)^PACKAGE_NAME=com\.wallhack\.questhud\s*$') {
        throw 'Preserve the existing com.wallhack.questhud Java namespace in GreenTestGradle.'
    }
    [xml]$viewerXML = Get-Content -LiteralPath $viewerManifest -Raw
    $viewerAlpha = @($viewerXML.manifest.application.'meta-data' | Where-Object {
        $_.GetAttribute('name', 'http://schemas.android.com/apk/res/android') -eq 'com.epicgames.unreal.GameActivity.PropagateAlpha'
    })
    if ($viewerAlpha.Count -ne 1 -or $viewerAlpha[0].GetAttribute('value', 'http://schemas.android.com/apk/res/android') -ne '1') {
        throw 'Existing Green Test manifest must have PropagateAlpha=1. Refresh it from corrected Unreal packaging first.'
    }
    New-Item -ItemType Directory -Path $viewerLogs, $viewerRun -Force | Out-Null
    $viewerLockPath = Join-Path $viewerRoot 'Saved\TrackingVerification\spatial-viewer-package.lock'
    try {
        $viewerLock = [System.IO.File]::Open($viewerLockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }
    catch { throw 'Another spatial viewer packager is active; wait for it to finish.' }
    $viewerBefore = Read-ViewerInputs
    $viewerOldHash = (Get-FileHash -LiteralPath $viewerFinal -Algorithm SHA256).Hash
    & $Python $viewerVerifier --apk $viewerFinal --report (Join-Path $viewerRun 'baseline-package-verification.json') *> (Join-Path $viewerLogs "SpatialViewerBaseline-$viewerStamp.log")
    if ($LASTEXITCODE -ne 0) { throw 'Existing final APK failed package verification; inspect the baseline report before replacing it.' }

    $viewerEnvironment = @{
        ANDROID_HOME = $viewerSDK; ANDROID_SDK_ROOT = $viewerSDK
        NDKROOT = (Join-Path $viewerSDK 'ndk\27.2.12479018'); NDK_ROOT = (Join-Path $viewerSDK 'ndk\27.2.12479018')
        JAVA_HOME = $JavaHome; GRADLE_USER_HOME = 'C:\Users\kevin\.gradle'
    }
    foreach ($viewerKey in $viewerEnvironment.Keys) {
        $viewerSavedEnvironment[$viewerKey] = [Environment]::GetEnvironmentVariable($viewerKey, 'Process')
        [Environment]::SetEnvironmentVariable($viewerKey, $viewerEnvironment[$viewerKey], 'Process')
    }
    Write-Host "Building Android native code only. Log: $viewerBuildLog"
    & $viewerDotnet $viewerUBT HandoffQuestHUD Android Development "-Project=$viewerProject" "-remoteini=$viewerRoot" -skipdeploy -NoHotReloadFromIDE *> $viewerBuildLog
    if ($LASTEXITCODE -ne 0) { throw "Android native compilation failed (exit $LASTEXITCODE). See $viewerBuildLog" }
    if (-not (Test-Path -LiteralPath $viewerNative -PathType Leaf)) { throw 'Successful native build produced no expected arm64 library.' }
    Copy-Item -LiteralPath $viewerNative -Destination $viewerGradleNative
    $viewerNativeHash = (Get-FileHash -LiteralPath $viewerNative -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $viewerGradleNative -Algorithm SHA256).Hash -ne $viewerNativeHash) { throw 'Copied Gradle native input does not match compiled library.' }

    Write-Host "Assembling greentest offline. Log: $viewerGradleLog"
    Push-Location -LiteralPath $viewerGradle
    try {
        # Override UE's copy finalizer so this cannot overwrite an ambiguously
        # named original-package APK in the project's Binaries directory.
        & $viewerGradleCommand :app:assembleDebug --offline --no-daemon --max-workers=2 `
            --project-cache-dir (Join-Path $viewerSetup 'GreenTestGradleCache') `
            "-POUTPUT_PATH=$viewerRun" '-POUTPUT_FILENAME=WallhackSpatialTracking.candidate.apk' *> $viewerGradleLog
        if ($LASTEXITCODE -ne 0) { throw "Green Test assembly failed (exit $LASTEXITCODE). See $viewerGradleLog" }
    }
    finally { Pop-Location }
    if (-not (Test-Path -LiteralPath $viewerCandidate -PathType Leaf)) { throw 'Gradle did not produce the candidate APK at the requested output path.' }

    & $Python $viewerVerifier --apk $viewerCandidate --report (Join-Path $viewerRun 'package-verification.json') *> (Join-Path $viewerLogs "SpatialViewerVerify-$viewerStamp.log")
    if ($LASTEXITCODE -ne 0) { throw "Candidate package verification failed. See $viewerRun\package-verification.json" }
    $viewerSignature = (& $viewerJava -jar $viewerSigner verify --verbose --print-certs $viewerCandidate 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw 'apksigner rejected the candidate signature.' }
    $viewerCertificate = [regex]::Match($viewerSignature, 'Signer #1 certificate SHA-256 digest: ([0-9a-fA-F]+)').Groups[1].Value.ToUpperInvariant()
    if ($viewerCertificate -ne $viewerBaselineCertificate -or $viewerSignature -notmatch 'Number of signers: 1' -or
        $viewerSignature -notmatch 'Verified using v2 scheme \(APK Signature Scheme v2\): true') {
        throw 'Candidate signing certificate/scheme does not match the installed Green Test baseline.'
    }
    $viewerNativeCheck = @'
import hashlib,json,sys
from pathlib import Path
from zipfile import ZipFile
apk,native=map(Path,sys.argv[1:])
with native.open('rb') as f: expected=hashlib.file_digest(f,'sha256').hexdigest().upper()
with ZipFile(apk) as z,z.open('lib/arm64-v8a/libUnreal.so') as f: actual=hashlib.file_digest(f,'sha256').hexdigest().upper()
if actual!=expected:raise SystemExit('APK native payload differs from final Gradle stripped output')
print(actual)
'@
    $viewerAPKNativeHash = & $Python -c $viewerNativeCheck $viewerCandidate $viewerStripped
    if ($LASTEXITCODE -ne 0) { throw 'Candidate native payload verification failed.' }
    $viewerAfter = Read-ViewerInputs
    if ($viewerAfter.snapshot_sha256 -ne $viewerBefore.snapshot_sha256 -or
        $viewerAfter.manifest_sha256 -ne $viewerBefore.manifest_sha256 -or $viewerAfter.obb_sha256 -ne $viewerBefore.obb_sha256) {
        throw 'Source/config/content or preserved manifest/OBB changed during packaging. Freeze inputs and rerun before replacing the final APK.'
    }
    if ((Get-FileHash -LiteralPath $viewerNative -Algorithm SHA256).Hash -ne $viewerNativeHash -or
        (Get-FileHash -LiteralPath $viewerGradleNative -Algorithm SHA256).Hash -ne $viewerNativeHash) {
        throw 'Another operation changed native output/input during packaging; final APK was not replaced.'
    }
    if ((Get-FileHash -LiteralPath $viewerFinal -Algorithm SHA256).Hash -ne $viewerOldHash) {
        throw 'The existing final APK changed during packaging; refusing to overwrite a concurrent result.'
    }
    $viewerBackupDirectory = Join-Path $viewerSetup "SpatialViewer-backups\$viewerStamp"
    New-Item -ItemType Directory -Path $viewerBackupDirectory -Force | Out-Null
    $viewerBackup = Join-Path $viewerBackupDirectory 'WallhackSpatialTracking.apk'
    Copy-Item -LiteralPath $viewerFinal -Destination $viewerBackup
    if ((Get-FileHash -LiteralPath $viewerBackup -Algorithm SHA256).Hash -ne $viewerOldHash) { throw 'Rollback APK backup failed integrity verification.' }
    $viewerCandidateHash = (Get-FileHash -LiteralPath $viewerCandidate -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $viewerCandidate -Destination $viewerFinal
    if ((Get-FileHash -LiteralPath $viewerFinal -Algorithm SHA256).Hash -ne $viewerCandidateHash) {
        throw "Final APK copy failed integrity verification. The previous APK is preserved at $viewerBackup"
    }
    $viewerReport = [ordered]@{
        recorded_at_utc = [DateTime]::UtcNow.ToString('o'); status = 'verified_native_only_package'
        apk = $viewerFinal; apk_sha256 = $viewerCandidateHash; candidate = $viewerCandidate
        rollback_apk = $viewerBackup; rollback_sha256 = $viewerOldHash
        android_native_sha256 = $viewerNativeHash; apk_native_sha256 = [string]$viewerAPKNativeHash
        signing_certificate_sha256 = $viewerCertificate; manifest_sha256 = $viewerBefore.manifest_sha256
        preserved_obb_sha256 = $viewerBefore.obb_sha256; cook_provenance = $CookProvenance
        input_snapshot_sha256 = $viewerBefore.snapshot_sha256; inputs_sha256 = $viewerBefore.inputs_sha256
        package_verification = (Join-Path $viewerRun 'package-verification.json')
        native_build_log = $viewerBuildLog; gradle_log = $viewerGradleLog
        scope = 'Native rebuild and existing cooked Green Test assets; no device installation or hardware verification.'
    }
    $viewerReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $viewerRun 'package-provenance.json') -Encoding UTF8
    Write-Host "PASS: $viewerFinal"
    Write-Host "SHA-256: $viewerCandidateHash"
    Write-Host "Previous APK: $viewerBackup"
    Write-Host "Provenance: $viewerRun\package-provenance.json"
}
catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
finally {
    foreach ($viewerKey in $viewerSavedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($viewerKey, $viewerSavedEnvironment[$viewerKey], 'Process')
    }
    if ($null -ne $viewerLock) { $viewerLock.Dispose() }
}
exit 0
