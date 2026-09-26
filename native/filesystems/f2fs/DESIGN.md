# F2FS Design and Ownership

## Classification

F2FS is a conventional local flash-oriented filesystem with checkpoint,
segment, node and garbage-collection semantics. Filesystem Support must
implement those semantics once in a host-neutral core.

## Current state

F2FS is currently **reference/import state**. The Linux implementation under
`reference/linux/` is preserved unchanged as upstream evidence and is not
project-authored source.

## Current directory contract

While F2FS remains in reference/import state, the filesystem root contains only
`DESIGN.md` and `reference/`, with the pinned upstream Linux implementation
under `reference/linux/`. The future `core/`, `linux/` and `windows/`
directories appear only with independently authored implementation. `kernel/`
and `userspace/` are not valid alternate source trees.

CI enforces this boundary and rejects accidental production linkage of the
imported reference source.

## Target layout

```text
native/filesystems/f2fs/
  DESIGN.md
  core/                 canonical F2FS format/filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The core owns F2FS-defined superblock/checkpoint validation, segment/NAT/SIT
state, node addressing, allocation, summary blocks, recovery, directory/hash
rules, inline data/xattrs, compression-format interpretation, zoned/flash
geometry policy where format-defined, and exact mutation/recovery invariants.

Linux VFS objects, bios/folios, writeback, workqueues, sysfs/debug plumbing and
kernel-specific crypto/verity hooks are adapter mechanisms.

## Promotion sequence

Begin with bounded host-neutral format/checkpoint decoding and read-only node,
directory and data traversal. Qualify independently manufactured images and
malformed checkpoint/segment metadata. Add writes, GC, checkpoint publication
and recovery only after explicit durability/destructive qualification.

## Failure policy

Invalid checkpoint packs, contradictory NAT/SIT/segment state, bad node
addresses, unsupported incompat features and unsafe recovery state fail closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/f2fs` to
`reference/linux/`. It must never overwrite future project-owned source.
