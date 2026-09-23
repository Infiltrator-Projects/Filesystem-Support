# ExtFS-for-Windows release history

Preserved release metadata before repository retirement. Historical binaries were experimental migration inputs and are not Filesystem Support production artifacts.

## v0.9.1 — ExtFS for Windows 0.9.1

- Target: `2c57c86ac66f22bbae487003bdad25fb7b15d944`
- Published: 2026-08-12T07:52:40Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.1

Licensing-standardisation maintenance release.

- Project licensing standardised on GPL-3.0-or-later.
- SPDX identifiers added throughout source, build configuration and documentation.
- Functional baseline remains the 0.9 bounded EXT4 external extent-tree mutation checkpoint.
- Read support for supported EXT2/EXT3/EXT4 remains available.
- Existing bounded write/resize support remains experimental and deliberately fail-closed for unsupported layouts.

This is experimental filesystem-driver software and is not production-qualified.

Assets:

- `BUILD-EXTFS-ONE-CLICK-V17.cmd` — 23392 bytes; sha256:f3dd61de9449e85b0357d240cf669ba4f813364977dadd649e9a4940e74844cc

## v0.9.2 — ExtFS for Windows 0.9.2

- Target: `40eaa6db663f91a8ad343e07dab06362d71e32d5`
- Published: 2026-08-19T11:38:04Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.2

ExtFS for Windows 0.9.2 is a forensic-audit hardening and qualification checkpoint for the bounded 0.9 filesystem feature set.

Hardening includes an ext2 durability-capability preflight that refuses metadata resize before any write when the host cannot provide a stable-storage barrier, plus explicit dirty/mutation/clean flush ordering. It also adds destructive real-image ext2/ext3/ext4 grow/shrink/reopen/e2fsck qualification, expanded sanitizer/static-analysis CI, native Windows WDK Release x64 qualification, and reversible Windows append/EOF-resize test probes.

The filesystem feature boundary is intentionally not broadened. Namespace mutation, paging writes, volume lock ownership, dirty-journal replay, deeper/multi-leaf ext4 extent trees and 64-bit/flex_bg allocation remain outside this checkpoint. Persistent regular-file mtime/ctime mutation remains deferred.

Automated qualification passed on 18 August 2026: portable GCC/Clang/unit/real-image/sanitizer/static-analysis checks and Windows WDK Release x64 build, Driver Code Analysis, InfVerif and Inf2Cat/package validation. The release package is development/test signed. Runtime Driver Verifier and destructive mounted-volume Windows qualification remain manual work for a disposable Windows VM and test volume; this is not a production driver.

Release assets intentionally contain only the experimental x64 setup executable and its SHA-256 checksum. GitHub supplies the canonical source archives for the tag automatically; no duplicate source ZIP or patch asset is published.


Assets:

- `ExtFS-for-Windows-0.9.2-experimental-x64-setup.exe` — 157621 bytes; sha256:e22406bc42a8b517e9ce98abf42a2647047d45ba1d04ec60f68919a4ef8e8849
- `SHA256SUMS.txt` — 118 bytes; sha256:c31ebe2512b8a19f8c652d1c25d4b1778473747ccc1dba3a87b86ca23c5d131d

## v0.9.3 — ExtFS for Windows 0.9.3

- Target: `d954421c022626ad93c52de75785d8bdaf6d21e7`
- Published: 2026-08-21T01:04:04Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.3

﻿ExtFS for Windows 0.9.3 is the Windows-lifecycle/Common-1.9 maintenance release on the 0.9.2 audit-hardened filesystem boundary.

Changes in this release:
- supplies the required FSRTL advanced FCB header for every Windows file object;
- makes Memory Manager authoritative for mapped-section closure before FCB reclamation or removable-media refresh;
- resynchronises removable-media read-only state;
- pins and verifies driver/package metadata at 0.9.3.0;
- bounds headless signature verification so packaging cannot hang indefinitely;
- pins Infiltratr Common 1.9.0 at a65b279b40682791f2cfefb4d9bdc274790b0c77.

This installer is development/test signed and remains experimental. Use it only on disposable/test Windows systems and backed-up test volumes.

Installer SHA-256: 8313C55AD62F9B601EBA0A4109B952D6633BAE996C0F0B6F015801B10D42148B


Assets:

- `ExtFS-for-Windows-0.9.3-experimental-x64-setup.exe` — 158388 bytes; sha256:8313c55ad62f9b601eba0a4109b952d6633bae996c0f0b6f015801b10d42148b

## v0.9.3-arm64-test.1 — ExtFS for Windows 0.9.3 ARM64 test preview 1

- Target: `789128996d11b76acaaf8446567805c53f05e9bd`
- Published: 2026-08-21T04:32:36Z
- Prerelease: true
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.3-arm64-test.1

﻿ExtFS for Windows 0.9.3 ARM64 test preview 1 is a test-only ARM64 build of the existing bounded 0.9.3 filesystem checkpoint.

This is not a production release. It is development/test signed and requires a disposable ARM64 Windows test system with Secure Boot disabled, TESTSIGNING enabled, a complete backup, and a disposable external ext test volume. Begin with read-only access. Do not use it on important data.

The installer, driver PE header, INF catalog membership and ARM64 package metadata were validated by the WDK pipeline.

Installer SHA-256: 605F7C9087F6EEDD87947DB709F59B1AAA431B5970F51DD5BA020C371FB1FBFF


Assets:

- `ExtFS-for-Windows-0.9.3-experimental-arm64-setup.exe` — 161954 bytes; sha256:605f7c9087f6eedd87947db709f59b1aaa431b5970f51dd5ba020c371fb1fbff

## v0.9.4 — ExtFS for Windows 0.9.4

- Target: `ba07fbdcd03a2f21ced86b2ed47a1b658aa85836`
- Published: 2026-08-22T11:15:15Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.4

﻿ExtFS for Windows 0.9.4 is an installer-only hotfix for the 0.9.3 driver release.

Fixed:
- corrected the malformed filesystem-service ImagePath that caused CreateService to succeed but StartService to fail with exit 6;
- service ImagePath is now exactly \SystemRoot\System32\drivers\extfs.sys;
- the installer validates ImagePath, filesystem service type and demand-start mode in the registry before loading;
- the File System load-order group is restored;
- failed driver loads now include recent Service Control Manager and Code Integrity diagnostics;
- CI now has a regression check for the service-path contract.

Driver payload: unchanged, already-qualified ExtFS 0.9.3.0 driver.
Shared dependency: Infiltratr Common 1.9.0 pinned at a65b279b40682791f2cfefb4d9bdc274790b0c77.

Development/test-signed experimental kernel driver. Use only on disposable/test Windows systems and backed-up test volumes.

Installer SHA-256: 46A6A2470877E1655CEE25C35C60B2C2926967B84A254060CA34659907C307B8


Assets:

- `ExtFS-for-Windows-0.9.4-experimental-x64-setup.exe` — 164003 bytes; sha256:46a6a2470877e1655cee25c35c60b2c2926967b84a254060ca34659907c307b8

## v0.9.5 — ExtFS for Windows 0.9.5

- Target: `36fb8f15d2b38badacc7648971f48095ff894e76`
- Published: 2026-08-22T11:53:59Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.5

﻿ExtFS for Windows 0.9.5 is an installer-only architecture-detection hotfix carrying the already-qualified ExtFS 0.9.3.0 filesystem driver.

Fixed:
- removes the broken machine-scoped PROCESSOR_ARCHITECTURE check that falsely rejected genuine x64 Windows;
- uses RuntimeInformation.OSArchitecture with a WOW64-safe fallback;
- independently verifies the bundled driver PE machine before installation;
- retains the 0.9.4 service ImagePath and service-contract fixes;
- retains Infiltratr Common 1.9.0.

Installer SHA-256: D2930581BAC7D212D4EE4C9CD65E227E43A2161FF7A138E85FB4824CB3A5E5EC


Assets:

- `ExtFS-for-Windows-0.9.5-experimental-x64-setup.exe` — 164145 bytes; sha256:868b3ff0fdbb64a1bab2bf6023e6782000b4101d463d5031786f721f8d906ccd

## v0.9.6 — ExtFS for Windows 0.9.6

- Target: `c668efe326e5cd3af088e99422f5015574e76fa2`
- Published: 2026-09-03T05:17:53Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.6

Automatic release from tested exact current main.

Assets:

- `ExtFS-for-Windows-0.9.6-experimental-arm64-setup.exe` — 169929 bytes; sha256:cfd8c7c454e27d144347fbccfa4398817477c25d539a7f63ef9e5047afc30eb1
- `ExtFS-for-Windows-0.9.6-experimental-x64-setup.exe` — 171903 bytes; sha256:cadb1f7360f7f372ede855294073af7aa4f7253c776dfbe829f1c5297da59172
- `ExtFS-for-Windows-0.9.6-source.zip` — 171729 bytes; sha256:c36fdd01924cae5a57eb535dab006698ae485ae73a9a5c909ecaf34ba4d297ad
- `SHA256SUMS.txt` — 340 bytes; sha256:84ad4b79253b04ac9676f47f4e6fecaafe9d1811e1cd75d36d561383cd732836

## v0.9.7 — ExtFS for Windows 0.9.7

- Target: `a54377e9c2d7e8c07da9727437a8119afa9ee155`
- Published: 2026-09-18T09:45:39Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.7

Automatic release from tested exact current main.

Assets:

- `ExtFS-for-Windows-0.9.7-experimental-arm64-setup.exe` — 169928 bytes; sha256:d2ebfc09e40101916f9133688ce0bd43baedf59e878e9d1ef745beef02e6c908
- `ExtFS-for-Windows-0.9.7-experimental-x64-setup.exe` — 171903 bytes; sha256:98d404f816ccae806b57919a7355c61a4fa592fd646c713c876f1d4730a6488c
- `ExtFS-for-Windows-0.9.7-source.zip` — 171754 bytes; sha256:ffbc7919b65bc15c958065144e301498a6179be3c1f14f6d1f011c2ccc0f8a4b
- `SHA256SUMS.txt` — 340 bytes; sha256:3dd457dcc4c46c3f2354603a6fabafc79283c692cc6f9fb4af1cd3afe768e474

## v0.9.8 — Ext Filesystem Driver 0.9.8

- Target: `ab6478a857636364779a1d1037c8f9922e7b7000`
- Published: 2026-09-18T10:39:12Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.8

Automatic release from tested exact current main.

Assets:

- `ExtFS-for-Windows-0.9.8-experimental-arm64-setup.exe` — 169928 bytes; sha256:03c7b39ef35000976813e2a6ca220bf81d85831462c9a3b5df0c425f81c63aad
- `ExtFS-for-Windows-0.9.8-experimental-x64-setup.exe` — 171900 bytes; sha256:4efa3b8be68b5036e0068d6cf4e63ca3fbcc11976897e80072af18b066bbbe4a
- `ExtFS-for-Windows-0.9.8-source.zip` — 171775 bytes; sha256:29c3b936ba68e9c41735f3e29480beb81b138801a89065dbf35fb6320ae9e925
- `SHA256SUMS.txt` — 340 bytes; sha256:00788ff10428f212e5dbaf2905cab45d6046d6fadbacd4dceea921243ef73113

## v0.9.9 — Ext Filesystem Driver 0.9.9

- Target: `dda219be47033372217149bce8c8ee20f19ded0a`
- Published: 2026-09-22T23:40:55Z
- Prerelease: false
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/releases/tag/v0.9.9

Automatic release from tested exact current main.

Assets:

- `ExtFS-for-Windows-0.9.9-experimental-arm64-setup.exe` — 169925 bytes; sha256:39c6016d271937519de6d65e4993cd88b7b25dcba9ec51d2b140af35fb04d2e9
- `ExtFS-for-Windows-0.9.9-experimental-x64-setup.exe` — 171912 bytes; sha256:7f2ca25069a4206f7d5a8d5d7f7261c6271a19c75bffcf036a564d8c0f1fe19e
- `ExtFS-for-Windows-0.9.9-source.zip` — 177568 bytes; sha256:b3dfdca4033e79b74cd7ca36b82f182408a05f1a1491e5372f48e9da3bbbd157
- `SHA256SUMS.txt` — 340 bytes; sha256:124c71c80feecc6a4142376c0fd326f2de4bf0709eade5e8aaf7393e39918c62

