# XFS Design and Ownership

## Classification

XFS is a conventional local journalled extent filesystem. Filesystem Support
therefore applies the one-canonical-filesystem architecture: persistent XFS
format and filesystem semantics belong in a host-neutral core, while operating
systems expose that core through thin adapters.

## Current implementation state

XFS is currently **reference/import state**.

There is no project-authored canonical XFS engine and no Filesystem Support XFS
native module claimed by this directory. The source under `reference/linux/`
is the pinned upstream Linux XFS implementation retained as format,
interoperability, repair and failure-behaviour evidence.

Those files retain their upstream SPDX/licence/copyright notices, filenames and
implementation boundaries. Relocating the unchanged Linux tree from the old
`kernel/` staging path to `reference/linux/` is provenance/layout hygiene
only; it is not an implementation rewrite and does not change authorship.

## Current directory layout

```text
native/filesystems/xfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux XFS source, reference only
```

When an independent implementation begins, project-owned source is added beside
that evidence:

```text
native/filesystems/xfs/
  DESIGN.md
  core/                  canonical XFS format/filesystem semantics
  linux/                 thin Linux VFS/block/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/inspection/qualification adapter
  reference/linux/       preserved upstream Linux evidence
```

Empty project implementation directories are not created merely to imply
progress.

## Canonical-core ownership

A future `core/` owns XFS-defined, host-neutral behaviour including, for the
explicitly supported feature set:

- superblock, allocation-group and feature-bit interpretation;
- AGF/AGI/AGFL geometry and allocation-group consistency;
- free-space, inode, reverse-mapping and refcount B+tree semantics;
- inode format, forks, extent mapping, unwritten extents and sparse files;
- reflink/shared-extent and copy-on-write persistent semantics;
- directories, names, attributes, remote attributes and symlink representation;
- realtime-device bitmap/summary format where supported;
- persistent quota records and accounting invariants;
- metadata checksums, UUID/owner checks and structural validation;
- log item formats, transaction ordering, log replay and crash recovery;
- persistent online-repair state only where explicitly implemented;
- resize/grow rules that alter persistent filesystem geometry;
- corruption, range and cross-structure consistency validation.

Linux VFS objects, folios/page cache, iomap plumbing, buffer/block-device
interfaces, workqueues, sysfs/procfs/debugfs, Linux credentials, module
lifetime and Linux-specific online-scrub plumbing belong in the Linux adapter
or remain reference mechanisms. Windows IRPs, Cache Manager integration and
native object lifetime belong only in the Windows adapter.

## Reference scrub and libxfs rule

The imported `libxfs/` and `scrub/` subtrees remain part of the upstream
reference snapshot. Their presence is valuable behavioural and repair evidence
but does not make them project-owned canonical code.

Project-authored XFS source must be cut around Filesystem Support
responsibilities and portable filesystem invariants. Moving, renaming, merging,
splitting or recommenting imported Linux translation units is never evidence
that their implementation has crossed the project-authorship boundary.

## Promotion sequence

XFS leaves reference/import state only through deliberate independent work:

1. define the supported XFS format generation and incompat/ro-compat features;
2. implement bounded host-neutral superblock and allocation-group validation;
3. implement read-only inode, extent, directory and attribute traversal using
   independently manufactured XFS media and malformed fixtures;
4. implement the required B+tree, checksum and ownership validation in the
   canonical core;
5. model log records, replay and recovery before any writer is admitted;
6. add a thin Linux adapter over the canonical core;
7. add allocation/mutation only after transaction, log, CoW/refcount and
   recovery ordering are explicit and destructively qualified;
8. add realtime, quota, reflink, grow and repair features only when their
   persistent contracts are independently implemented and tested;
9. add Windows support only as an adapter over the same canonical engine.

## Safety and failure behaviour

Unknown incompatible features, invalid allocation-group geometry, malformed or
misordered B+trees, contradictory rmap/refcount/allocation state, invalid inode
forks/extents, checksum or owner mismatches, out-of-range block references and
unsafe/ambiguous log-recovery state must fail closed.

A reference Linux implementation being able to mount or repair a volume is not
evidence that Filesystem Support has independently qualified equivalent write
or recovery support.

## Reference provenance

The refresh workflow pins Linux **v6.12.107** and copies `fs/xfs` unchanged
into `reference/linux/`. Refreshes may replace only that copied reference
tree and must never overwrite future project-owned `core/`, `linux/`,
`windows/` or `userspace/` source.

## Completion rule

The current tree is reference evidence only. XFS becomes a Filesystem Support
first-party implementation only when its canonical engine and applicable
platform adapter have independent implementation, tests and qualification
evidence.
