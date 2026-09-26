# Btrfs Design and Ownership

## Classification

Btrfs is a conventional local copy-on-write filesystem with checksums,
snapshots/subvolumes and multi-device capabilities. It belongs in the normal
canonical-engine architecture.

## Current state

Btrfs is currently **reference/import state**. The source under
`reference/linux/` is the pinned upstream Linux implementation and is not
project-authored Filesystem Support code.

The reference tree is intentionally preserved with upstream filenames,
licences, copyright and subsystem boundaries.

## Current directory contract

While Btrfs remains in reference/import state, the filesystem root contains only
`DESIGN.md` and `reference/`; the copied Linux tree remains under
`reference/linux/` with upstream filenames and boundaries intact. The future
`core/`, `linux/` and `windows/` directories are created only when an
independent rewrite actually begins. `kernel/` and `userspace/` are not valid
shortcuts around that promotion boundary.

CI enforces this present-state layout and rejects accidental linkage of the
reference tree into the production CMake graph.

## Target layout

```text
native/filesystems/btrfs/
  DESIGN.md
  core/                 canonical Btrfs filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows IFS/WDK adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future host-neutral core owns Btrfs-defined structures and behaviour:
superblocks/chunk trees, key and B-tree semantics, extents/backreferences,
checksums, copy-on-write transactions, subvolumes/snapshots, device/chunk
mapping, allocation, compression format handling, namespace/metadata rules and
filesystem-defined recovery/repair invariants.

Linux folios, bios, workqueues, VFS objects, kernel locking and mount/module
lifetime remain Linux-adapter concerns.

## File-boundary rule

Upstream files such as `ctree.c`, `inode.c`, `extent-tree.c`,
`tree-log.c`, `volumes.c` and related headers remain reference filenames.
Project-authored files must be cut around Filesystem Support ownership and
portable invariants rather than copied/renamed from Linux.

## Promotion sequence

The rewrite begins with bounded format/feature decoding and read-only tree,
chunk/device and extent traversal, qualified against independent images and
malformed-media cases. Mutation, snapshots/subvolumes, multi-device writes and
repair require separate durability/recovery qualification before they can be
claimed.

A Linux adapter and any future Windows adapter must consume the same core.

## Failure policy

Unsupported incompatibility features, invalid tree keys/levels, checksum
failures, contradictory chunk/device maps, out-of-range extents and unsafe
transaction/log state fail closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/btrfs` to
`reference/linux/`. Reference refreshes must never overwrite project-owned
implementation directories.
