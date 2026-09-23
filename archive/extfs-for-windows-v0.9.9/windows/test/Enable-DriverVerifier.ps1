# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}

& verifier.exe /reset | Out-Host
if ($LASTEXITCODE -ne 0) { throw "verifier /reset failed with exit code $LASTEXITCODE." }
& verifier.exe /standard /driver extfs.sys | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Driver Verifier configuration failed with exit code $LASTEXITCODE." }

Write-Host ''
Write-Host 'Driver Verifier standard checks are configured for extfs.sys only.'
Write-Host 'Restart the disposable test VM before exercising ExtFS.'
Write-Host 'To recover from a verifier-triggered boot loop, use Safe Mode and run: verifier /reset'
