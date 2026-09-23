# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$TargetArchitecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$serviceName = 'ExtFS'
$driverSource = Join-Path $PSScriptRoot 'extfs.sys'
$infSource = Join-Path $PSScriptRoot 'extfs.inf'
$catalogSource = Join-Path $PSScriptRoot 'extfs.cat'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'ExtFS Setup must be run as administrator.'
    }
}

function Get-NativeWindowsArchitecture {
    try {
        $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToUpperInvariant()
        switch ($architecture) {
            'X64'   { return 'AMD64' }
            'ARM64' { return 'ARM64' }
            'X86'   { return 'x86' }
        }
    } catch {}
    $architecture = $env:PROCESSOR_ARCHITEW6432
    if ([string]::IsNullOrWhiteSpace($architecture)) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    if ([string]::IsNullOrWhiteSpace($architecture)) { throw 'Windows native architecture could not be determined.' }
    return $architecture.ToUpperInvariant()
}

function Get-PeMachine {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE image: $Path" }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose(); $stream.Dispose()
    }
}

function Assert-MicrosoftSignature {
    param([Parameter(Mandatory)][string]$Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
        throw "A valid trusted signature is required for '$Path'; status is '$($signature.Status)'."
    }
    $subject = $signature.SignerCertificate.Subject
    $issuer = $signature.SignerCertificate.Issuer
    if ($subject -notmatch '(?i)Microsoft' -and $issuer -notmatch '(?i)Microsoft') {
        throw "The production ExtFS payload must be Microsoft-signed. Signer subject='$subject'; issuer='$issuer'."
    }
}

function Test-PendingRestart {
    $sessionManager = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
    $fileRename = (Get-ItemProperty -LiteralPath $sessionManager -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    $componentServicing = Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'
    $windowsUpdate = Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'
    return [bool]($fileRename -or $componentServicing -or $windowsUpdate)
}

function Wait-ServiceStopped {
    param([string]$Name, [int]$Attempts = 50)
    for ($i = 0; $i -lt $Attempts; $i++) {
        $text = (& sc.exe query $Name 2>&1 | Out-String)
        $code = $LASTEXITCODE
        if ($code -ne 0 -or $text -match '(?im)^\s*STATE\s*:\s*1\b') { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Wait-ServiceRemoved {
    param([string]$Name, [int]$Attempts = 50)
    for ($i = 0; $i -lt $Attempts; $i++) {
        & sc.exe query $Name *> $null
        if ($LASTEXITCODE -ne 0) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Get-RecentDriverDiagnostics {
    $lines = @()
    $since = (Get-Date).AddMinutes(-5)
    foreach ($log in @(
        @{ LogName = 'System'; ProviderName = 'Service Control Manager' },
        @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational' }
    )) {
        try {
            $filter = @{ LogName = $log.LogName; StartTime = $since }
            if ($log.ProviderName) { $filter.ProviderName = $log.ProviderName }
            $lines += Get-WinEvent -FilterHashtable $filter -ErrorAction Stop |
                Select-Object -First 8 |
                ForEach-Object { "$($log.LogName) $($_.Id): $($_.Message -replace '[\r\n]+', ' ')" }
        } catch {
            $lines += "$($log.LogName) diagnostics unavailable: $($_.Exception.Message)"
        }
    }
    return ($lines -join [Environment]::NewLine)
}

Assert-Administrator

$native = Get-NativeWindowsArchitecture
$expectedNative = if ($TargetArchitecture -eq 'ARM64') { 'ARM64' } else { 'AMD64' }
if ($native -ne $expectedNative) {
    throw "This package targets $TargetArchitecture but Windows reports native architecture $native."
}
foreach ($file in @($driverSource, $infSource, $catalogSource)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Production package is missing '$file'." }
}
$expectedMachine = if ($TargetArchitecture -eq 'ARM64') { 0xAA64 } else { 0x8664 }
$machine = Get-PeMachine -Path $driverSource
if ($machine -ne $expectedMachine) {
    throw ('Driver architecture mismatch: expected 0x{0:X4}, found 0x{1:X4}.' -f $expectedMachine, $machine)
}

Assert-MicrosoftSignature -Path $driverSource
Assert-MicrosoftSignature -Path $catalogSource

if (Test-PendingRestart) {
    throw 'Windows has a pending restart. Restart Windows before installing the filesystem driver.'
}

try {
    $secureBoot = Confirm-SecureBootUEFI -ErrorAction Stop
    if ($secureBoot) {
        Write-Host 'Secure Boot is enabled. Continuing with the Microsoft-signed production driver.'
    } else {
        Write-Warning 'Secure Boot is disabled. The production package does not require it to be disabled.'
    }
} catch {
    Write-Warning "Secure Boot state could not be queried: $($_.Exception.Message)"
}

# Remove a stale development service before handing installation to SetupAPI.
& sc.exe query $serviceName *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop $serviceName *> $null
    if (-not (Wait-ServiceStopped -Name $serviceName)) {
        throw 'An older ExtFS driver is still resident in this boot. Restart Windows, then run production Setup again.'
    }
    & sc.exe delete $serviceName *> $null
    if ($LASTEXITCODE -ne 0 -or -not (Wait-ServiceRemoved -Name $serviceName)) {
        throw 'The older ExtFS service could not be removed cleanly.'
    }
}

# ExtFS is a primitive filesystem driver. On current Windows, processing its
# architecture-decorated DefaultInstall section routes through the supported
# primitive-driver installation path and places the package in the Driver Store.
$arguments = "setupapi.dll,InstallHinfSection DefaultInstall 132 `"$infSource`""
$install = Start-Process -FilePath "$env:SystemRoot\System32\rundll32.exe" -ArgumentList $arguments -Wait -PassThru
if ($install.ExitCode -ne 0 -and $install.ExitCode -ne 3010) {
    throw "Windows driver-package installation failed with exit code $($install.ExitCode)."
}
if ($install.ExitCode -eq 3010) {
    Write-Host 'Windows installed the production ExtFS package and requires a restart before it can be loaded.'
    exit 3010
}

& sc.exe query $serviceName *> $null
if ($LASTEXITCODE -ne 0) {
    throw 'SetupAPI completed, but the ExtFS filesystem service is not registered.'
}

$startOutput = (& sc.exe start $serviceName 2>&1 | Out-String).Trim()
$startExit = $LASTEXITCODE
if ($startExit -ne 0) {
    $diagnostics = Get-RecentDriverDiagnostics
    throw @"
Windows accepted the Microsoft-signed ExtFS driver package but the filesystem service did not start (sc.exe exit $startExit).

sc.exe output:
$startOutput

Recent diagnostics:
$diagnostics
"@
}

Write-Host "ExtFS production driver loaded on $TargetArchitecture Windows. Secure Boot was not disabled and TESTSIGNING was not used."
