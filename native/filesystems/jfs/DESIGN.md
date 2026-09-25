# JFS Design and Ownership

## Classification

JFS is a conventional local journalled block filesystem. Filesystem Support
therefore applies the one-canonical-filesystem architecture: format and
filesystem semantics belong in a host-neutral core; operating-system code is an
adapter around that core.

## Current implementation state

JFS is currently **reference/import state**.

There is no project-authored canonical JFS engine and no Filesystem Support JFS
native module claimed by this directory. The source under `reference/linux/`
is the pinned upstream Linux JFS implementation retained as semantic,
interoperability and failure-behaviour evidence.

The reference files retain their original provenance, licences, filenames and
implementation boundaries. Their relocation under `reference/linux/` is not
an implementation rewrite.

## Current layout

```text
native/filesystems/jfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux JFS evidence
```

A future project implementation is added beside the reference tree:

```text
native/filesystems/jfs/
  DESIGN.md
  core/                  canonical JFS format/filesystem semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/qualification adapter
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future core owns JFS-defined semantics independent of the host OS,
including:

- superblock and aggregate/fileset geometry;
- inode allocation groups and inode-map semantics;
- block allocation maps and extent descriptors;
- directory B+tree and name/record rules;
- file extent mapping;
- journal/log record format, transaction and replay invariants;
- extended attributes and filesystem metadata records;
- free-space/accounting rules;
- corruption/range validation;
- exact mutation and crash-recovery semantics.

Linux VFS objects, page/buffer cache, block-device plumbing, workqueues,
credentials, quota framework glue and module lifetime belong in the Linux
adapter.

## File-boundary rule

Upstream Linux filenames remain unchanged beneath `reference/linux/`.
Project-owned source must be organised around Filesystem Support
responsibilities and host-neutral JFS invariants, not copied or mechanically
renamed from Linux.

Moving, renaming, merging or recommenting external source is never evidence that
the implementation has crossed the project-authorship boundary.

## Promotion sequence

JFS leaves reference/import state only through an independent rewrite:

1. document supported on-disk format and journal variants;
2. implement bounded superblock, inode-map, allocation-map and extent decoding;
3. implement read-only directory/file traversal with independent fixtures;
4. implement and test journal/log decoding and replay invariants;
5. qualify malformed media and crash-state handling;
6. add a thin Linux adapter over the canonical core;
7. add mutation only after transaction/recovery ordering is independently
   implemented and destructively qualified;
8. add a Windows adapter against the same core when ready.

## Safety and failure behaviour

Invalid geometry, contradictory allocation/inode maps, malformed B+tree nodes,
out-of-range extents, bad journal records and uncertain replay state must fail
closed.

Write support must not be inferred from the capabilities of the reference
driver.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/jfs` into
`reference/linux/`. Refreshes may update reference evidence but must never
overwrite future project-owned source.

## Completion rule

The current tree is reference evidence only. JFS is not a Filesystem Support
native implementation until the canonical core and applicable adapter are
independently implemented and qualified.
