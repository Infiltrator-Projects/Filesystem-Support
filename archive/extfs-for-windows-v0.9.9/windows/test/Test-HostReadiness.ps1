# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$TargetArchitecture = 'ARM64',
    [int]$ExternalDiskNumber = -1,
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

function Get-UninstallMatches {
    $paths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    return @(Get-ItemProperty -Path $paths -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -match '(?i)Paragon|UFSD|ExtFS|Ext2Fsd' } |
        Select-Object DisplayName, DisplayVersion, Publisher, UninstallString, PSPath)
}

$isAdmin = Test-Administrator
$machineArchitecture = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
$expectedMachineArchitecture = if ($TargetArchitecture -eq 'ARM64') { 'ARM64' } else { 'AMD64' }
$pendingRestartReasons = @(Get-PendingRestartReasons)
$testSigningText = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
$testSigningEnabled = $testSigningText -match '(?im)^testsigning\s+Yes\s*$'

$secureBootState = 'Unknown'
$secureBootDetail = $null
try {
    $secureBootState = if (Confirm-SecureBootUEFI -ErrorAction Stop) { 'Enabled' } else { 'Disabled' }
} catch {
    $secureBootDetail = $_.Exception.Message
}

$apps = @(Get-UninstallMatches)
$services = @()
try {
    $services = @(Get-CimInstance Win32_SystemDriver -ErrorAction Stop |
        Where-Object { $_.Name -match '(?i)Paragon|UFSD|ExtFS|Ext2Fsd' -or $_.DisplayName -match '(?i)Paragon|UFSD|ExtFS|Ext2Fsd' } |
        Select-Object Name, DisplayName, State, StartMode, PathName)
} catch {
    $services = @([pscustomobject]@{ Error = $_.Exception.Message })
}

$disks = @()
try {
    $disks = @(Get-Disk -ErrorAction Stop | Select-Object Number, FriendlyName, BusType, PartitionStyle, OperationalStatus, HealthStatus, Size, IsBoot, IsSystem, IsReadOnly, IsOffline)
} catch {
    $disks = @([pscustomobject]@{ Error = $_.Exception.Message })
}

$selectedDisk = $null
$selectedPartitions = @()
if ($ExternalDiskNumber -ge 0) {
    try {
        $selectedDisk = Get-Disk -Number $ExternalDiskNumber -ErrorAction Stop |
            Select-Object Number, FriendlyName, BusType, PartitionStyle, OperationalStatus, HealthStatus, Size, IsBoot, IsSystem, IsReadOnly, IsOffline
        $selectedPartitions = @(Get-Partition -DiskNumber $ExternalDiskNumber -ErrorAction SilentlyContinue |
            Select-Object PartitionNumber, DriveLetter, Type, Size, IsReadOnly, IsOffline, IsHidden, IsSystem)
    } catch {
        $selectedDisk = [pscustomobject]@{ Error = $_.Exception.Message }
    }
}

$bitLocker = @()
try {
    $bitLocker = @(Get-BitLockerVolume -ErrorAction Stop | Select-Object MountPoint, VolumeType, VolumeStatus, ProtectionStatus, EncryptionMethod)
} catch {
    $bitLocker = @([pscustomobject]@{ Error = $_.Exception.Message })
}

$blockingReasons = [Collections.Generic.List[string]]::new()
if (-not $isAdmin) { $blockingReasons.Add('Run the preflight from an elevated PowerShell window for authoritative security and driver results.') }
if ($machineArchitecture -ne $expectedMachineArchitecture) { $blockingReasons.Add("Host architecture is $machineArchitecture; package target is $TargetArchitecture.") }
if ($pendingRestartReasons.Count -gt 0) { $blockingReasons.Add('Windows has a pending restart.') }
if ($secureBootState -ne 'Disabled') { $blockingReasons.Add("Secure Boot must be confirmed Disabled on a disposable test system; current state is $secureBootState.") }
if (-not $testSigningEnabled) { $blockingReasons.Add('Windows TESTSIGNING is not enabled; a restart is required after changing it.') }
if ($ExternalDiskNumber -lt 0) {
    $blockingReasons.Add('No explicit external test disk number was supplied.')
} elseif ($selectedDisk.Error) {
    $blockingReasons.Add("Selected disk could not be inspected: $($selectedDisk.Error)")
} else {
    if ($selectedDisk.IsBoot -or $selectedDisk.IsSystem) { $blockingReasons.Add('Selected disk is a boot/system disk and must never be used for ExtFS testing.') }
    if ($selectedDisk.BusType -notin @('USB', 'SD', 'MMC', 'Virtual')) { $blockingReasons.Add("Selected disk bus type is $($selectedDisk.BusType), not an expected removable/test bus.") }
    if ($selectedDisk.IsOffline) { $blockingReasons.Add('Selected disk is offline.') }
    if ($selectedDisk.HealthStatus -ne 'Healthy') { $blockingReasons.Add("Selected disk health is $($selectedDisk.HealthStatus).") }
}
if ($services | Where-Object { -not $_.Error }) { $blockingReasons.Add('A potentially conflicting filesystem driver/service is present; identify and remove it before ExtFS installation.') }

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
    MatchingInstalledApplications = $apps
    MatchingFilesystemDrivers = $services
    Disks = $disks
    SelectedExternalDisk = $selectedDisk
    SelectedDiskPartitions = $selectedPartitions
    BitLocker = $bitLocker
    ReadyForExperimentalInstall = ($blockingReasons.Count -eq 0)
    BlockingReasons = @($blockingReasons)
}

$json = $report | ConvertTo-Json -Depth 8
if ($OutputPath) {
    $parent = Split-Path -Parent $OutputPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { throw "Output directory does not exist: $parent" }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding UTF8
}
$json
if ($blockingReasons.Count -gt 0) { exit 2 }
