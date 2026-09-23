# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$SkipCodeAnalysis,
    [string]$EvCertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'Build-And-Validate.ps1'
$release = Join-Path $root 'release\driver'
$submissionRoot = Join-Path $root "release\hardware-submission\$Platform"
$packageDir = Join-Path $submissionRoot 'ExtFS'
$cabName = "ExtFS-0.9.3-$Platform-Hardware-Submission.cab"
$cabPath = Join-Path $submissionRoot $cabName
$ddfPath = Join-Path $submissionRoot 'ExtFS.ddf'

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Build script not found: $buildScript"
}

& $buildScript -Configuration Release -Platform $Platform -SkipCodeAnalysis:$SkipCodeAnalysis

if (Test-Path -LiteralPath $submissionRoot) {
    Remove-Item -LiteralPath $submissionRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

foreach ($name in @('extfs.sys', 'extfs.inf', 'extfs.cat')) {
    $source = Join-Path $release $name
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Validated driver package is missing $name."
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageDir $name) -Force
}

$driverRoot = Join-Path $PSScriptRoot 'driver'
$pdb = Get-ChildItem -LiteralPath $driverRoot -Filter 'extfs.pdb' -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object FullName -Match "\\$Platform\\Release\\" |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if (-not $pdb) {
    throw 'extfs.pdb was not produced by the Release build. Partner Center submissions should include symbols for crash analysis.'
}
Copy-Item -LiteralPath $pdb.FullName -Destination (Join-Path $packageDir 'extfs.pdb') -Force

# Partner Center requires each driver package in its own CAB subdirectory; no
# driver-package files may live at CAB root. Microsoft regenerates the catalog
# during signing, but requires the submitted CAT for company verification.
$escaped = $packageDir.Replace('"', '""')
$ddf = @"
.OPTION EXPLICIT
.Set CabinetFileCountThreshold=0
.Set FolderFileCountThreshold=0
.Set FolderSizeThreshold=0
.Set MaxCabinetSize=0
.Set MaxDiskFileCount=0
.Set MaxDiskSize=0
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
.Set CabinetNameTemplate=$cabName
.Set DiskDirectoryTemplate=$submissionRoot
.Set DestinationDir=ExtFS
"$escaped\extfs.inf"
"$escaped\extfs.sys"
"$escaped\extfs.pdb"
"$escaped\extfs.cat"
"@
Set-Content -LiteralPath $ddfPath -Value $ddf -Encoding ascii

$makecab = Get-Command makecab.exe -ErrorAction Stop
& $makecab.Source /F $ddfPath
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cabPath)) {
    throw "MakeCab failed to produce $cabPath."
}

if ($EvCertificateThumbprint) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10" -Filter signtool.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object FullName -Match '\\x64\\' |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $signtool) { throw 'signtool.exe was not found.' }
    & $signtool.FullName sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $EvCertificateThumbprint /s My $cabPath
    if ($LASTEXITCODE -ne 0) { throw 'EV signing of the Hardware Dev Center CAB failed.' }
    & $signtool.FullName verify /pa /v $cabPath
    if ($LASTEXITCODE -ne 0) { throw 'EV-signed CAB verification failed.' }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $cabPath).Hash
Write-Host ''
Write-Host 'Hardware Dev Center submission bundle completed.'
Write-Host "Platform: $Platform"
Write-Host "CAB: $cabPath"
Write-Host "SHA-256: $hash"
if (-not $EvCertificateThumbprint) {
    Write-Host 'CAB is intentionally unsigned. Sign it with the EV certificate registered to the Hardware Dev Center account before submission.'
}
