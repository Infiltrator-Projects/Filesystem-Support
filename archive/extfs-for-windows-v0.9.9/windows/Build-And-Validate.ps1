# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$SkipCodeAnalysis
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $PSScriptRoot 'driver\extfs.sln'
$project = Join-Path $PSScriptRoot 'driver\extfs.vcxproj'
$inf = Join-Path $PSScriptRoot 'driver\extfs.inf'
$release = Join-Path $root 'release\driver'
$packagesConfig = Join-Path $root 'packages.config'
$packagesDir = Join-Path $root 'packages'
$wdkNuGetVersion = '10.0.28000.2526'
$wdkPackageName = if ($Platform -eq 'ARM64') { 'Microsoft.Windows.WDK.ARM64' } else { 'Microsoft.Windows.WDK.x64' }
$driverVersion = '0.9.3.0'
$driverDate = '08/20/2026'
$commonVersionFile = Join-Path $root 'third_party\infiltratr-common\VERSION'
if (-not (Test-Path -LiteralPath $commonVersionFile)) {
    throw 'Infiltratr Common 1.19.24 is missing. Clone ExtFS with --recurse-submodules.'
}
$commonVersion = (Get-Content -LiteralPath $commonVersionFile -Raw).Trim()
if ($commonVersion -ne '1.19.24') {
    throw "ExtFS requires Infiltratr Common 1.19.24; found '$commonVersion'."
}

function Get-PortableExecutableMachine {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE image: $Path" }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Find-NuGet {
    $command = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $wingetLink = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\nuget.exe'
    if (Test-Path -LiteralPath $wingetLink) { return $wingetLink }

    throw 'NuGet was not found. Install Microsoft.NuGet (winget install --id Microsoft.NuGet --source winget).'
}


function Enter-VisualStudioDeveloperShell {
    if ($env:VSCMD_VER) {
        Write-Host "Visual Studio Developer Shell already active ($($env:VSCMD_VER))."
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'vswhere.exe was not found. Visual Studio 2017 or later is required.'
    }

    $installation = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -property installationPath | Select-Object -First 1
    if (-not $installation) {
        throw 'No Visual Studio installation containing MSBuild was found.'
    }

    $devShellDll = Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $devShellDll)) {
        throw "Visual Studio Developer Shell module was not found: $devShellDll"
    }

    Import-Module $devShellDll
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation | Out-Null
    Write-Host "Visual Studio Developer Shell initialised: $installation"
}

function Add-WdkBuildToolsToPath {
    param([Parameter(Mandatory)][string]$RestoredPackages,
          [Parameter(Mandatory)][string]$Version,
          [Parameter(Mandatory)][string]$PackageName)

    # WDK MSBuild tasks launch helper programs such as StampInf through the
    # process search path. Prefer the helper from the pinned WDK package so
    # that the executable version matches the headers/targets being built.
    $wdkPackage = Join-Path $RestoredPackages "$PackageName.$Version"
    $stampinf = $null
    if (Test-Path -LiteralPath $wdkPackage) {
        $stampinf = Get-ChildItem -LiteralPath $wdkPackage -Filter 'stampinf.exe' -File -Recurse `
            -ErrorAction SilentlyContinue |
            Where-Object FullName -Match '\\x64\\' |
            Sort-Object FullName -Descending |
            Select-Object -First 1
    }

    if (-not $stampinf) {
        $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
        if (Test-Path -LiteralPath $kits) {
            $stampinf = Get-ChildItem -LiteralPath $kits -Filter 'stampinf.exe' -File -Recurse `
                -ErrorAction SilentlyContinue |
                Where-Object FullName -Match '\\x64\\' |
                Sort-Object FullName -Descending |
                Select-Object -First 1
        }
    }

    if (-not $stampinf) {
        throw 'stampinf.exe was not found in either the restored WDK NuGet package or the installed Windows Kits.'
    }

    $toolDirectory = Split-Path -Parent $stampinf.FullName
    $pathEntries = $env:Path -split ';'
    if ($pathEntries -notcontains $toolDirectory) {
        $env:Path = "$toolDirectory;$env:Path"
    }
    Write-Host "WDK build-tool path: $toolDirectory"
    Write-Host "StampInf: $($stampinf.FullName)"
}

function Find-MSBuild {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($path) { return $path }
    }
    throw 'MSBuild was not found. Install Visual Studio Build Tools/Visual Studio with the WDK.'
}

function Find-WindowsKitTool {
    param([Parameter(Mandatory)][string]$Name,
          [string]$RestoredPackages)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    foreach ($root in @($RestoredPackages,
                         (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'))) {
        if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
        $candidates = @(Get-ChildItem -LiteralPath $root -Filter $Name -File -Recurse `
            -ErrorAction SilentlyContinue | Sort-Object FullName -Descending)
        if ($candidates.Count -eq 0) { continue }

        # Prefer an x64-hosted utility when one exists. Some WDK package tools,
        # notably Inf2Cat, are distributed only as an x86-hosted executable even
        # when validating an x64 driver package; those tools are architecture-
        # neutral with respect to the package they inspect.
        $candidate = $candidates |
            Where-Object FullName -Match '\\x64\\' |
            Select-Object -First 1
        if (-not $candidate) { $candidate = $candidates | Select-Object -First 1 }
        if ($candidate) { return $candidate.FullName }
    }
    throw "$Name was not found in the restored WDK/SDK packages or installed Windows Kits."
}

$nuget = Find-NuGet
Write-Host "Restoring pinned Microsoft WDK/SDK NuGet packages ($wdkNuGetVersion)..."
& $nuget restore $packagesConfig -PackagesDirectory $packagesDir -NonInteractive
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed with exit code $LASTEXITCODE." }

$wdkProps = Join-Path $packagesDir "$wdkPackageName.$wdkNuGetVersion\build\native\$wdkPackageName.props"
if (-not (Test-Path -LiteralPath $wdkProps)) {
    throw "WDK NuGet restore completed but the expected WDK build properties were not found: $wdkProps"
}

# Microsoft's current WDK sample build flow enters the Visual Studio Developer
# Shell for both installed-WDK and NuGet-WDK builds. This supplies the normal
# compiler/SDK environment expected by WDK MSBuild tasks. We also expose the
# pinned WDK helper directory explicitly because tasks such as StampInf launch
# their executable through PATH.
Enter-VisualStudioDeveloperShell
Add-WdkBuildToolsToPath -RestoredPackages $packagesDir -Version $wdkNuGetVersion -PackageName $wdkPackageName

$msbuild = Find-MSBuild
$infverif = Find-WindowsKitTool -Name 'InfVerif.exe' -RestoredPackages $packagesDir
$inf2cat = Find-WindowsKitTool -Name 'Inf2Cat.exe' -RestoredPackages $packagesDir

$platformMsbuildArguments = @()
if ($Platform -eq 'ARM64') {
    $arm64Runtime = @($packagesDir, $env:VCToolsInstallDir) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
        ForEach-Object {
            Get-ChildItem -LiteralPath $_ -Filter 'arm64rt.lib' -File -Recurse -ErrorAction SilentlyContinue
        } |
        Where-Object FullName -Match '\\arm64\\' |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $arm64Runtime) {
        throw 'arm64rt.lib was not found in the restored WDK/SDK packages or Visual C++ ARM64 tools.'
    }
    $arm64RuntimeDirectory = Split-Path -Parent $arm64Runtime.FullName
    $platformMsbuildArguments = @("/p:ExtfsArm64RuntimeDir=$arm64RuntimeDirectory")
    Write-Host "ARM64 runtime library: $($arm64Runtime.FullName)"
}

Write-Host "WDK NuGet props: $wdkProps"
Write-Host "Building ExtFS $Configuration $Platform with: $msbuild"
$buildArguments = @($solution, '/m', '/t:Clean,Build', "/p:Configuration=$Configuration", "/p:Platform=$Platform") + $platformMsbuildArguments
& $msbuild @buildArguments
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }

if (-not $SkipCodeAnalysis) {
    Write-Host 'Running WDK/Visual C++ driver Code Analysis...'
    $analysisArguments = @($project, '/m', "/p:Configuration=$Configuration", "/p:Platform=$Platform", '/p:RunCodeAnalysisOnce=True') + $platformMsbuildArguments
    & $msbuild @analysisArguments
    if ($LASTEXITCODE -ne 0) { throw "Driver Code Analysis failed with exit code $LASTEXITCODE." }
}

$driverRoot = Join-Path $PSScriptRoot 'driver'
$driver = Get-ChildItem -LiteralPath $driverRoot -Filter extfs.sys -File -Recurse |
    Where-Object FullName -Match "\\$Platform\\$Configuration\\" |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if (-not $driver) {
    throw 'The build completed but extfs.sys could not be located under windows\driver.'
}
$peMachine = Get-PortableExecutableMachine -Path $driver.FullName
$expectedPeMachine = if ($Platform -eq 'ARM64') { 0xAA64 } else { 0x8664 }
if ($peMachine -ne $expectedPeMachine) {
    throw ('Driver machine mismatch: expected 0x{0:X4} for {1}, found 0x{2:X4}.' -f $expectedPeMachine, $Platform, $peMachine)
}

# The checked-in INF is the release manifest. WDK's build output may carry a
# development timestamp/version, so never stage that generated copy.
$driverVerLine = Get-Content -LiteralPath $inf |
    Where-Object { $_ -match '^\s*DriverVer\s*=' } |
    Select-Object -First 1
$normalisedDriverVer = $driverVerLine -replace '\s', ''
$expectedDriverVer = "DriverVer=$driverDate,$driverVersion"
if ($normalisedDriverVer -ne $expectedDriverVer) {
    throw "Source INF version mismatch: expected '$expectedDriverVer', found '$driverVerLine'."
}

if (Test-Path -LiteralPath $release) {
    Remove-Item -LiteralPath $release -Recurse -Force
}
New-Item -ItemType Directory -Path $release -Force | Out-Null
$stagedDriver = Join-Path $release 'extfs.sys'
$stagedInf = Join-Path $release 'extfs.inf'
$stagedCatalog = Join-Path $release 'extfs.cat'
Copy-Item -LiteralPath $driver.FullName -Destination $stagedDriver -Force
Copy-Item -LiteralPath $inf -Destination $stagedInf -Force

Write-Host 'Running InfVerif in Windows Driver mode against the staged package...'
& $infverif /w /v $stagedInf
if ($LASTEXITCODE -ne 0) { throw "InfVerif failed with exit code $LASTEXITCODE." }

# Generate the catalog explicitly after staging the final SYS and release-controlled INF.
# This avoids relying on Visual Studio's implicit signing certificate and makes
# the catalog hashes correspond exactly to the package that our setup ships.
$inf2catOs = if ($Platform -eq 'ARM64') {
    '10_VB_ARM64,10_CO_ARM64,10_NI_ARM64,10_GE_ARM64,10_25H2_ARM64'
} else {
    '10_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64,10_25H2_X64'
}
Write-Host "Generating extfs.cat with Inf2Cat for: $inf2catOs"
& $inf2cat "/driver:$release" "/os:$inf2catOs" /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed with exit code $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $stagedCatalog)) {
    throw "Inf2Cat completed but did not produce the catalog declared by extfs.inf: $stagedCatalog"
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedDriver).Hash
$catHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedCatalog).Hash
$report = @(
    "ExtFS Windows build validation"
    "Date: $([DateTime]::Now.ToString('o'))"
    "Configuration: $Configuration"
    "Platform: $Platform"
    ("PEMachine: 0x{0:X4}" -f $peMachine)
    "MSBuild: $msbuild"
    "NuGet: $nuget"
    "WDKNuGet: $wdkNuGetVersion"
    "InfVerif: $infverif"
    "Inf2Cat: $inf2cat"
    "CodeAnalysis: $(-not $SkipCodeAnalysis)"
    "DriverVer: $driverDate,$driverVersion"
    "Driver: $stagedDriver"
    "DriverSHA256: $hash"
    "CatalogSHA256: $catHash"
) -join [Environment]::NewLine
Set-Content -LiteralPath (Join-Path $release 'build-report.txt') -Value $report -Encoding UTF8

Write-Host ''
Write-Host 'WDK build and static package validation completed.'
Write-Host "Driver: $stagedDriver"
Write-Host "Driver SHA-256: $hash"
Write-Host "Catalog: $stagedCatalog"
Write-Host "Catalog SHA-256: $catHash"
