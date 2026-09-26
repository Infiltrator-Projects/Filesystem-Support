# System V / Xenix / Coherent Filesystem Design and Ownership

## Classification

The SysV filesystem family represented by the Linux `sysv` driver covers
historical System V-derived disk formats, including recognised Xenix, System V
and Coherent variants. These are conventional local block filesystems.

Filesystem Support therefore applies one canonical host-neutral engine for the
supported SysV-family variants, with thin operating-system adapters.

## Current implementation state

SysV is currently **reference/import state**.

There is no project-authored canonical SysV-family engine and no Filesystem
Support SysV native module claimed by this directory. The source beneath
`reference/linux/` is the pinned upstream Linux implementation retained as
format, variant-detection and interoperability evidence.

The upstream licences, copyrights, filenames and translation-unit boundaries
remain intact. The former top-level `kernel/` staging directory was misleading
because it looked like project-owned production source.

## Current directory layout

```text
native/filesystems/sysv/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux SysV-family source, reference only
```

When an independent implementation begins:

```text
native/filesystems/sysv/
  DESIGN.md
  core/                  canonical supported SysV/Xenix/Coherent semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows adapter when implemented
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns variant detection plus the filesystem-defined
superblock, inode, free-block/free-inode accounting, directory, block mapping,
byte-order and corruption rules for each explicitly supported variant.

Variant-specific differences must be represented explicitly in the canonical
model rather than hidden in duplicated platform implementations.

Linux buffer/page APIs, VFS objects, credentials, locking and mount/module
lifecycle remain Linux-adapter concerns.

## File-boundary rule

The imported `balloc.c`, `ialloc.c`, `dir.c`, `inode.c`,
`itree.c`, `namei.c`, `super.c` and `sysv.h` remain unchanged in
`reference/linux/`.

Project-owned source must be cut around portable format/variant responsibilities
rather than mechanically preserving those Linux file boundaries.

## Promotion sequence

1. enumerate and document the exact supported SysV/Xenix/Coherent variants;
2. implement bounded host-neutral variant detection and geometry validation;
3. implement inode, directory and block-tree traversal with independent media;
4. qualify read-only operation and malformed-media rejection per variant;
5. add allocation/mutation only where exact update/recovery semantics are known;
6. add thin platform adapters over the same canonical engine.

## Failure policy

Unknown/ambiguous variants, impossible geometry, out-of-range inode/block
references, corrupt free-list state and malformed directory records must fail
closed. Variant guessing must never turn an uncertain image into a writable
mount.

## Reference provenance

The Linux-reference workflow copies `fs/sysv` into `reference/linux/`.
Refreshes may replace only that reference tree and must never overwrite future
project-owned `core/`, `linux/` or `windows/` source.

## Completion rule

The current tree is reference evidence only. SysV becomes a Filesystem Support
first-party implementation only when the canonical engine and applicable
adapter have independent implementation and qualification evidence for every
variant claimed.
