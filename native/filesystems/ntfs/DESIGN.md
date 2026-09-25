# NTFS Design and Ownership

## Classification

NTFS is Microsoft's local on-disk filesystem. Filesystem Support therefore
applies the normal canonical-filesystem architecture: NTFS semantics are owned
once by one host-neutral engine and operating systems expose that engine through
thin adapters.

The catalogue currently exposes two external provider paths for the same
filesystem:

- `ntfs` — Debian's mature NTFS-3G userspace provider;
- `ntfs3` — the Linux kernel's native NTFS3 provider when the running kernel
  supplies it.

Those provider identities are not separate filesystems and must never become
separate Filesystem Support NTFS implementations.

## Current implementation state

NTFS is currently **external-provider / future-native state**.

There is no project-authored NTFS filesystem engine in this directory today.
The product manages NTFS-3G through the catalogue/action backend. The Linux
NTFS3 source retained elsewhere in the repository is reference/provider
evidence only and must not become a competing canonical implementation.

The former `.gitkeep` represented no implementation or ownership contract and
has been removed.

## Canonical ownership point

This directory is the ownership point for any future first-party NTFS
implementation:

```text
native/filesystems/ntfs/
  DESIGN.md
  core/                 canonical NTFS format/filesystem semantics, when written
  linux/                thin Linux VFS/module adapter, when written
  windows/              thin Windows IFS/WDK adapter, when written
  reference/            optional preserved external implementation evidence
```

Directories are created only when real project-owned implementation or
preserved reference material exists. Empty `core/`, `linux/` or `windows/`
placeholders must not be used to imply progress.

## One NTFS implementation rule

All filesystem-defined behaviour belongs in `ntfs/core/` when the rewrite is
started. That includes, as supported:

- NTFS boot-sector and volume geometry interpretation;
- MFT and file-record parsing, fixup/update-sequence validation and record
  identity;
- resident and non-resident attribute semantics;
- run-list decoding, allocation and sparse/compressed stream rules;
- directory indexes and filename/namespace behaviour;
- hard links, alternate data streams and reparse-point semantics;
- bitmap/free-space accounting;
- security-descriptor and object-id format semantics;
- logfile/recovery behaviour and transaction ordering;
- metadata integrity, range validation and corruption rejection.

Linux VFS objects, folios/page cache, block-device plumbing, module lifetime and
Linux error translation belong in `linux/`.

Windows IRPs, VCB/FCB/CCB lifetime, Cache Manager/Memory Manager integration,
Windows mount/verify/lock semantics and NTSTATUS translation belong in
`windows/`.

The Linux `ntfs3` provider must not grow a second project-owned NTFS parser,
allocator, namespace engine or recovery implementation. Any future NTFS3-derived
reference snapshot remains reference evidence for this canonical NTFS work.

## Provider relationship

NTFS-3G and Linux NTFS3 remain useful interoperability/reference providers
until the project has independently implemented and qualified the canonical
engine.

Provider availability is not evidence that Filesystem Support itself has
implemented NTFS. Package or kernel-module detection must remain distinct from
first-party capability claims.

## Promotion sequence

NTFS enters rewrite state only through an explicit milestone:

1. define the supported NTFS format/features and unsupported states;
2. implement bounded host-neutral boot-sector, MFT and attribute decoding;
3. qualify the core against independently manufactured NTFS images and malformed
   media;
4. implement read-only namespace, stream and run-list access;
5. add a thin Linux adapter over that same core;
6. introduce mutation only with explicit allocation, logfile/recovery and
   destructive qualification;
7. add a Windows adapter against the same canonical engine;
8. retain third-party/reference provenance until corresponding implementation
   has actually been independently replaced.

Moving or renaming NTFS-3G/NTFS3 source does not cross the authorship boundary.

## Failure policy

Unknown incompatible features, invalid update-sequence arrays, malformed MFT
records, contradictory attribute lengths, invalid run lists, out-of-range
clusters, corrupt indexes and unsafe logfile/recovery state must fail closed.

A writer must not modify media until all structures required for the operation
and its recovery boundary are understood and independently validated.

## Completion rule

The current `ntfs` catalogue entry is complete only as an external
NTFS-3G provider contract. A Filesystem Support native NTFS implementation is
not claimed until canonical core code, adapter code, tests and qualification
evidence exist and agree with this design.
