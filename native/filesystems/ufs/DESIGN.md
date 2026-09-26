# UFS Filesystem Design and Ownership

## Classification

UFS is a family of conventional local block filesystems used by BSD, Sun and
other Unix systems. Filesystem Support must model explicitly which UFS1/UFS2
and vendor/layout variants it supports rather than treating "UFS" as one
undifferentiated Linux-driver mode.

The target architecture is one canonical host-neutral UFS engine with thin host
adapters.

## Current implementation state

UFS is currently **reference/import state**.

There is no project-authored canonical UFS engine and no Filesystem Support UFS
native module claimed by this directory. The source under `reference/linux/`
is the pinned upstream Linux UFS implementation retained as format,
variant-detection, byte-order and interoperability evidence.

The imported files retain their upstream provenance, licences, filenames and
implementation boundaries. The former top-level `kernel/` directory was a
legacy staging path and did not make that source project-authored.

## Current directory layout

```text
native/filesystems/ufs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux UFS-family source, reference only
```

When an independent implementation begins:

```text
native/filesystems/ufs/
  DESIGN.md
  core/                  canonical supported UFS1/UFS2/variant semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows adapter when implemented
  userspace/             optional image/inspection adapter
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns the supported UFS format semantics, including:

- variant/superblock discovery and byte-order interpretation;
- cylinder-group geometry and summaries;
- inode representation and direct/indirect block mapping;
- allocation/free-space and inode-allocation rules;
- directory record and namespace semantics;
- fragment/block allocation defined by the selected variant;
- filesystem-defined flags, timestamps and metadata;
- exact mutation/recovery rules for variants where write support is admitted;
- corruption, bounds and cross-structure consistency validation.

Linux VFS objects, buffer/page APIs, credentials, locking and mount/module
lifetime remain Linux-adapter concerns.

## File-boundary rule

The imported `super.c`, `cylinder.c`, `balloc.c`, `ialloc.c`,
`inode.c`, `dir.c`, `namei.c`, `ufs_fs.h` and related files remain
unchanged beneath `reference/linux/`.

Project-owned source must be cut around portable UFS responsibilities and
explicit variant boundaries, not mechanically renamed from Linux.

## Promotion sequence

1. enumerate exact UFS variants, magic values and byte-order modes supported;
2. implement bounded canonical superblock/geometry detection;
3. implement inode, directory and block-tree traversal for each claimed variant;
4. qualify read-only behaviour with independent UFS1/UFS2/vendor-produced media;
5. add allocation/mutation only per variant with exact safety/recovery contracts;
6. expose the same engine through thin platform adapters.

## Failure policy

Ambiguous/unknown variant state, impossible cylinder geometry, out-of-range
inode/block references, malformed directories and unsupported writable
features must fail closed. Read-only support for one UFS variant must never
silently imply safe mutation of another.

## Reference provenance

The Linux-reference refresh copies `fs/ufs` into `reference/linux/`.
Refreshes may replace only that reference tree and must never overwrite future
project-owned `core/`, `linux/`, `windows/` or `userspace/` source.

## Completion rule

The current tree is reference evidence only. UFS becomes a Filesystem Support
first-party implementation only when its canonical engine and applicable
platform adapters have independent implementation and qualification evidence
for each variant claimed.
