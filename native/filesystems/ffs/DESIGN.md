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

## Forensic completion notes against AmigaOS filesystem documentation

### Boot blocks and DOS types

Classic AmigaDOS volumes begin with boot blocks. The first boot block carries the filesystem DOS type plus boot/checksum information when the volume is bootable.

`DOS\1` is classic FFS. International and directory-cache modes use the related odd-numbered DOS types (`DOS\3` and `DOS\5`). AmigaOS 4 documentation also defines later FFS2/LNFS forms separately; those must not be conflated with this classic FFS implementation.

### Supported user-visible limits

For the AmigaOS FFS family, the official user documentation records:

- classic FFS filenames: 30 characters;
- maximum file size: 4 GiB minus 2 bytes;
- supported block sizes: 512 through 32768 bytes;
- large partition capacity varies with block size/implementation, with modern AmigaOS FFS reaching multi-terabyte ranges.

Those are product/driver limits layered over the basic metadata layout and should be kept distinct from what a parser can numerically decode.

### Root-block and directory-header details

For a classic 512-byte root block:

- type is TYPE_SHORT;
- root own-key and sequence values are zero;
- hash-table size is 72 longwords;
- checksum balances all 32-bit words to zero;
- 72 directory hash buckets follow;
- bitmap-valid state and up to 25 direct bitmap keys are stored in the root;
- a bitmap-extension pointer continues the bitmap list;
- root/disk timestamps and the 30-character BCPL volume name are stored near the tail;
- secondary type is `ST_ROOT`.

User-directory blocks carry the same checksum/hash-table concept plus owner/group, protection, comment, timestamp, name, hash-chain, parent and secondary type.

### Bitmap validity

A root bitmap-valid value of -1 denotes a valid allocation bitmap in the classic format; zero means validation is required. Filesystem Support must not allocate from an invalid bitmap merely because its individual block contents appear plausible.

### Raw-data difference from OFS

FFS file data blocks are raw payload. Ownership, logical ordering and continuation live in the file header/extension pointer arrays rather than per-data-block headers. This is the central reason FFS avoids the per-block payload overhead of OFS.

### Directory-cache FFS

`DOS\5` adds directory-list/cache blocks. They are a derived acceleration structure containing compact entry metadata. The ordinary object/header blocks and hash chains remain the authoritative namespace.

### Later long-name FFS is a different format mode

AmigaOS later introduced long-name/FFS2 modes that alter directory-entry storage and can support 107-character names. Classic FFS remains the 30-character format described here. Filesystem Support must only claim long-name compatibility if those altered structures are independently implemented.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
