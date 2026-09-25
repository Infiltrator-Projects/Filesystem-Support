# HPFS Design and Ownership

## Classification

HPFS (High Performance File System) is a conventional local block filesystem
used by OS/2 and related systems. Filesystem Support therefore applies the
normal one-canonical-filesystem architecture.

## Current implementation state

HPFS is currently **reference/import state**.

There is no project-authored HPFS canonical engine and no Filesystem Support
HPFS Linux or Windows driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux HPFS implementation retained
as compatibility and semantic evidence.

Those files keep their upstream SPDX/licence/copyright notices, filenames and
implementation boundaries. Relocating them beneath `reference/linux/` is
provenance hygiene only and is not an implementation rewrite.

## Current layout

```text
native/filesystems/hpfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux HPFS evidence
```

When independent implementation begins, project-owned source is added beside
the reference tree:

```text
native/filesystems/hpfs/
  DESIGN.md
  core/                  canonical HPFS format/filesystem semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter
  userspace/             optional inspection/image adapter
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future `core/` owns HPFS-defined semantics that are independent of the
host operating system, including:

- boot/superblock and spare-block decoding and validation;
- sector, band and allocation-bitmap geometry;
- fnode and anode structures and file block mapping;
- dnode directory B-tree structure, ordering and traversal;
- filename, codepage and case/collation rules defined by HPFS;
- extended attributes and filesystem metadata records;
- free-space accounting and allocation;
- hotfix/remapping state where supported by the format;
- timestamps, attributes and object metadata;
- corruption/range validation;
- exact mutation, ordering and recovery semantics for any supported writer.

Linux VFS objects, buffer/page-cache integration, mount/module lifetime,
credentials and Linux error translation belong in `linux/`.

Windows IFS/WDK objects, IRPs, VCB/FCB/CCB lifetime, Cache Manager integration
and NTSTATUS translation belong in `windows/`.

## File-boundary rule

Reference filenames such as `alloc.c`, `anode.c`, `dnode.c`, `map.c`,
`namei.c` and `super.c` remain unchanged under `reference/linux/` so
their provenance is explicit.

Project-authored source must be cut around Filesystem Support responsibilities
and portable filesystem invariants. Moving, renaming, merging or recommenting
copied Linux source is never evidence that the implementation has been
rewritten.

## Promotion sequence

HPFS leaves reference/import state only through a deliberate rewrite:

1. define supported HPFS versions, codepages and media geometry;
2. implement bounded host-neutral superblock/spare-block/fnode/anode decoding;
3. implement allocation and directory-tree traversal with independent fixtures;
4. qualify names, codepage/case behaviour and extended attributes;
5. test malformed/corrupt media and out-of-range structures;
6. add a thin read-only Linux adapter over the canonical core;
7. add mutation only after allocation/update/recovery rules are explicitly
   documented and destructively qualified;
8. add a Windows adapter against the same core when ready;
9. remove inherited provenance only from bodies that have actually been
   independently replaced.

## Safety and failure behaviour

Invalid geometry, contradictory allocation maps, malformed fnode/anode chains,
invalid dnode ordering, out-of-range sectors, unsupported codepage/collation
state and unsafe recovery state must fail closed.

The presence of write paths in an external reference implementation is not
evidence that Filesystem Support has qualified HPFS writes.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/hpfs` into
`reference/linux/`. Refreshes may update reference evidence but must never
overwrite future project-owned `core/`, `linux/`, `windows/` or
`userspace/` source.

## Completion rule

HPFS remains reference/import only until a canonical project-authored engine
and at least one adapter have independent implementation and qualification
evidence.
