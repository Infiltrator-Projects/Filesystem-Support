# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$DriverFile = 'filesystem_support_ext2.sys'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}
if ([string]::IsNullOrWhiteSpace($DriverFile) -or
    $DriverFile.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
    [IO.Path]::GetFileName($DriverFile) -ne $DriverFile) {
    throw 'DriverFile must be a driver filename, not a path.'
}

& verifier.exe /reset | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "verifier /reset failed with exit code $LASTEXITCODE."
}
& verifier.exe /standard /driver $DriverFile | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Driver Verifier configuration failed with exit code $LASTEXITCODE."
}

Write-Host "Driver Verifier standard checks are configured for $DriverFile."
Write-Host 'Restart the disposable qualification machine before exercising the driver.'
Write-Host 'Recovery: start Windows in Safe Mode and run verifier.exe /reset.'
