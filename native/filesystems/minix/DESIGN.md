# Minix Filesystem Design and Ownership

## Classification

The Minix filesystem family (v1, v2 and v3) is a conventional local block
filesystem. Filesystem Support therefore applies the one-canonical-filesystem
architecture: format and filesystem semantics belong in a host-neutral core,
with thin operating-system adapters around that core.

## Current implementation state

Minix is currently **reference/import state**.

Filesystem Support does not claim the imported Linux Minix implementation as
project-authored code. The pinned upstream source is retained unchanged beneath
`reference/linux/` as compatibility, format and failure-behaviour evidence.

The upstream filenames, licences, copyrights and implementation boundaries are
preserved. Moving the tree out of the retired `kernel/` staging path and into
`reference/linux/` is provenance/layout hygiene only; it is not a rewrite.

## Current layout

```text
native/filesystems/minix/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux Minix evidence
```

When independent implementation begins, project-owned code is added beside the
reference tree:

```text
native/filesystems/minix/
  DESIGN.md
  core/                  canonical Minix v1/v2/v3 format semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/qualification adapter
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future core owns Minix-defined host-neutral behaviour including:

- v1/v2/v3 superblock and geometry decoding;
- inode and zone/block addressing;
- inode/zone allocation bitmaps;
- directory record and filename semantics;
- direct and indirect tree mapping;
- timestamps, modes and filesystem-defined metadata;
- free-space/inode accounting;
- corruption and range validation;
- exact mutation and recovery/ordering rules for any supported writer.

Linux VFS objects, buffer/page-cache mechanics, mount/module lifetime and Linux
credential/error translation belong only in the Linux adapter.

## Version rule

Minix v1, v2 and v3 are variants of one filesystem family. Their format
differences must be explicit state in one canonical Minix engine rather than
three unrelated host implementations.

## File-boundary rule

Upstream files such as `inode.c`, `namei.c`, `bitmap.c` and
`itree_*.c` remain unchanged only under `reference/linux/`.

Project-authored source must be cut around Filesystem Support responsibilities
and portable Minix invariants. Moving, renaming, merging or recommenting copied
Linux source is never evidence that it has crossed the authorship boundary.

## Promotion sequence

1. define the supported Minix v1/v2/v3 media variants and limits;
2. implement bounded host-neutral superblock/inode/bitmap decoding;
3. implement read-only directory and file traversal against independent images;
4. qualify malformed geometry, bitmaps, inode references and indirect trees;
5. add a thin Linux adapter over the canonical core;
6. add mutation only after allocation/publication/recovery rules are explicitly
   implemented and destructively qualified;
7. add Windows against the same canonical core when ready.

## Failure policy

Impossible geometry, invalid bitmap references, out-of-range zones/inodes,
malformed directory entries, cyclic/invalid indirect trees and unsupported
format state must fail closed.

## Reference provenance

The Linux reference refresh workflow pins Linux v6.12.107 and copies
`fs/minix` into `reference/linux/`. Refreshes may update reference evidence
but must never overwrite future project-owned implementation directories.

## Completion rule

The current tree is reference evidence only. Minix is not a Filesystem Support
native implementation until a canonical project-authored engine and applicable
adapter are independently implemented and qualified.
