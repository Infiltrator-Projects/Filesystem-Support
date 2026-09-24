# Amiga FFS Filesystem Design

## Purpose

FFS is the Amiga Fast File System. Filesystem Support maintains it independently from OFS.

FFS keeps the Amiga root/object/bitmap/hash model but removes OFS per-data-block headers. File data blocks are therefore raw payload, improving usable capacity and throughput.

## DOS types and variants

Canonical FFS DOS types include:

- `DOS\1` — FFS;
- `DOS\3` — international FFS;
- `DOS\5` — directory-cache FFS;
- corresponding multi-user variants including `muF\1`, `muF\3`, `muF\5`.

The canonical core classifies DOS type into MUFS, international and directory-cache variant flags.

## Byte order and checksums

Metadata is big-endian. Metadata blocks use Amiga additive checksum rules. Checksums are validated before metadata is trusted and regenerated after mutation.

## Root block

The FFS root block contains the same broad classes of information as OFS:

- type/hash-size/checksum and directory hash table;
- bitmap-valid flag and bitmap block references;
- bitmap extension;
- root/disk timestamps;
- disk name;
- optional directory-cache reference;
- secondary root type.

## File and directory object blocks

Header/tail metadata records carry:

- type and key;
- pointer/hash table;
- checksum;
- uid/gid and protection;
- file size;
- comment/name;
- timestamps;
- hard-link/original metadata;
- hash-chain;
- parent;
- extension pointer;
- secondary object type.

## Raw file data blocks

The defining FFS storage difference is that ordinary file data blocks contain payload directly. There is no OFS data-block header consuming bytes in each block.

Logical file mapping is driven by the pointer table in the object header and, for larger files, extension blocks.

The canonical primitives compute:

- logical block to extension index;
- pointer index inside that extension;
- block count from file size;
- required extension count.

## Directories and hashing

Directories are hashed. Each directory/root block contains a hash table and objects form chains through their hash-chain field.

Filesystem Support owns independent FFS name validation, character folding and directory hash calculation. The traditional DOS-name maximum is 30 bytes for the base format family.

## Allocation bitmap

Free-space allocation uses bitmap blocks referenced from the root and optional bitmap extensions. Canonical FFS primitives provide block range checks, bitmap geometry, bitmap location, final-word masking and contiguous free-run selection.

## Directory cache variants

DirCache variants add cache metadata intended to speed directory enumeration. The cache is an optimisation; authoritative object/header chains remain the correctness source when cache state is invalid or unavailable.

## Links and symlinks

FFS supports hard-link object types and soft-link blocks. Host path syntax is translated through canonical symlink encode/decode helpers rather than embedded in Linux-only code.

## Protection and time

AmigaDOS protection flags and Amiga timestamps are preserved on disk and mapped into host permissions/time representations by the adapter.

## No journal

FFS is not journalled. Mutation ordering must ensure that new blocks are initialised before references become authoritative and that freed blocks are not exposed as free before all live references are removed.

## Filesystem Support source design

```text
core/
  ffs_core.c/.h
  ffs_primitives.c/.h

linux/
  core_bridge.c
  storage.c
  namespace.c
  lifecycle.c
  linux_adapter.h
  disk_layout.h

windows/
  README.md
```

FFS does not call into OFS for filesystem semantics. Similar arithmetic is duplicated intentionally so each filesystem can evolve without silently changing the other.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
