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
Write-Host 'Driver Verifier settings were reset. Restart Windows to complete the change.'
