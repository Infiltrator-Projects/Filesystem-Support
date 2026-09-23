# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $root 'windows\installer\Install-ExtFS.ps1'
$nsi = Join-Path $root 'windows\installer\extfs-installer.nsi'
$packageBuilder = Join-Path $root 'windows\Build-ExperimentalSetup.ps1'
$wdkWorkflow = Join-Path $root '.github\workflows\windows-wdk-ci.yml'
$publishWorkflow = Join-Path $root '.github\workflows\publish-release.yml'
$oneClickBuilder = Join-Path $root 'BUILD-EXTFS.cmd'
$versionFile = Join-Path $root 'VERSION'
foreach ($path in @($installer, $nsi, $packageBuilder, $wdkWorkflow, $publishWorkflow, $oneClickBuilder, $versionFile)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Installer source not found: $path"
    }
}

$text = Get-Content -LiteralPath $installer -Raw
$nsiText = Get-Content -LiteralPath $nsi -Raw
$packageBuilderText = Get-Content -LiteralPath $packageBuilder -Raw
$wdkWorkflowText = Get-Content -LiteralPath $wdkWorkflow -Raw
$publishWorkflowText = Get-Content -LiteralPath $publishWorkflow -Raw
$oneClickBuilderText = Get-Content -LiteralPath $oneClickBuilder -Raw
$packageVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($packageVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Root VERSION '$packageVersion' is not semantic major.minor.patch."
}
$expectedAssignment = "`$serviceImagePath = '\SystemRoot\System32\drivers\extfs.sys'"
$badAssignment = "`$serviceImagePath = '\\SystemRoot\\System32\\drivers\\extfs.sys'"

if ($text -notmatch [regex]::Escape($expectedAssignment)) {
    throw "Install-ExtFS.ps1 must define the kernel service ImagePath exactly as \SystemRoot\System32\drivers\extfs.sys."
}
if ($text -match [regex]::Escape($badAssignment)) {
    throw 'Install-ExtFS.ps1 contains the invalid doubled-backslash kernel ImagePath that caused the 0.9.3 StartService failure.'
}
if ($text -notmatch [regex]::Escape('binPath= $serviceImagePath')) {
    throw 'sc.exe create must consume the validated $serviceImagePath variable.'
}
if ($text -notmatch [regex]::Escape("group= 'File System'")) {
    throw 'Filesystem service creation must retain the File System load-order group.'
}
if ($text -notmatch [regex]::Escape('Get-ItemProperty -LiteralPath $serviceRegistryPath')) {
    throw 'Installer must validate the actual service registry contract before attempting StartService.'
}
if ($text -notmatch [regex]::Escape('$configuredImagePath -ne $serviceImagePath')) {
    throw 'Installer must fail closed when the configured service ImagePath differs from the expected NT path.'
}

# 0.9.4 incorrectly asked .NET for the machine-scoped value of
# PROCESSOR_ARCHITECTURE. That variable is process-scoped and may be absent in
# the machine environment, falsely rejecting a genuine x64 Windows host.
$badArchitectureLookup = "GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')"
if ($text -match [regex]::Escape($badArchitectureLookup) -or
    $nsiText -match [regex]::Escape($badArchitectureLookup)) {
    throw 'Installer still contains the broken machine-scoped PROCESSOR_ARCHITECTURE lookup from 0.9.4.'
}
if ($text -notmatch [regex]::Escape('[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture')) {
    throw 'Installer must use RuntimeInformation.OSArchitecture for native Windows architecture detection.'
}
if ($text -notmatch [regex]::Escape('$env:PROCESSOR_ARCHITEW6432')) {
    throw 'Installer must retain a WOW64-safe native-architecture fallback.'
}
# Documentation may legitimately mention PROCESSOR_ARCHITECTURE. Reject only
# an executable NSIS gate that invokes it, not explanatory comments.
if ($nsiText -match '(?im)^\s*nsExec::ExecToStack.*PROCESSOR_ARCHITECTURE') {
    throw 'NSIS must not execute its own PROCESSOR_ARCHITECTURE architecture gate; PowerShell performs the authoritative OS and PE checks.'
}

if ($packageBuilderText -notmatch [regex]::Escape("Join-Path `$root 'VERSION'") -or
    $packageBuilderText -notmatch [regex]::Escape('"/DPACKAGE_VERSION=$packageVersion"')) {
    throw 'Experimental package builder must read root VERSION and pass it to NSIS.'
}
if ($nsiText -notmatch [regex]::Escape('!ifndef PACKAGE_VERSION') -or
    $nsiText -notmatch [regex]::Escape('${PACKAGE_VERSION}')) {
    throw 'NSIS installer must consume the PACKAGE_VERSION build definition.'
}
if ($text -notmatch [regex]::Escape("Join-Path `$PSScriptRoot 'VERSION'")) {
    throw 'Installed PowerShell setup must read the bundled VERSION file.'
}
if ($wdkWorkflowText -notmatch [regex]::Escape("Get-Content -LiteralPath 'VERSION'") -or
    $publishWorkflowText -notmatch [regex]::Escape("Get-Content -LiteralPath 'VERSION'") -or
    $oneClickBuilderText -notmatch [regex]::Escape('set /p "PACKAGE_VERSION="<"%~dp0VERSION"')) {
    throw 'CI, publishing and one-click builds must all resolve the root VERSION file.'
}
foreach ($source in @($packageBuilderText, $nsiText, $text, $wdkWorkflowText, $publishWorkflowText, $oneClickBuilderText)) {
    if ($source -match "(?<![0-9])$([regex]::Escape($packageVersion))(?![0-9])") {
        throw "Executable package source hard-codes current version $packageVersion instead of consuming VERSION."
    }
}

Write-Host "Installer service-path, architecture and package-version contracts: PASS ($packageVersion)"
