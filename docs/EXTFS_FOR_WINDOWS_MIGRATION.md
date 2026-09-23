# ExtFS-for-Windows migration preservation ledger

## Purpose

This document prevents useful engineering information from being lost while ExtFS-for-Windows is folded into Filesystem Support.

The old repository is not considered disposable merely because equivalent functionality appears to exist elsewhere. Every unique responsibility must be classified, migrated or explicitly retired, then verified.

## Source baseline

Migration source:

- repository: `Infiltrator-Projects/ExtFS-for-Windows`
- main commit: `dda219be47033372217149bce8c8ee20f19ded0a`
- tree: `76b127deaa8b5b4776774a9579ca6c791d9d6b38`
- latest release: `v0.9.9`, published 2026-09-22
- Common pin at that baseline: `748e089ae175329471d4cf375522c44081371bd5`

Migration destination baseline:

- repository: `Infiltrator-Projects/Filesystem-Support`
- main commit before documentation: `0f9da8ca450917005b8bba9f18d8fa720cd09aad`
- tree: `db98355a2fcdfb09f25f0c42f8b60f6f9c074501`

## What exists in ExtFS-for-Windows

### Portable filesystem implementation

- `core/extfs.c`
- `core/extfs_classic_resize.c`
- `core/extfs_resize_dispatch.c`
- `include/extfs/extfs.h`

These contain portable EXT2/EXT3/EXT4 parsing, validation and a bounded mutation implementation. They must not survive as a second long-term implementation beside Filesystem Support's canonical EXT family.

During migration they are evidence and testable reference code. Functions that are stronger or more defensive than the Filesystem Support implementation must be carried into the canonical implementation where appropriate before the duplicate is retired.

### Windows filesystem driver

- `windows/driver/extfs_driver.c`
- `windows/driver/extfs_driver.h`
- `windows/driver/extfs.inf`
- `windows/driver/extfs.rc`
- `windows/driver/extfs.vcxproj`
- `windows/driver/extfs.sln`

This is not duplicate filesystem-format code in its entirety. It includes Windows IFS lifecycle and request-handling knowledge that must migrate into the Windows platform adapter/framework.

### Windows build, signing and packaging

Preserve and generalise the engineering contained in:

- `BUILD-EXTFS.cmd`
- `Directory.Build.props`
- `packages.config`
- `windows/Build-And-Validate.ps1`
- `windows/Build-ExperimentalSetup.ps1`
- `windows/Build-HardwareSubmission.ps1`
- `windows/build-cross-linux.sh`
- `windows/build-wdk.cmd`
- `windows/installer/*`

The existing setup EXE is an NSIS installer, not a general filesystem-selection application. Its packaging/signing logic is valuable, while its EXT-only product identity must be replaced by Filesystem Support identities and module selection.

### Windows qualification

Preserve and generalise:

- `windows/test/Collect-Diagnostics.ps1`
- `windows/test/Enable-DriverVerifier.ps1`
- `windows/test/Disable-DriverVerifier.ps1`
- `windows/test/Test-ExtFS.ps1`
- `windows/test/Test-FilesystemContracts.ps1`
- `windows/test/Test-HostReadiness.ps1`
- `windows/test/Test-InstallerContract.ps1`
- portable tests under `tests/`

The tests are especially important because they define behaviours that must continue to pass after duplicate EXT logic is removed.

### Documentation that carries unique decisions

The following documents contain decisions or validation knowledge that must either migrate or be superseded explicitly:

- `docs/ARCHITECTURE.md`
- `docs/ARM64_TESTING.md`
- `docs/DECISIONS.md`
- `docs/DESIGN.md`
- `docs/FEATURE_SUPPORT.md`
- `docs/ROADMAP.md`
- `docs/SECURE_BOOT_SIGNING.md`
- `docs/VALIDATION.md`
- `docs/VERIFICATION.md`
- `docs/WINDOWS_BUILD.md`
- `SECURITY.md`
- `CHANGELOG.md`
- `NOTICE.md`

Key durable decisions already identified are: portable filesystem semantics separated from Windows plumbing; unsupported mutation layouts fail closed; durability barriers are part of the filesystem contract; Windows object/cache/lifecycle correctness requires separate qualification; and production Secure Boot support requires the normal Microsoft signing path rather than asking users to disable Secure Boot.

## Historical releases

The old repository currently records these releases:

- v0.9.1
- v0.9.2
- v0.9.3
- v0.9.3-arm64-test.1
- v0.9.4
- v0.9.5
- v0.9.6
- v0.9.7
- v0.9.8
- v0.9.9

v0.9.9 contains x64 and ARM64 experimental setup executables, a source archive and SHA-256 manifest.

Deleting the old repository would delete the GitHub-hosted release page/assets unless they are separately preserved. Repository deletion therefore remains blocked until the required historical artifacts or an explicit decision to discard them has been made.

## Issue and pull-request history

At the migration baseline the repository has numbered history through #27. All entries returned by GitHub are closed; the set includes both pull requests and issue-driven qualification runs.

This history contains installer, publication, lifecycle, ARM64, signing and qualification context. It is not safe to assume the current source tree alone captures every rationale.

Before repository deletion, engineering-relevant issue/PR bodies and comments must either be archived into Filesystem Support or intentionally declared disposable after review.

## Classification rule

Every ExtFS-for-Windows item must end in one of four states:

- **MIGRATED** — required functionality now lives in Filesystem Support.
- **MERGED** — useful filesystem semantics were reconciled into the canonical filesystem engine rather than copied as a second implementation.
- **ARCHIVED** — retained for provenance/testing/reference but not built as production code.
- **RETIRED** — deliberately obsolete, with rationale recorded.

Nothing may remain merely “probably not needed”.

## Planned destination

The migration targets these conceptual responsibilities:

```text
Filesystem-Support/
  native/filesystems/ext2/core/       canonical EXT2 semantics
  native/platform/linux/              Linux adapter/framework
  native/platform/windows/            Windows adapter/framework
  windows/build/                      WDK/build/sign/package orchestration
  windows/installer/                  general Filesystem Support installer
  windows/test/                       Windows qualification
  docs/windows/                       Windows build/sign/test documentation
```

The final exact layout may be adjusted as dependencies are extracted, but duplicate filesystem semantics are prohibited.

## EXT2 first-pass decomposition

For EXT2, compare and reconcile:

- superblock and feature validation;
- inode decoding and validation;
- direct/single/double/triple-indirect traversal;
- block allocation and free accounting;
- resize/truncate semantics;
- directory traversal and namespace semantics;
- xattrs/ACLs where applicable;
- durability/crash-consistency behaviour;
- malformed-media rejection and checked arithmetic.

The current Filesystem Support EXT2 kernel source is much more feature-complete, while the ExtFS portable core contains useful host-neutral validation, checked geometry and fail-closed behaviour. The goal is not to choose one repository wholesale; it is to form the strongest single canonical EXT2 implementation.

## Deletion gate

Do **not** delete ExtFS-for-Windows yet.

Deletion becomes safe only after the migration checklist in `WINDOWS_FILESYSTEM_ARCHITECTURE.md` has been completed and a final repository-to-repository comparison confirms that no unique source, documentation, test, build/signing logic or required release artifact remains only in ExtFS-for-Windows.


## Migration status — 2026-09-23

Completed preservation work:

- the complete `ExtFS-for-Windows` v0.9.9 main tree at
  `dda219be47033372217149bce8c8ee20f19ded0a` is preserved byte-for-byte under
  `archive/extfs-for-windows-v0.9.9/`;
- all 62 source-tree blobs from that baseline match their archived blob SHA;
- `docs/archive/EXTFS_FOR_WINDOWS_ISSUE_HISTORY.md` preserves the old
  repository's 27 issue/pull-request records and available discussion;
- `docs/archive/EXTFS_FOR_WINDOWS_RELEASE_HISTORY.md` preserves metadata,
  notes, asset names, sizes and recorded SHA-256 digests for all 10 releases;
- the Windows manager consumes the shared Filesystem Support catalogue and
  keeps every unqualified Windows implementation non-installable;
- generic Windows host-readiness and diagnostics tooling now lives under
  `windows/test/`;
- `native/platform/windows/` is reserved explicitly for Windows IFS/WDK
  adaptation only, never filesystem-format duplication;
- CI/CTest now has a Windows catalogue qualification-gate test.

Still blocking deletion of the standalone repository:

1. the production Windows IFS adapter has not yet been extracted from the old
   ExtFS driver into `native/platform/windows/`;
2. WDK build/package/signing orchestration is still only preserved in the
   archive and has not yet been generalized into the live Windows build path;
3. EXT2 does not yet use one canonical filesystem engine on both Linux and
   Windows;
4. the old ExtFS Windows driver still depends on the archived independent
   `core/extfs*.c` implementation, so it cannot simply be promoted as the
   production Windows adapter;
5. a fresh-clone Windows build/qualification from Filesystem Support has not
   yet passed;
6. final repository-to-repository migration closure has not yet been performed.

**Deletion status: NOT SAFE YET.** Keep `ExtFS-for-Windows` unchanged until
these remaining gates are closed.


## Historical binary disposition

The standalone ExtFS-for-Windows release binaries are deliberately **RETIRED**,
not migrated into the live Filesystem Support product.

They were experimental/test-signed installers for the superseded standalone
EXT implementation. Carrying those executable installers forward would create
a second distribution path for code that Filesystem Support is explicitly
retiring.

For historical verification, Filesystem Support preserves:

- the exact v0.9.9 repository source tree byte-for-byte;
- release names, dates and release notes;
- every release asset filename and byte size;
- GitHub-recorded SHA-256 digests for the release assets where available;
- the issue/pull-request engineering history and available comments.

This is an explicit RETIRED classification under this ledger, not an accidental
loss. Deleting the old repository may therefore remove the old downloadable
experimental binaries without violating the migration preservation rule.
