# NILFS2 Design and Ownership

## Classification

NILFS2 is a conventional local log-structured block filesystem with continuous
checkpoint/snapshot and segment-cleaning semantics. Filesystem Support therefore
applies the one-canonical-filesystem architecture.

## Current implementation state

NILFS2 is currently **reference/import state**.

The Linux NILFS2 implementation beneath `reference/linux/` is pinned upstream
source retained as format, interoperability and recovery evidence. It is not
project-authored Filesystem Support code. Its original filenames, licences and
source boundaries remain intact.

Moving that source out of the retired `kernel/` staging directory is
provenance/layout hygiene only; it is not a rewrite.

## Current layout

```text
native/filesystems/nilfs2/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux NILFS2 evidence
```

A future independent implementation is added beside it:

```text
native/filesystems/nilfs2/
  DESIGN.md
  core/                  canonical NILFS2 format/filesystem semantics
  linux/                 thin Linux VFS/block adapter
  windows/               thin Windows adapter when implemented
  userspace/             optional image/qualification adapter
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

The future core owns NILFS2-defined host-neutral behaviour including superblock
and geometry validation, segment summaries, checkpoints, DAT/ifile/cpfile/
sufile metadata, inode/directory semantics, block mapping, segment construction,
continuous checkpoint publication, recovery, segment cleaning/GC invariants and
filesystem-defined corruption policy.

Linux VFS objects, page/buffer cache, block-device plumbing, sysfs, workqueues
and module lifetime remain adapter concerns.

## Promotion sequence

1. define supported NILFS2 format/features and checkpoint rules;
2. implement bounded superblock/checkpoint/segment decoding;
3. implement read-only inode/directory/data traversal against independent media;
4. qualify malformed segment/checkpoint and recovery-state fixtures;
5. add a thin Linux adapter;
6. add mutation, segment construction and cleaner interaction only after
   durability/recovery ordering is explicitly implemented and destructively
   qualified;
7. add Windows against the same canonical core when ready.

## Failure policy

Invalid segment summaries, contradictory checkpoint/DAT state, out-of-range
blocks/inodes, malformed metadata trees and uncertain recovery state must fail
closed.

## Reference provenance

The Linux reference refresh workflow pins Linux v6.12.107 and copies
`fs/nilfs2` into `reference/linux/`. Refreshes may update reference evidence
but must never overwrite future project-owned implementation directories.

## Completion rule

The current tree is reference evidence only. NILFS2 is not a Filesystem Support
native implementation until the canonical core and applicable adapter are
independently implemented and qualified.
