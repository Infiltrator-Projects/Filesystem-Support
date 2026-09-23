<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Verification record — 0.9.3 maintenance checkpoint

## 0.9.3 Windows lifecycle/Common 1.9 maintenance

0.9.3 retains the 0.9.2 filesystem mutation boundary and repairs Windows-adapter lifecycle/release defects found by the subsequent forensic audit. Every `FILE_OBJECT.FsContext` now points to the required `FSRTL_ADVANCED_FCB_HEADER`; mapped-section closure is decided through Memory Manager before an FCB is reclaimed or a removable-media cache is refreshed; header size fields follow successful resize operations; removable-media verification resynchronises `FILE_READ_ONLY_DEVICE`; the staged INF is pinned and asserted at `0.9.3.0`; and Infiltratr Common 1.9.0 remains pinned as the kernel-safe compiler-annotation dependency.

Publication is gated on Portable ExtFS CI and a bounded Windows WDK package job that builds, analyses, catalogs, signs, inspects signatures/catalog membership without mutating trust stores, and assembles the installer. The installed driver is not exercised by hosted CI: runtime Driver Verifier, mapped-file stress and destructive mounted-volume qualification remain explicit manual tests on a disposable Windows VM/test volume.

## Established Windows baseline — 11 August 2026

0.6.0 completed a native Release x64 WDK build and WDK/Visual C++ Driver Code Analysis with 0 warnings and 0 errors. InfVerif and Inf2Cat also completed with no warnings/errors, and the SYS/CAT test signatures verified.

0.7.0 and 0.8.0 were not independently WDK-tested before development continued. 0.9.1 was the GPL/SPDX maintenance checkpoint on the unqualified 0.9.0 functional code.

## 0.9.2 audit hardening

The ext2 direct-resize path now refuses a metadata-changing resize with `EXTFS_ERR_UNSUPPORTED` when the host supplies no durability barrier, before any filesystem write occurs. Once mutation begins it enforces three storage barriers: after making the filesystem dirty, after completing data/allocation/inode mutation, and after restoring the clean superblock. Regression coverage verifies the missing-flush zero-write refusal and injects failures at the first and second barriers to prove that mutation cannot proceed before a durable dirty marker and that the clean marker is never written while mutation durability is uncertain.

0.9.2 also adds a qualification-only real-image mutator. CI builds disposable `mke2fs` ext2/ext3/ext4 images within the supported mutation feature profile, grows and shrinks an existing file through the real ExtFS metadata paths, closes/reopens the image, compares file bytes and requires a clean read-only `e2fsck` after each mutation. The real-image qualification covers ordinary bounded grow/shrink. Fragmented ext4 allocation, inline-to-external promotion, external-leaf mutation/collapse and corruption/failure paths remain covered by synthetic checksum and failure-injection tests rather than being claimed as real-image-qualified.

The Windows smoke-test script performs repeated open/close FCB-lifetime stress and has an explicit `-ExerciseResize` mode for disposable test volumes. It reversibly exercises append semantics, `FileEndOfFileInformation` growth/shrink, zero-fill of newly exposed bytes and byte-for-byte restoration of the original file. Cleanup makes a best-effort original-EOF restoration after failures without allowing a cleanup exception to hide the primary qualification error; an otherwise successful probe still requires the final length and SHA-256 to match the original file.

## Automated qualification — 18 August 2026

The hardened branch has demonstrated both permanent qualification gates successfully during the 0.9.2 audit:

- Portable ExtFS CI: PASS — GCC warnings-as-errors and unit tests; real ext2/ext3/ext4 grow/shrink/reopen/content/`e2fsck`; CMake/CTest; ASan+UBSan; Clang warnings-as-errors; GCC `-fanalyzer`; Clang static analyzer; shell/XML syntax; PowerShell parse checks.
- Windows WDK CI: PASS — pinned WDK/SDK restore, native Release x64 WDK build, Driver Code Analysis, InfVerif and Inf2Cat/package validation using WDK 10.1.28000.2526 tooling.

Release remains gated on both workflows passing again on the final 0.9.2 branch head and then on the identical squashed `main` tree. Runtime Driver Verifier and destructive mounted-volume Windows qualification remain separate manual steps on a disposable Windows VM/test volume and are not implied by these automated PASS results.

## Deliberate non-claims

Persistent regular-file mtime/ctime updates remain deferred with inode metadata semantics. Paging writes, volume lock ownership, dirty-journal replay, namespace mutation, multi-leaf/deeper ext4 extent trees and 64-bit/flex_bg allocation remain outside the 0.9.3 supported mutation boundary.
