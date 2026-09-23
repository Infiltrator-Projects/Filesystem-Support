# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$TargetArchitecture = 'x64',
    [int]$ExternalDiskNumber = -1,
    [switch]$RequireTestSigning,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-PendingRestartReasons {
    $reasons = [Collections.Generic.List[string]]::new()
    $sessionManager = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
    if ((Get-ItemProperty -LiteralPath $sessionManager -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations) {
        $reasons.Add('PendingFileRenameOperations')
    }
    if (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') {
        $reasons.Add('ComponentBasedServicing')
    }
    if (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') {
        $reasons.Add('WindowsUpdate')
    }
    return @($reasons)
}

$isAdmin = Test-Administrator
$machineArchitecture = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
$expectedArchitecture = if ($TargetArchitecture -eq 'ARM64') { 'ARM64' } else { 'AMD64' }
$pendingRestartReasons = @(Get-PendingRestartReasons)

$secureBootState = 'Unknown'
$secureBootDetail = $null
try {
    $secureBootState = if (Confirm-SecureBootUEFI -ErrorAction Stop) { 'Enabled' } else { 'Disabled' }
} catch {
    $secureBootDetail = $_.Exception.Message
}

$testSigningText = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
$testSigningEnabled = $testSigningText -match '(?im)^testsigning\s+Yes\s*$'

$disks = @()
try {
    $disks = @(Get-Disk -ErrorAction Stop | Select-Object Number, FriendlyName, BusType,
        PartitionStyle, OperationalStatus, HealthStatus, Size, IsBoot, IsSystem,
        IsReadOnly, IsOffline)
} catch {
    $disks = @([pscustomobject]@{ Error = $_.Exception.Message })
}

$selectedDisk = $null
if ($ExternalDiskNumber -ge 0) {
    try {
        $selectedDisk = Get-Disk -Number $ExternalDiskNumber -ErrorAction Stop |
            Select-Object Number, FriendlyName, BusType, PartitionStyle,
                OperationalStatus, HealthStatus, Size, IsBoot, IsSystem,
                IsReadOnly, IsOffline
    } catch {
        $selectedDisk = [pscustomobject]@{ Error = $_.Exception.Message }
    }
}

$blockingReasons = [Collections.Generic.List[string]]::new()
if (-not $isAdmin) { $blockingReasons.Add('Run from an elevated PowerShell window for authoritative driver checks.') }
if ($machineArchitecture -ne $expectedArchitecture) { $blockingReasons.Add("Host architecture is $machineArchitecture; requested target is $TargetArchitecture.") }
if ($pendingRestartReasons.Count -gt 0) { $blockingReasons.Add('Windows has a pending restart.') }
if ($RequireTestSigning -and -not $testSigningEnabled) { $blockingReasons.Add('TESTSIGNING is required for this development package and is not enabled.') }
if ($ExternalDiskNumber -ge 0) {
    if ($selectedDisk.Error) {
        $blockingReasons.Add("Selected disk could not be inspected: $($selectedDisk.Error)")
    } else {
        if ($selectedDisk.IsBoot -or $selectedDisk.IsSystem) { $blockingReasons.Add('Selected disk is a boot/system disk and must not be used for filesystem qualification.') }
        if ($selectedDisk.IsOffline) { $blockingReasons.Add('Selected disk is offline.') }
        if ($selectedDisk.HealthStatus -ne 'Healthy') { $blockingReasons.Add("Selected disk health is $($selectedDisk.HealthStatus).") }
    }
}

$report = [ordered]@{
    GeneratedAt = [DateTime]::Now.ToString('o')
    ComputerName = $env:COMPUTERNAME
    Administrator = $isAdmin
    TargetArchitecture = $TargetArchitecture
    MachineArchitecture = $machineArchitecture
    PendingRestartReasons = $pendingRestartReasons
    SecureBoot = $secureBootState
    SecureBootDetail = $secureBootDetail
    TestSigningEnabled = $testSigningEnabled
    RequireTestSigning = [bool]$RequireTestSigning
    Disks = $disks
    SelectedExternalDisk = $selectedDisk
    Ready = ($blockingReasons.Count -eq 0)
    BlockingReasons = @($blockingReasons)
}

$json = $report | ConvertTo-Json -Depth 8
if ($OutputPath) {
    $parent = Split-Path -Parent $OutputPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        throw "Output directory does not exist: $parent"
    }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding UTF8
}
$json
if ($blockingReasons.Count -gt 0) { exit 2 }
