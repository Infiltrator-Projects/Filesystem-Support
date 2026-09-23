<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Ext Filesystem Driver

**Project copyright:** © 1993-2026 Shannon Smith

[![Portable ExtFS CI](https://github.com/Infiltrator-Projects/ExtFS-for-Windows/actions/workflows/portable-ci.yml/badge.svg)](https://github.com/Infiltrator-Projects/ExtFS-for-Windows/actions/workflows/portable-ci.yml)

Ext Filesystem Driver is an original native Windows filesystem project written primarily in C. Its goal is native ext2/ext3/ext4 access through the normal Windows I/O stack, with the portable filesystem core kept independent from Windows-specific driver plumbing.

**Current package version:** 0.9.8  
**Filesystem driver payload:** 0.9.3.0  
**Platforms:** Windows x64 and ARM64, plus a portable userspace qualification core  
**Licence:** GPL-3.0-or-later

## Engineering ethos

What does it take to build native Windows ext filesystem support from first principles without making another driver or utility the product's foundation? Ext Filesystem Driver starts from the ext on-disk formats and the Windows I/O contract, then implements the supported intersection directly.

Specifications, Linux behaviour, existing tools and other drivers are reference evidence; they are not runtime dependencies or substitutes for understanding the structures being modified. The portable filesystem core owns the parsing and mutation rules, while the Windows layer adapts those rules to the native kernel interface. When the evidence is insufficient for a safe write, the project refuses the operation rather than approximating it.

Mature techniques are retained when they remain the strongest option. New code or dependencies are adopted because they improve correctness, safety, portability or maintainability, not merely because they are newer.

## Capabilities

The current filesystem boundary includes:

- read access to supported ext2/ext3/ext4 layouts;
- same-size overwrite of allocated initialized regular-file data;
- ext2 direct-file resize;
- journaled ext3 direct-file resize;
- bounded ext2/ext3 classic single-indirect resize;
- bounded checksum-aware ext4 resize using inode-resident extents or one external depth-0 leaf beneath a depth-1 root; and
- fail-closed rejection of unsupported write-sensitive layouts.

Version 0.9.9 is a dependency-maintenance checkpoint carrying the already-qualified 0.9.3.0 filesystem driver. It retains the 0.9.5 native-architecture correction, pins Infiltratr Common 1.19.24 and derives every installer/build version from the single root `VERSION` file.

## Architecture

The portable core is freestanding C with no operating-system headers, internal heap allocation, threads or global mutable state. Windows kernel integration lives at the adapter/driver boundary.

The Windows adapter consumes only the kernel-safe compiler-annotation header from pinned Infiltratr Common 1.19.24. Common user-mode runtime sources are not linked into `extfs.sys`.

Unsupported layouts remain fail-closed. Double/triple-indirect classic mutation, broader multi-leaf/deeper ext4 extent trees, 64-bit/flex_bg metadata allocation, sparse/unwritten allocation, external journals, `bigalloc`, inline data, encrypted/casefolded layouts and unknown write-sensitive features are deliberately refused.

## Build and test

Portable qualification:

```sh
make
./build/test-extfs
make integration
```

`build/extfs-tool` is a read-only image inspector. `build/extfs-mutate-test` exists only for qualification against disposable images.

Windows package build with Visual Studio/WDK and NSIS:

```bat
git clone --recurse-submodules https://github.com/Infiltrator-Projects/ExtFS-for-Windows.git
cd ExtFS-for-Windows
BUILD-EXTFS.cmd setup x64
BUILD-EXTFS.cmd setup ARM64
```

GitHub CI runs portable unit/integration tests, sanitizer and static-analysis passes, filesystem-contract regression checks and Windows WDK packaging/validation.

## Secure Boot and signing

**Secure Boot is a production requirement.** Public production drivers are intended to install and load on stock supported Windows systems with UEFI Secure Boot enabled. Users must not be required to disable Secure Boot or enable TESTSIGNING.

The current 0.9.x installers are development/test-signed packages. Production releases must consume a Microsoft Hardware Dev Center production-signed driver package. `windows/Build-HardwareSubmission.ps1` creates the submission CAB; see `docs/SECURE_BOOT_SIGNING.md` for the signing path.

## Release assets

A numbered release is designed to publish:

| File | Purpose |
| --- | --- |
| `ExtFS-for-Windows-<version>-experimental-x64-setup.exe` | x64 development/test installer. |
| `ExtFS-for-Windows-<version>-experimental-arm64-setup.exe` | ARM64 development/test installer. |
| `ExtFS-for-Windows-<version>-source.zip` | Tested source archive from the exact release commit. |
| `SHA256SUMS.txt` | SHA-256 checksums for all published project artifacts. |

The experimental installers are not a substitute for the future production-signed Secure-Boot distribution path.

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs the portable CI, with Windows WDK/package CI providing the platform-specific qualification path. Ordinary commits do not publish. A commit is release-eligible only when its subject begins with the exact package version as `Release <version>` and the required CI gates succeed.

The publisher checks out the exact tested commit, verifies it is still current `main`, rebuilds and validates both x64 and ARM64 installers, creates the source ZIP and checksums, then creates the version tag and GitHub release. Existing version tags and published releases are immutable and are never moved, replaced or edited in place.

Manually runnable build/submission helpers are diagnostic or packaging tools only and are not release-approval mechanisms.

## Documentation

- `docs/FEATURE_SUPPORT.md` — exact filesystem feature boundary.
- `docs/SECURE_BOOT_SIGNING.md` — production signing path.
- `docs/ARM64_TESTING.md` — ARM64 qualification.
- `docs/VERIFICATION.md` — verification evidence and checkpoints.
- `docs/ROADMAP.md` — planned filesystem/driver work.

## Licence

Ext Filesystem Driver is free software licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`). See `LICENSE`.
