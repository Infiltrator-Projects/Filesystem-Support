# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$driver = Get-Content -LiteralPath (Join-Path $root 'windows\driver\extfs_driver.c') -Raw
$header = Get-Content -LiteralPath (Join-Path $root 'windows\driver\extfs_driver.h') -Raw
$core = Get-Content -LiteralPath (Join-Path $root 'core\extfs.c') -Raw
$coreHeader = Get-Content -LiteralPath (Join-Path $root 'include\extfs\extfs.h') -Raw
foreach ($token in @('VolumeLockFileObject','VPB_LOCKED','ExtfsLockMountedVolume','ExtfsUnlockMountedVolume','ExtfsDismountLockedVolume','FSCTL_DISMOUNT_VOLUME','NameStatus','STATUS_VOLUME_DISMOUNTED')) {
    if ($driver -notmatch [regex]::Escape($token) -and $header -notmatch [regex]::Escape($token)) { throw "Missing filesystem contract: $token" }
}
if ($coreHeader -notmatch '#define EXTFS_VERSION_PATCH 3') { throw 'Portable core version is not 0.9.3.' }
if ($core -notmatch 'EXTFS_ERR_NO_SPACE:\s*return "no space left on device"') { throw 'NO_SPACE status text is missing.' }
if ($driver -notmatch 'pick->NameStatus = status') { throw 'Invalid ext byte names can still be silently skipped.' }
Write-Host 'Filesystem lifecycle/name/version contracts: PASS'
