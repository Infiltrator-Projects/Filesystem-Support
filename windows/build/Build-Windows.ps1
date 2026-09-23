# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildRoot = Join-Path $root "build\windows-$($Platform.ToLowerInvariant())"

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'CMake was not found. Install CMake and ensure cmake.exe is on PATH.'
}

function Find-CTest {
    $command = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'CTest was not found. Install CMake and ensure ctest.exe is on PATH.'
}

$cmake = Find-CMake
$ctest = Find-CTest
$generatorPlatform = if ($Platform -eq 'ARM64') { 'ARM64' } else { 'x64' }

Write-Host "Filesystem Support Windows build"
Write-Host "Root:          $root"
Write-Host "Build:         $buildRoot"
Write-Host "Configuration: $Configuration"
Write-Host "Platform:      $Platform"

& $cmake -S $root -B $buildRoot -A $generatorPlatform -DFILESYSTEM_SUPPORT_WARNINGS_AS_ERRORS=ON
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

& $cmake --build $buildRoot --config $Configuration --target filesystem-support
if ($LASTEXITCODE -ne 0) {
    throw "Filesystem Support manager build failed with exit code $LASTEXITCODE."
}

if (-not $SkipTests) {
    & $ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Filesystem Support Windows tests failed with exit code $LASTEXITCODE."
    }
}

$manager = Get-ChildItem -LiteralPath $buildRoot -Filter 'filesystem-support.exe' -File -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1

if (-not $manager) {
    throw 'Build completed but filesystem-support.exe could not be located.'
}

Write-Host ''
Write-Host 'Windows manager build completed.'
Write-Host "Manager: $($manager.FullName)"
Write-Host ''
Write-Host 'No filesystem driver module is built or installable yet.'
Write-Host 'EXT2 remains the first shared-engine Windows qualification target.'
