<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ARM64 test transition

This procedure is for the test-only ARM64 preview. It does not make ExtFS production-ready. A kernel filesystem defect can crash Windows or damage data, so use a disposable Windows installation and a disposable, fully backed-up ext volume.

## Stop conditions

Do not uninstall the existing filesystem product or install ExtFS while any of these is true:

- the ARM64 GitHub prerelease and its SHA-256 have not been independently verified;
- Windows has a pending restart;
- the exact existing Paragon product and its uninstaller have not been identified;
- the external disk contains the only copy of any data;
- the selected disk is a Windows boot/system disk;
- the BitLocker recovery key is unavailable;
- Secure Boot or BitLocker state is unknown.

## 1. Preserve rollback

1. Keep Paragon installed while qualifying the ExtFS artifact.
2. Record the exact Paragon product name and version from **Settings > Apps > Installed apps**.
3. Save its installer, licence details and configuration.
4. Make and verify a separate backup of the external ext disk.
5. Save the Windows BitLocker recovery key. If firmware boot settings will change, suspend BitLocker protection using Microsoft's documented procedure first.
6. Restart Windows to clear pending servicing and file-renaming operations.

## 2. Run the read-only preflight

Open an elevated PowerShell window in the repository and run:

```powershell
.\windows\test\Test-HostReadiness.ps1 -TargetArchitecture ARM64 -ExternalDiskNumber <disk-number> -OutputPath .\extfs-host-readiness.json
```

Replace `<disk-number>` with the explicitly verified external test disk. Review every field in the JSON report. `ReadyForExperimentalInstall` must be `true`; never choose a disk marked `IsBoot` or `IsSystem`.

## 3. Remove the conflicting product

Only after the ARM64 package and rollback materials are ready:

1. Disconnect the external ext disk.
2. Uninstall the exact recorded Paragon product from **Settings > Apps > Installed apps**. Do not delete driver files or services by guessed names.
3. Restart Windows.
4. Run the preflight again. Confirm that `MatchingInstalledApplications` and `MatchingFilesystemDrivers` contain no Paragon/UFSD filesystem component.

## 4. Install and smoke-test

1. Verify the downloaded installer's SHA-256 against the GitHub prerelease.
2. Run the ARM64 installer as administrator. It will refuse a mismatched host, a mismatched driver PE image, a pending restart, enabled Secure Boot, or an unknown Secure Boot state.
3. If the installer enables Windows TESTSIGNING, restart and run it a second time.
4. Reconnect only the disposable, backed-up ext test disk.
5. Start with the read-only smoke probe in `windows/test/Test-ExtFS.ps1`.
6. Run reversible write probes only against a known disposable file after read-only results and collected diagnostics are clean.

Do not enable Driver Verifier on a primary/daily Windows installation. Driver Verifier testing belongs on a disposable VM or sacrificial test system because a failing filesystem driver can cause a boot loop.

## Rollback

1. Disconnect the external test disk.
2. Uninstall **ExtFS for Windows 0.9.3 Experimental (ARM64)** from Settings.
3. Restart Windows and confirm the `ExtFS` service and `System32\drivers\extfs.sys` are absent.
4. After ExtFS is removed, disable TESTSIGNING, restore the intended Secure Boot setting and resume BitLocker protection.
5. Reinstall the saved Paragon package if required, restart, then reconnect the disk.

Collect `windows/test/Collect-Diagnostics.ps1` output before changing a failed system; it records driver, service, Code Integrity and recent system-event evidence.
