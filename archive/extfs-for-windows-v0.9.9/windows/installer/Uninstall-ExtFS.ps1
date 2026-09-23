# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$serviceName = 'ExtFS'
$driverDestination = Join-Path $env:SystemRoot 'System32\drivers\extfs.sys'
$certificateSource = Join-Path $PSScriptRoot 'extfs-test.cer'

function Wait-ServiceStopped {
    param([string]$Name, [int]$Attempts = 50)
    for ($i = 0; $i -lt $Attempts; $i++) {
        $text = (& sc.exe query $Name 2>&1 | Out-String)
        $code = $LASTEXITCODE
        if ($code -ne 0 -or $text -match '(?im)^\s*STATE\s*:\s*1\b') {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Wait-ServiceRemoved {
    param([string]$Name, [int]$Attempts = 50)
    for ($i = 0; $i -lt $Attempts; $i++) {
        & sc.exe query $Name *> $null
        if ($LASTEXITCODE -ne 0) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'ExtFS Uninstall must be run as administrator.'
}

# Remove the filesystem service and driver image before removing the exact
# bundled test certificate from both stores populated by Install-ExtFS.ps1.
& sc.exe query $serviceName *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop $serviceName *> $null
    if (-not (Wait-ServiceStopped -Name $serviceName)) {
        Write-Warning 'ExtFS is intentionally resident for this boot; restart Windows to complete driver removal.'
    }
    & sc.exe delete $serviceName *> $null
    if (-not (Wait-ServiceRemoved -Name $serviceName)) {
        Write-Warning 'ExtFS service deletion is still pending and may require a restart.'
    }
}
Remove-Item -LiteralPath $driverDestination -Force -ErrorAction SilentlyContinue

$thumbprint = $null
if (Test-Path -LiteralPath $certificateSource) {
    try {
        $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $certificateSource)
        $thumbprint = $certificate.Thumbprint
        $certificate.Dispose()
    } catch {
        Write-Warning "Could not read the bundled ExtFS certificate: $_"
    }
}

if ($thumbprint) {
    foreach ($store in @('Root', 'TrustedPublisher')) {
        Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object Thumbprint -eq $thumbprint |
            Remove-Item -Force
    }
} else {
    Write-Warning 'The bundled certificate was unavailable; no certificate-store entries were removed.'
}

Write-Host 'ExtFS has been removed. Windows test-signing mode was left unchanged.'
