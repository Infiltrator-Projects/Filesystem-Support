# ExtFS-for-Windows repository history archive

This engineering-provenance snapshot was captured before the standalone repository is retired into Filesystem Support.

- Repository baseline: `Infiltrator-Projects/ExtFS-for-Windows`
- Migration baseline: `dda219be47033372217149bce8c8ee20f19ded0a`
- Source tree: `76b127deaa8b5b4776774a9579ca6c791d9d6b38`
- Frozen final source: `archive/extfs-for-windows-v0.9.9/`

## Releases

### v0.9.9 — Ext Filesystem Driver 0.9.9

Published: 2026-09-22T23:40:55Z.

Automatic release from tested exact current main.

Assets:
- `ExtFS-for-Windows-0.9.9-experimental-arm64-setup.exe` — 169925 bytes — recorded downloads 0
- `ExtFS-for-Windows-0.9.9-experimental-x64-setup.exe` — 171912 bytes — recorded downloads 0
- `ExtFS-for-Windows-0.9.9-source.zip` — 177568 bytes — recorded downloads 0
- `SHA256SUMS.txt` — 340 bytes — recorded downloads 0

### v0.9.8 — Ext Filesystem Driver 0.9.8

Published: 2026-09-18T10:39:12Z.

Automatic release from tested exact current main.

Assets:
- `ExtFS-for-Windows-0.9.8-experimental-arm64-setup.exe` — 169928 bytes — recorded downloads 0
- `ExtFS-for-Windows-0.9.8-experimental-x64-setup.exe` — 171900 bytes — recorded downloads 1
- `ExtFS-for-Windows-0.9.8-source.zip` — 171775 bytes — recorded downloads 0
- `SHA256SUMS.txt` — 340 bytes — recorded downloads 0

### v0.9.7 — ExtFS for Windows 0.9.7

Published: 2026-09-18T09:45:39Z.

Automatic release from tested exact current main.

Assets:
- `ExtFS-for-Windows-0.9.7-experimental-arm64-setup.exe` — 169928 bytes — recorded downloads 0
- `ExtFS-for-Windows-0.9.7-experimental-x64-setup.exe` — 171903 bytes — recorded downloads 0
- `ExtFS-for-Windows-0.9.7-source.zip` — 171754 bytes — recorded downloads 0
- `SHA256SUMS.txt` — 340 bytes — recorded downloads 0

### v0.9.6 — ExtFS for Windows 0.9.6

Published: 2026-09-03T05:17:53Z.

Automatic release from tested exact current main.

Assets:
- `ExtFS-for-Windows-0.9.6-experimental-arm64-setup.exe` — 169929 bytes — recorded downloads 1
- `ExtFS-for-Windows-0.9.6-experimental-x64-setup.exe` — 171903 bytes — recorded downloads 2
- `ExtFS-for-Windows-0.9.6-source.zip` — 171729 bytes — recorded downloads 1
- `SHA256SUMS.txt` — 340 bytes — recorded downloads 0

### v0.9.5 — ExtFS for Windows 0.9.5

Published: 2026-08-22T11:53:59Z.

ExtFS for Windows 0.9.5 is an installer-only architecture-detection hotfix carrying the already-qualified ExtFS 0.9.3.0 filesystem driver.

Fixed:
- removes the broken machine-scoped PROCESSOR_ARCHITECTURE check that falsely rejected genuine x64 Windows;
- uses RuntimeInformation.OSArchitecture with a WOW64-safe fallback;
- independently verifies the bundled driver PE machine before installation;
- retains the 0.9.4 service ImagePath and service-contract fixes;
- retains Infiltratr Common 1.9.0.

Installer SHA-256: D2930581BAC7D212D4EE4C9CD65E227E43A2161FF7A138E85FB4824CB3A5E5EC

Assets:
- `ExtFS-for-Windows-0.9.5-experimental-x64-setup.exe` — 164145 bytes — recorded downloads 2

### v0.9.4 — ExtFS for Windows 0.9.4

Published: 2026-08-22T11:15:15Z.

ExtFS for Windows 0.9.4 is an installer-only hotfix for the 0.9.3 driver release.

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
- `ExtFS-for-Windows-0.9.4-experimental-x64-setup.exe` — 164003 bytes — recorded downloads 1

### v0.9.3 — ExtFS for Windows 0.9.3

Published: 2026-08-21T01:04:04Z.

ExtFS for Windows 0.9.3 is the Windows-lifecycle/Common-1.9 maintenance release on the 0.9.2 audit-hardened filesystem boundary.

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
- `ExtFS-for-Windows-0.9.3-experimental-x64-setup.exe` — 158388 bytes — recorded downloads 1

### v0.9.3-arm64-test.1 — ExtFS for Windows 0.9.3 ARM64 test preview 1

Published: 2026-08-21T04:32:36Z (prerelease).

ExtFS for Windows 0.9.3 ARM64 test preview 1 is a test-only ARM64 build of the existing bounded 0.9.3 filesystem checkpoint.

This is not a production release. It is development/test signed and requires a disposable ARM64 Windows test system with Secure Boot disabled, TESTSIGNING enabled, a complete backup, and a disposable external ext test volume. Begin with read-only access. Do not use it on important data.

The installer, driver PE header, INF catalog membership and ARM64 package metadata were validated by the WDK pipeline.

Installer SHA-256: 605F7C9087F6EEDD87947DB709F59B1AAA431B5970F51DD5BA020C371FB1FBFF

Assets:
- `ExtFS-for-Windows-0.9.3-experimental-arm64-setup.exe` — 161954 bytes — recorded downloads 0

### v0.9.2 — ExtFS for Windows 0.9.2

Published: 2026-08-19T11:38:04Z.

ExtFS for Windows 0.9.2 is a forensic-audit hardening and qualification checkpoint for the bounded 0.9 filesystem feature set.

Hardening includes an ext2 durability-capability preflight that refuses metadata resize before any write when the host cannot provide a stable-storage barrier, plus explicit dirty/mutation/clean flush ordering. It also adds destructive real-image ext2/ext3/ext4 grow/shrink/reopen/e2fsck qualification, expanded sanitizer/static-analysis CI, native Windows WDK Release x64 qualification, and reversible Windows append/EOF-resize test probes.

The filesystem feature boundary is intentionally not broadened. Namespace mutation, paging writes, volume lock ownership, dirty-journal replay, deeper/multi-leaf ext4 extent trees and 64-bit/flex_bg allocation remain outside this checkpoint. Persistent regular-file mtime/ctime mutation remains deferred.

Automated qualification passed on 18 August 2026: portable GCC/Clang/unit/real-image/sanitizer/static-analysis checks and Windows WDK Release x64 build, Driver Code Analysis, InfVerif and Inf2Cat/package validation. The release package is development/test signed. Runtime Driver Verifier and destructive mounted-volume Windows qualification remain manual work for a disposable Windows VM and test volume; this is not a production driver.

Release assets intentionally contain only the experimental x64 setup executable and its SHA-256 checksum. GitHub supplies the canonical source archives for the tag automatically; no duplicate source ZIP or patch asset is published.

Assets:
- `ExtFS-for-Windows-0.9.2-experimental-x64-setup.exe` — 157621 bytes — recorded downloads 1
- `SHA256SUMS.txt` — 118 bytes — recorded downloads 0

### v0.9.1 — ExtFS for Windows 0.9.1

Published: 2026-08-12T07:52:40Z.

Licensing-standardisation maintenance release.

- Project licensing standardised on GPL-3.0-or-later.
- SPDX identifiers added throughout source, build configuration and documentation.
- Functional baseline remains the 0.9 bounded EXT4 external extent-tree mutation checkpoint.
- Read support for supported EXT2/EXT3/EXT4 remains available.
- Existing bounded write/resize support remains experimental and deliberately fail-closed for unsupported layouts.

This is experimental filesystem-driver software and is not production-qualified.

Assets:
- `BUILD-EXTFS-ONE-CLICK-V17.cmd` — 23392 bytes — recorded downloads 1

## Issues

### #16 — [HAL] Run ExtFS pre-MS hardening

State: closed. Created: 2026-08-22T13:32:25Z. Closed: 2026-08-22T13:34:53Z.

Superseded before execution because the initial qualification workflow YAML was syntactically invalid. No source hardening ran from this trigger. Replaced by a fresh trigger after correcting and validating the workflow syntax.


### #17 — [HAL] Run ExtFS pre-MS hardening

State: closed. Created: 2026-08-22T13:35:02Z. Closed: 2026-08-22T13:36:58Z.

The gate triggered correctly but stopped before publishing any filesystem source change. The initial failure report was too terse to identify the failing stage. Superseded by an instrumented rerun that records stage outcomes and build/test log tails.

- Comment by github-actions[bot] (): Pre-MS hardening started on main at a5fc64db4a75befeb1c9e09e53cfdd55bd8d72c1. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.
- Comment by github-actions[bot] (): Pre-MS hardening stopped before publication: the source transformation or portable qualification failed. No hardening source commit was pushed by this run.

### #18 — [HAL] Run ExtFS pre-MS hardening

State: closed. Created: 2026-08-22T13:37:07Z. Closed: 2026-08-22T13:39:43Z.

Instrumented run identified the exact source-transformer failure: the write and FileEndOfFile set-information handlers have different guard layouts, while the transformer incorrectly required two identical anchors. No filesystem source change was published. Transformer corrected with separate exact anchors and additional dismounted guards for query-volume/device-control paths.

- Comment by github-actions[bot] (): Pre-MS hardening started on main at f30c33f6a551c59b7efd0e68beeff150cc226ed2. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.
- Comment by github-actions[bot] (): Pre-MS hardening stopped before publication. No hardening source commit was pushed by this run.
  
  Stage outcomes: transform=failure, docs=skipped, qualify=skipped, publish=skipped
  
  --- extfs-transform.log tail ---
  write/set-information guard anchors missing

### #19 — [HAL] Run ExtFS pre-MS hardening

State: closed. Created: 2026-08-22T13:39:52Z. Closed: 2026-08-22T13:55:13Z.

Portable/real-image/contracts/cross-build all passed, but this run used the old direct-main publication step and correctly failed at publication. Superseded by the detached-candidate gate, which keeps main untouched until x64 and ARM64 WDK both pass.

- Comment by github-actions[bot] (): Pre-MS hardening started on main at 0c3566a62e69df4eb485c6c729518350e05ad4a4. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.
- Comment by github-actions[bot] (): Pre-MS hardening stopped before publication. No hardening source commit was pushed by this run.
  
  Stage outcomes: transform=success, docs=success, qualify=success, publish=failure
  
  --- extfs-transform.log tail ---
  
  --- extfs-qualification.log tail ---
  e2fsprogs set to manually installed.
  make is already the newest version (4.3-4.1build2).
  gcc is already the newest version (4:13.2.0-7ubuntu1).
  The following additional packages will be installed:
    gcc-mingw-w64-base gcc-mingw-w64-x86-64-posix
    gcc-mingw-w64-x86-64-posix-runtime gcc-mingw-w64-x86-64-win32
    gcc-mingw-w64-x86-64-win32-runtime mingw-w64-common mingw-w64-x86-64-dev
  Suggested packages:
    gcc-13-locales wine64
  The following NEW packages will be installed:
    binutils-mingw-w64-x86-64 gcc-mingw-w64-base gcc-mingw-w64-x86-64
    gcc-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix-runtime
    gcc-mingw-w64-x86-64-win32 gcc-mingw-w64-x86-64-win32-runtime
    mingw-w64-common mingw-w64-x86-64-dev
  0 upgraded, 9 newly installed, 0 to remove and 28 not upgraded.
  Need to get 115 MB of archives.
  After this operation, 587 MB of additional disk space will be used.
  Get:1 file:/etc/apt/apt-mirrors.txt Mirrorlist [144 B]
  Get:2 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 binutils-mingw-w64-x86-64 amd64 2.41.90.20240122-1ubuntu1+11.4 [6053 kB]
  Get:3 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 mingw-w64-common all 11.0.1-3build1 [5580 kB]
  Get:4 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 mingw-w64-x86-64-dev all 11.0.1-3build1 [3786 kB]
  Get:5 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-base amd64 13.2.0-6ubuntu1+26.1 [188 kB]
  Get:6 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-posix-runtime amd64 13.2.0-6ubuntu1+26.1 [14.0 MB]
  Get:7 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-posix amd64 13.2.0-6ubuntu1+26.1 [35.4 MB]
  Get:8 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-win32-runtime amd64 13.2.0-6ubuntu1+26.1 [14.2 MB]
  Get:9 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-win32 amd64 13.2.0-6ubuntu1+26.1 [35.4 MB]
  Get:10 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64 all 13.2.0-6ubuntu1+26.1 [187 kB]
  Fetched 115 MB in 1s (85.2 MB/s)
  Selecting previously unselected package binutils-mingw-w64-x86-64.
  (Reading database ... (Reading database ... 5%(Reading database ... 10%(Reading database ... 15%(Reading database ... 20%(Reading database ... 25%(Reading database ... 30%(Reading database ... 35%(Reading database ... 40%(Reading database ... 45%(Reading database ... 50%(Reading database ... 55%(Reading database ... 60%(Reading database ... 65%(Reading database ... 70%(Reading database ... 75%(Reading database ... 80%(Reading database ... 85%(Reading database ... 90%(Reading database ... 95%(Reading database ... 100%(Reading database ... 203124 files and directories currently installed.)
  Preparing to unpack .../0-binutils-mingw-w64-x86-64_2.41.90.20240122-1ubuntu1+11.4_amd64.deb ...
  Unpacking binutils-mingw-w64-x86-64 (2.41.90.20240122-1ubuntu1+11.4) ...
  Selecting previously unselected package mingw-w64-common.
  Preparing to unpack .../1-mingw-w64-common_11.0.1-3build1_all.deb ...
  Unpacking mingw-w64-common (11.0.1-3build1) ...
  Selecting previously unselected package mingw-w64-x86-64-dev.
  Preparing to unpack .../2-mingw-w64-x86-64-dev_11.0.1-3build1_all.deb ...
  Unpacking mingw-w64-x86-64-dev (11.0.1-3build1) ...
  Selecting previously unselected package gcc-mingw-w64-base:amd64.
  Preparing to unpack .../3-gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb ...
  Unpacking gcc-mingw-w64-base:amd64 (13.2.0-6ubuntu1+26.1) ...
  Selecting previously unselected package gcc-mingw-w64-x86-64-posix-runtime.
  Preparing to unpack .../4-gcc-mingw-w64-x86-64-posix-runtime_13.2.0-6ubuntu1+26.1_amd64.deb ...
  Unpacking gcc-mingw-w64-x86-64-posix-runtime (13.2.0-6ubuntu1+26.1) ...
  Selecting previously unselected package gcc-mingw-w64-x86-64-posix.
  Preparing to unpack .../5-gcc-mingw-w64-x86-64-posix_13.2.0-6ubuntu1+26.1_amd64.deb ...
  Unpacking gcc-mingw-w64-x86-64-posix (13.2.0-6ubuntu1+26.1) ...
  Selecting previously unselected package gcc-mingw-w64-x86-64-win32-runtime.
  Preparing to unpack .../6-gcc-mingw-w64-x86-64-win32-runtime_13.2.0-6ubuntu1+26.1_amd64.deb ...
  Unpacking gcc-mingw-w64-x86-64-win32-runtime (13.2.0-6ubuntu1+26.1) ...
  Selecting previously unselected package gcc-mingw-w64-x86-64-win32.
  Preparing to unpack .../7-gcc-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb ...
  Unpacking gcc-mingw-w64-x86-64-win32 (13.2.0-6ubuntu1+26.1) ...
  Selecting previously unselected package gcc-mingw-w64-x86-64.
  Preparing to unpack .../8-gcc-mingw-w64-x86-64_13.2.0-6ubuntu1+26.1_all.deb ...
  Unpacking gcc-mingw-w64-x86-64 (13.2.0-6ubuntu1+26.1) ...
  Setting up binutils-mingw-w64-x86-64 (2.41.90.20240122-1ubuntu1+11.4) ...
  Setting up gcc-mingw-w64-base:amd64 (13.2.0-6ubuntu1+26.1) ...
  Setting up gcc-mingw-w64-x86-64-win32-runtime (13.2.0-6ubuntu1+26.1) ...
  Setting up mingw-w64-common (11.0.1-3build1) ...
  Setting up mingw-w64-x86-64-dev (11.0.1-3build1) ...
  Setting up gcc-mingw-w64-x86-64-posix-runtime (13.2.0-6ubuntu1+26.1) ...
  Setting up gcc-mingw-w64-x86-64-posix (13.2.0-6ubuntu1+26.1) ...
  update-alternatives: using /usr/bin/x86_64-w64-mingw32-gcc-posix to provide /usr/bin/x86_64-w64-mingw32-gcc (x86_64-w64-mingw32-gcc) in auto mode
  Setting up gcc-mingw-w64-x86-64-win32 (13.2.0-6ubuntu1+26.1) ...
  update-alternatives: using /usr/bin/x86_64-w64-mingw32-gcc-win32 to provide /usr/bin/x86_64-w64-mingw32-gcc (x86_64-w64-mingw32-gcc) in auto mode
  Setting up gcc-mingw-w64-x86-64 (13.2.0-6ubuntu1+26.1) ...
  Processing triggers for man-db (2.12.0-4build2) ...
  Not building database; man-db/auto-update is not 'true'.
  
  Running kernel seems to be up-to-date.
  
  No services need to be restarted.
  
  No containers need to be restarted.
  
  No user sessions are running outdated binaries.
  
  No VM guests are running outdated hypervisor (qemu) binaries on this host.
  rm -rf build
  mkdir -p build
  cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c core/extfs.c -o build/extfs.o
  ar rcs build/libextfs.a build/extfs.o
  cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tools/extfs-tool.c -o build/extfs-tool.o
  cc -O2  build/extfs-tool.o build/libextfs.a  -o build/extfs-tool
  cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tests/test_extfs.c -o build/test-extfs.o
  cc -O2  build/test-extfs.o build/libextfs.a  -o build/test-extfs
  cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tests/mutate_image.c -o build/mutate_image.o
  cc -O2  build/mutate_image.o build/libextfs.a  -o build/extfs-mutate-test
  All ExtFS unit tests passed.
  sh tests/integration.sh
  ext2 /hello.txt resized to 4096 bytes and reopened successfully
  ext2 /hello.txt resized to 7 bytes and reopened successfully
  ext3 /hello.txt resized to 4096 bytes and reopened successfully
  ext3 /hello.txt resized to 7 bytes and reopened successfully
  ext4 /hello.txt resized to 4096 bytes and reopened successfully
  ext4 /hello.txt resized to 7 bytes and reopened successfully
  Ext2, ext3 and ext4 read/traversal and real-image resize tests passed.
  Filesystem lifecycle/name/version contracts: PASS
  /home/runner/work/ExtFS-for-Windows/ExtFS-for-Windows/build-cross/extfs.sys

### #20 — [HAL] Run ExtFS pre-MS hardening

State: closed. Created: 2026-08-22T13:55:28Z. Closed: 2026-08-22T14:04:55Z.

Superseded by the simplified v3 issue-triggered gate. The push-triggered route is not reliable for connector-created commits in this repository. No filesystem source promotion occurred from this issue.


### #21 — [HAL] Run ExtFS pre-MS v3

State: closed. Created: 2026-08-22T14:05:07Z. Closed: 2026-08-22T23:42:52Z.

Run the simplified detached pre-Microsoft qualification gate. Apply the audited lifecycle/name correctness fixes, portable unit tests, real ext2/ext3/ext4 mke2fs/e2fsck mutation qualification, filesystem contracts and MinGW cross-build; then validate the exact patch through WDK Release x64 and ARM64. Do not move main from the workflow. Report the detached candidate SHA only after full PASS.


### #22 — [HAL] Pre-MS ping

State: closed. Created: 2026-08-22T14:06:35Z. Closed: 2026-08-22T23:42:56Z.

One-shot Actions issue-dispatch probe. No source changes.


### #23 — [HAL] Pre-MS ping

State: closed. Created: 2026-08-22T14:07:06Z. Closed: 2026-08-22T23:43:00Z.

Second issue-dispatch probe after allowing the workflow to register on the default branch. No source changes.


## Pull requests

### #1 — Harden and qualify ExtFS 0.9.2

State: closed. Created: 2026-08-18T10:06:58Z. Closed: 2026-08-18T10:38:14Z. Merged: .

Forensic-audit hardening checkpoint before further filesystem feature work.

Scope completed:
- preserved the bounded 0.9 filesystem feature boundary while versioning the post-0.9.1 durability work as 0.9.2;
- added ext2 durability capability preflight and explicit crash-consistency barriers;
- added destructive real mke2fs ext2/ext3/ext4 grow/shrink/reopen/e2fsck qualification;
- expanded sanitizer/static-analysis CI;
- added Windows WDK Release x64 / Driver Code Analysis / InfVerif / Inf2Cat CI;
- added reversible Windows append/EOF-resize qualification probes;
- refreshed stale version and feature descriptions.

Final branch head passed both permanent CI gates. The 47-step audit-development history was intentionally not merged. Its tested tree was squashed directly onto main as clean commit `40eaa6db663f91a8ad343e07dab06362d71e32d5`. This PR is therefore closed as integrated-by-squash.


### #2 — Publish verified ExtFS 0.9.2

State: closed. Created: 2026-08-18T10:48:13Z. Closed: 2026-08-19T11:39:25Z. Merged: .

Release-only lane completed successfully.

Run #7 (`32248121863`) revalidated the exact `main` release source, passed the portable and real-image qualification, rebuilt/analyzed/validated the Windows driver package, test-signed the SYS/CAT deterministically without the headless trust-chain hang, built and hashed the NSIS installer, published `v0.9.2`, verified the exact two explicit release assets, and removed all release/audit working branches.

This PR was never intended to be merged; it is closed after successful publication.


### #3 — Apply ExtFS forensic fixes and Common 1.9 pin

State: closed. Created: 2026-08-20T01:10:25Z. Closed: 2026-08-20T01:33:45Z. Merged: 2026-08-20T01:33:45Z.

Temporary validation PR for the forensic maintenance pass. The repair runner applies three concrete Windows-driver fixes, pins Infiltratr Common 1.9.0 exactly, runs portable/unit/real-image qualification and a freestanding Windows cross-build, then publishes one clean commit to main only if all checks succeed. The temporary workflow/runner are removed from the resulting main commit.


### #4 — Release ExtFS 0.9.3 maintenance package

State: closed. Created: 2026-08-20T01:41:39Z. Closed: 2026-08-20T01:48:17Z. Merged: 2026-08-20T01:48:17Z.

Publishes the already-audited Windows lifecycle/Common 1.9 maintenance changes as ExtFS 0.9.3 instead of mutating the historical 0.9.2 release. This PR updates authoritative binary/INF/installer/docs metadata to 0.9.3, keeps the filesystem mutation boundary unchanged, removes the stale hardening branch trigger from WDK CI, and adds an automated release publisher. On the final main release commit it builds and test-signs the x64 installer, verifies version/SHA/release asset metadata, publishes GitHub Release v0.9.3, and deletes the merged forensic-fix-common19 branch so main is the only branch. No duplicate custom source ZIP or patch asset is created.


### #5 — Trigger verified ExtFS 0.9.3 publication

State: closed. Created: 2026-08-20T02:09:21Z. Closed: 2026-08-20T22:48:00Z. Merged: .

Superseded by the controlled ExtFS 0.9.3 recovery. This PR's force-publish workflow is obsolete and must not be merged.


### #6 — Fix hung ExtFS 0.9.3 publisher

State: closed. Created: 2026-08-20T03:52:37Z. Closed: 2026-08-20T03:52:53Z. Merged: 2026-08-20T03:52:53Z.

Replaces the stalled 0.9.3 rescue packaging path with a bounded fast publisher. It builds the exact already-qualified release commit d7b43df5c672927b767e2ef63a58b724f23c61ad, verifies Common 1.9.0, skips only the redundant second Code Analysis pass during packaging, publishes/repairs v0.9.3 with exactly one installer asset, and removes the temporary release branches after successful verification.


### #7 — Make ExtFS 0.9.3 publisher observable

State: closed. Created: 2026-08-20T04:00:02Z. Closed: 2026-08-20T04:00:16Z. Merged: 2026-08-20T04:00:16Z.

Adds a PR trigger to the fast 0.9.3 publisher so the packaging job can be inspected directly through GitHub Actions while retaining the main-push trigger. The release source remains pinned to the already-qualified 0.9.3 commit and temporary release branches are removed after successful publication.


### #8 — Recover ExtFS 0.9.3 Windows lifecycle and publication

State: closed. Created: 2026-08-20T23:03:23Z. Closed: 2026-08-20T23:44:48Z. Merged: 2026-08-20T23:44:48Z.

## Recovery scope

- replace the invalid FILE_OBJECT FsContext layout with an FSRTL advanced FCB header;
- make Memory Manager authoritative for mapped-section closure before FCB reclamation and removable-media refresh;
- keep FCB header sizes synchronized after supported resize operations;
- pin and assert the staged INF at DriverVer 0.9.3.0;
- sign the final SYS before catalog generation, then sign the catalog and inspect both signatures/catalog membership in bounded workers without mutating Windows trust stores;
- make Windows CI build the complete signed installer under explicit time limits;
- remove the broad rescue publisher and retain one exact-message, post-merge release trigger;
- update the Windows qualification probe and documentation.

This intentionally does not expand the filesystem mutation boundary. Hosted CI validates build, analysis, package metadata, signing and installer assembly; installed-driver/Driver Verifier testing remains a manual disposable-VM qualification step.


### #9 — Fix v0.9.3 publisher absence preflight

State: closed. Created: 2026-08-21T00:51:02Z. Closed: 2026-08-21T00:59:21Z. Merged: 2026-08-21T00:59:21Z.

The guarded publisher correctly refused to continue after PowerShell promoted GitHub CLI's expected `release not found` stderr to a terminating error. Replace that native-process probe with an authenticated GitHub REST lookup that explicitly accepts only HTTP 404 as the absent-release state. All other HTTP failures remain fatal, and an existing v0.9.3 still blocks publication.


### #10 — Make v0.9.3 recovery cleanup idempotent

State: closed. Created: 2026-08-21T01:07:32Z. Closed: 2026-08-21T01:12:37Z. Merged: 2026-08-21T01:12:37Z.

The v0.9.3 release and installer were published and verified, but the publisher's final housekeeping step stopped because obsolete PR #5 was already closed. Separate branch cleanup into a guarded, idempotent job that first verifies the final release asset, accepts only missing branches as harmless, and deletes the stale recovery branches after a one-time finalize merge.


### #11 — Add ARM64 test package and guarded transition workflow

State: closed. Created: 2026-08-21T03:52:21Z. Closed: 2026-08-21T04:16:36Z. Merged: 2026-08-21T04:16:36Z.

## Summary

- add x64 and ARM64 WDK project/package configurations
- validate the driver PE machine type and architecture-specific INF catalog targets
- build separately named x64 and ARM64 experimental installers
- refuse host/driver architecture mismatches, pending restarts and unsafe Secure Boot state
- add read-only host readiness inventory and a reversible ARM64/Paragon transition guide
- add a separately labelled ARM64 test prerelease workflow; the existing v0.9.3 x64 release is untouched

## Safety boundary

This is test readiness, not production readiness. The ARM64 artifact remains development/test signed and is intended only for a disposable Windows test system and a disposable, backed-up ext volume.

## Validation

- [x] x64 WDK build, code analysis, INF/catalog/signature verification and installer assembly
- [x] ARM64 WDK build, code analysis, PE `0xAA64`, INF/catalog/signature verification and installer assembly
- [x] portable core CI remains green

Validated on commit `de6ad8a80c69e2f1fa355795b234e50ca6c8b4a6`:

- Windows WDK package CI run 100: success (x64 and ARM64)
- Portable ExtFS CI run 102: success

Related host audit: the target Windows PC is native ARM64, has a pending restart, and currently exposes no unambiguous Paragon/UFSD uninstall entry or filesystem service. Existing software must remain installed until the replacement artifact is qualified and the exact product is identified.


### #12 — Fix ARM64 prerelease absence check

State: closed. Created: 2026-08-21T04:19:03Z. Closed: 2026-08-21T04:25:19Z. Merged: 2026-08-21T04:25:19Z.

## Summary

Replace the GitHub CLI absence probe with an explicit authenticated REST check that accepts only HTTP 404 as the expected not-yet-published state.

The failed publication run stopped at this preflight before building or creating a release. The validated ARM64 and x64 package code is unchanged.

## Validation

- [x] Portable ExtFS CI run 104
- [x] x64 WDK package CI run 102
- [x] ARM64 WDK package CI run 102


### #13 — Fix ExtFS 0.9.3 installer StartService failure

State: closed. Created: 2026-08-22T11:03:02Z. Closed: 2026-08-22T11:08:59Z. Merged: 2026-08-22T11:08:59Z.

Fixes the reproducible installer failure where CreateService succeeds but StartService returns exit 6 because Install-ExtFS.ps1 wrote a doubled-backslash NT service ImagePath. Adds registry-level service contract validation and diagnostics, packages the installer-only hotfix as 0.9.4 while retaining the already-qualified 0.9.3.0 driver payload, adds a regression test, updates WDK package CI, and prepares verified v0.9.4 publication plus cleanup of temporary branches.

- Comment/review by chatgpt-codex-connector[bot] (): You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

### #14 — Trigger verified ExtFS 0.9.4 artifact publication

State: closed. Created: 2026-08-22T11:13:15Z. Closed: 2026-08-22T11:15:17Z. Merged: .

One-shot publication trigger only. Uses the already-green x64 WDK/package artifact from run 32569243508, verifies installer 0.9.4.0 and the unchanged 0.9.3.0 driver payload, publishes/verifies v0.9.4, then closes this PR and removes all temporary branches. This PR is not intended to merge.


### #15 — Publish verified ExtFS 0.9.5

State: closed. Created: 2026-08-22T11:42:14Z. Closed: 2026-08-22T11:54:02Z. Merged: .

One-shot publication trigger for the already-committed 0.9.5 architecture-detection hotfix. Builds the exact main source plus this workflow-only trigger, verifies Common 1.9.0 and installer contracts, publishes v0.9.5, then closes and deletes this temporary branch. This PR is not intended to merge.

- Comment/review by github-actions[bot] (): ExtFS 0.9.5 published and verified; removing one-shot publication branch.

### #24 — Qualify pre-MS filesystem hardening

State: closed. Created: 2026-08-22T14:14:26Z. Closed: 2026-08-22T14:46:58Z. Merged: 2026-08-22T14:46:58Z.

Temporary qualification PR only. The filesystem hardening has now passed portable CI, real ext2/ext3/ext4 image qualification, filesystem-contract regression checks, and Windows WDK Release packaging on both x64 and ARM64. Final step before merge: remove the temporary materializer/cleanup logic from the PR head, retain only the permanent filesystem-contract gates, rerun the exact clean candidate, then merge. After merge, all remaining temporary pre-MS workflow files on main will be removed in one cleanup commit and the temporary branch will be removed.

- Comment/review by chatgpt-codex-connector[bot] (): You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

### #25 — Delete temporary pre-MS qualification branch

State: closed. Created: 2026-08-22T14:50:13Z. Closed: 2026-08-22T14:51:38Z. Merged: .

Housekeeping completed successfully. The registered cleanup job deleted pre-ms-qualification-temp. This PR was never intended to merge.

- Comment/review by chatgpt-codex-connector[bot] (): You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

### #26 — Qualify single-indirect resize implementation

State: closed. Created: 2026-08-22T22:49:46Z. Closed: 2026-08-22T23:13:45Z. Merged: .

Qualification-only PR. The production implementation is already on main at 34ebc65d0f1d83aa7fc87ca96837b89009829ce9. This branch adds only a trigger file so the same source can be exercised by the repository's pull-request CI. Do not merge this PR.

- Comment/review by chatgpt-codex-connector[bot] (): You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

### #27 — Require explicit approval for ExtFS publishers

State: closed. Created: 2026-08-23T03:41:11Z. Closed: 2026-08-23T03:47:35Z. Merged: 2026-08-23T03:47:35Z.

Removes the magic commit-message publication triggers from both ExtFS release workflows. The 0.9.5 hotfix publisher and ARM64 preview publisher are now manual-only, check out and verify exact current `main`, refuse existing tags/releases, and target the checked-out main SHA. Portable CI now rejects any future automatic trigger in these publishers.

- Comment/review by chatgpt-codex-connector[bot] (): You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

## Recent commit provenance

The connector returns the latest 100 commit records. Final v0.9.9 file contents are preserved byte-for-byte in the frozen source snapshot.

- `dda219be47033372217149bce8c8ee20f19ded0a` —  — Release 0.9.9: advance Common to 1.19.24
- `c26ca06f7c36b7ef36fd53e103f6d74e8d5decd1` —  — Update copyright start year to 1993
- `939e6f49d98adfe6394259b2c2b6f70055c0de18` —  — Normalize copyright to 2000-2026
- `fd49361ced56fbfbc3a3483260d6cea45ccaf76f` —  — Run one-time copyright normalization
- `4e33a446f8936f368fafe04e78b5250b867cc189` —  — Normalize project copyright to 2000-2026
- `8d660f5a5a96ab140282617cc6d679232742ae8d` —  — Fix canonical documentation references
- `cda13e084aa31d8ef4fb60310b20513d6561e9ed` —  — Promote canonical engineering documentation
- `2d35b3033dad29f8ca6675d0d30c6a65710bf760` —  — Add canonical documentation index
- `efc46d9e352dcbd9e8cdc3e2e8f6c172d4ed8b23` —  — Standardise documentation baseline
- `a4307c71200af1f95cb06bca0f8336e22dafc154` —  — Make first-principles ethos explicit
- `c9de5b3d23623751c28515ec611ec498cb53e27a` —  — Document project engineering ethos
- `ab6478a857636364779a1d1037c8f9922e7b7000` —  — Release 0.9.8: rename project to Ext Filesystem Driver
- `a54377e9c2d7e8c07da9727437a8119afa9ee155` —  — Release 0.9.7: adopt Common 1.19.2
- `2d96090ffaf9461a7f55a75cb557d1a4b8ba6790` —  — ci: restore home Linux runner preference
- `c668efe326e5cd3af088e99422f5015574e76fa2` —  — Release 0.9.6: retry hosted publication after release-probe fix
- `bc2b4049cf0e407c0210a1f83c88156885c01185` —  — ci: make Windows release existence probes non-fatal
- `2213c41d76f6361a66ff2a04eb71c3bed3579a33` —  — Release 0.9.6: hosted rebuild and publication refresh
- `10a9665f6a7ac2e7d87f592d41dbb2d8f4209f10` —  — ci: run native Linux qualification on hosted Ubuntu
- `c6c06f9bac6bfed6a3c3c0dce9b87023dc54fe61` —  — Requalify ExtFS 0.9.6 after runner restart
- `645fb0ab2dcda042b2040939fdb18b41583e36f7` —  — Allow expected missing release probe on Windows
- `25339e536cfdaf953c4c3b8fe2bed207b60a3753` —  — Release 0.9.6: fresh verified publication
- `180190dd83a0b1758703ffd749bb00687e232869` —  — Add self-hosted Linux qualification lane
- `3b5c94c1fe9265419bd71b7ac487f6417fe8c955` —  — Update repository references after organization migration
- `f6de242f7bdf1d244ea9e994822ed70b4e1c6aa0` —  — Fix installer package version contract
- `0dd7205d8840600b34f3d7f87eb45008ac7be0b1` —  — Update Infiltratr Common to 1.15.0
- `dfc865214704f70471969fe9ae4af58a1c81ecfc` —  — Prepare ExtFS 0.9.6 package version
- `0f0ca7e2ee39113a8af16c09afd15da1b63e4bcc` —  — Document ExtFS Common 1.11 signing boundary
- `f06907cd479b6eef6e1924ce33e1490feaf9fad6` —  — Document ExtFS Common 1.11 dependency
- `f92c5e46fe8c6df712317ee3bf4fca9911af519f` —  — Update ExtFS cross build to Common 1.11
- `d22e91a563af90022c509e79804f10ff7491f6ba` —  — Update ExtFS to Infiltratr Common 1.11
- `36eee12014048c247e059a5e026bba8d77403803` —  — Standardise ExtFS README
- `4c7147d7b4d7df975447178546dce54bb862174e` —  — Make ExtFS WDK CI main-only
- `275522d6db14d3ec710abf06879b73825ce61921` —  — Remove obsolete ExtFS ARM64 preview publisher
- `222beef2debad3ccecae91dfe8570a40d94b85e2` —  — Make ExtFS portable CI main-only and guard automatic releases
- `ec5456dcddfc9c81a979fa9eea9d8a6883065eaf` —  — Unify ExtFS automatic main-only publishing
- `003c96799080877944f6679d0efb0642eb3e21db` —  — Require explicit approval for ExtFS publishers (#27)
- `50656a5920663d8c73a65f839ef3b394d7810698` —  — Correct classic indirect resize boundary
- `34ebc65d0f1d83aa7fc87ca96837b89009829ce9` —  — Add bounded ext2/ext3 single-indirect resize
- `0361bfede5a787238b8ffa79e46a6ea20577ac0f` —  — Remove one-shot branch cleanup hook
- `8aaf43757fab4a2d79a086524dd97292f7165689` —  — Add one-shot branch cleanup hook
- `dc0acc0d275fa74c76a6a72619163d100829d404` —  — Remove temporary pre-MS qualification workflows
- `853e57332627afeca100824fd68cf04243ac3b96` —  — Pre-MS hardening: lifecycle and filename correctness
- `0a8caf5bf66a6f427bba7e02a11705b344e374d7` —  — Add issue workflow dispatch probe
- `18e42ba9be3cf8a9970805d1b8aee612d5326aa5` —  — Add observable issue-driven pre-MS gate
- `9f511555beaf620ec8b88665c5a7ba1ae764d6df` —  — Add pre-MS detached candidate builder
- `07919885d57758b4e904cbf2c9d051baf7f3d731` —  — Add pre-MS documentation transformer
- `921fa6767c7fe62616742d6a4a00313347f170e4` —  — Trigger observable pre-MS qualification
- `12252a5ab150a67fd8ed15d3f2cd5990c6e5ea55` —  — Install observable detached pre-MS gate
- `21c034c252f56c5e5f252a9e60346762cca420a6` —  — Trigger detached pre-MS qualification
- `b57091af43e5b16189468414df06f76200eefa2e` —  — Install detached pre-MS push gate
- `5d0f60cf9f6c935936dcc9a2ccc995371dd52030` —  — Keep pre-MS qualification gate deterministic
- `598b54af2fbac9b10e328b91132f9b38c9cf1194` —  — Preserve exact patch mode in pre-MS qualification
- `ceea565d4658b76b2208bd311577fc4278ae6232` —  — Normalize pre-MS detached qualification workflow
- `4a3595c448f6e3b1585a04316c2f08ad8e8b039a` —  — Qualify detached pre-MS hardening commit
- `0c3566a62e69df4eb485c6c729518350e05ad4a4` —  — Fix pre-MS hardening source anchors
- `f30c33f6a551c59b7efd0e68beeff150cc226ed2` —  — Add diagnostics to pre-MS qualification gate
- `a5fc64db4a75befeb1c9e09e53cfdd55bd8d72c1` —  — Fix pre-MS qualification workflow syntax
- `0c780d564ce8c4d282b1f98a225f62d099b961b3` —  — Harden pre-MS qualification runner
- `8e916ace8bbff81ea46e34bc55a4caff0db43433` —  — Make pre-MS hardening explicitly triggerable
- `d8cf8e989b727ff8caeb322270f96a09ddc85334` —  — Execute pre-MS hardening
- `69c614e5bbc28b160b613f3f10a2ac2666f10bce` —  — Execute pre-MS hardening
- `9138c2d186a9e69620b1a35a57a7ea30cdbd9f5c` —  — Stage pre-MS hardening transformer
- `612bc0360cf8a02469fb48cebcadc48b94c49b16` —  — Start pre-MS ExtFS hardening
- `58d7ed713fc7267ca9159f57f42b00c9dcacdf66` —  — Start pre-MS ExtFS hardening
- `7b19e2fc32641f57a628a412fd45a32a94efeff7` —  — Add Secure Boot production installer path
- `ac80c6a537ebb680ea3775b40118578bde3d88ad` —  — Add Hardware Dev Center submission CI
- `ab2e6db2ccaa0ca8f64a1ede44dd911d9ee95e41` —  — Make Secure Boot a production requirement
- `47efd50430b7ac3d50369bed99140fb7a9b5cc20` —  — Document Secure Boot production signing path
- `01a44aad123291fc4b3fc15eccfd656d5b3b9eb9` —  — Add Secure Boot hardware submission builder
- `bd4fabf605345a5726a8ff83899abcb115deb477` —  — Remove superseded ExtFS 0.9.6 repack publisher
- `d1120415c74c5869a8a2abdb4ce2308821be6549` —  — Restore ExtFS 0.9.5 release documentation
- `0e3b135575a8c7ee7694430bc561b9c89a6498b7` —  — Restore ExtFS 0.9.5 release publisher
- `d787f694bac6cc2fdffbe669ded9ce7b3463746b` —  — Restore ExtFS 0.9.5 Windows package CI
- `ad77b670e0e3a19dc54ea066451e1063ab9395e8` —  — Restore ExtFS 0.9.5 uninstall script
- `e54daee1c817d58615f9cc7b40396d24157cb95a` —  — Restore ExtFS 0.9.5 installer contract tests
- `d8ac2a1bf897f2097d49544e9c4a75005a3890b3` —  — Restore ExtFS 0.9.5 package builder
- `49ebecfe70b3435b9af0e0451efb274bca8d0ca1` —  — Restore ExtFS 0.9.5 installer shell
- `de7321fa207c37cd595cd789ee35071037f8d8e5` —  — Restore validated 0.9.5 reboot preflight
- `3ec00e2be83ed835db9a5fe02b22e6bd8a61ead1` —  — Repack and publish ExtFS 0.9.6
- `c067e0ab209a0fbc514939f7c3e56bdcf8a9e492` —  — Add fast ExtFS 0.9.6 installer repack publisher
- `d72c6f156df6389dd0439c7afee919af4864146d` —  — Publish ExtFS 0.9.6 pending-restart hotfix
- `eb5e6a2e4098168e7d36a5cfe5ddf9ea96e8a47b` —  — Add ExtFS 0.9.6 publication receipt
- `d1b782631210d30ea772a8eb8ececeff6e3aa71d` —  — Publish ExtFS 0.9.6 pending-restart hotfix
- `efc39e20f41c5b1110ae9456f346fe55a147a39f` —  — Make ExtFS 0.9.6 publisher idempotent
- `0e2c89e0c35b9a63cef6d4ab1ce3abd372039cbb` —  — Harden ExtFS 0.9.6 reboot marker persistence
- `528e8799962df00f87ac0529d272b19521770e1b` —  — Publish ExtFS 0.9.6 pending-restart hotfix
- `ea37287f9da521494c5cd5ad9bf37e3a049c2183` —  — Prepare ExtFS 0.9.6 release publisher
- `b7a8f809f910c3ada3ac68d6f2e7cde79731033f` —  — Qualify ExtFS 0.9.6 packages
- `b2218f624d2483f993cc40c4c90c79029a383eb7` —  — Update ExtFS 0.9.6 uninstall messaging
- `deee0365dad80449faf49c68509846f6380891e9` —  — Qualify ExtFS 0.9.6 reboot preflight
- `7718d65a201a0cb24dde7c8816d3c6410d0bb3d4` —  — Version ExtFS package builder at 0.9.6
- `5b65640aedb09f3be9d290c64b1fd0095f08b24f` —  — Prepare ExtFS 0.9.6 installer
- `b12f64afdca28c6ac5e55573c8f27c9940dddbc1` —  — Fix ExtFS pending-restart preflight
- `b8aaa45ad993d7c044d830f2bbd7c2ab4396d023` —  — Fix 0.9.5 architecture regression test
- `36fb8f15d2b38badacc7648971f48095ff894e76` —  — Publish ExtFS 0.9.5 architecture hotfix
- `c17b3fe1945b2bc82b74d1a5d99e65c361318994` —  — Prepare ExtFS 0.9.5 architecture hotfix release
- `13d67e8122bee08b16e911f6aad8a81e536c6bc7` —  — Update ExtFS 0.9.5 uninstall messaging
- `970d864bb790d346cbbce7258a95a475c8a7e9a4` —  — Qualify ExtFS 0.9.5 installer hotfix
- `4598e8e117b1e49883b0e41b1614ed20682d4790` —  — Add architecture detection regression coverage
- `c21b45feb2bd9ab454a7b988d9042d8779765e4f` —  — Package architecture hotfix as 0.9.5

## Branch-search snapshot

```json
{
  "branches": [
    {
      "branch": "main"
    }
  ],
  "cursor": "MQ"
}
```

## Preservation limitation

This archive preserves the final source byte-for-byte plus the engineering metadata available through the connector: releases and their asset inventory, issue/PR bodies and discussions, recent commit identities, and branch-search state. It does not duplicate GitHub-hosted binary release assets or every historical Git object/diff. Historical installers will cease to be downloadable from the old repository if that repository is deleted unless separately preserved.
