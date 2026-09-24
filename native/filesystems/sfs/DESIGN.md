# Amiga Smart File System (SFS) Design

## Purpose

SFS is an extent-oriented Amiga filesystem with structured metadata blocks, redundant roots, bitmap/admin-space allocation structures, object containers and tree-based mapping.

Filesystem Support treats SFS and SFS2 as different formats with separate canonical engines.

## Fundamental block header

Every structured metadata block begins with a 12-byte header:

- 32-bit block ID;
- 32-bit checksum;
- 32-bit self/own-block number.

The self pointer is validated against the physical block being read. This prevents a valid block image from being accepted at the wrong location.

Important block IDs include:

- root: `SFS\0`;
- object container: `OBJC`;
- B-node container: `BNDC`;
- node container: `NDC `;
- hash table: `HTAB`;
- soft link: `SLNK`;
- admin-space container: `ADMC`;
- bitmap: `BTMP`;
- transaction storage: `TRST`;
- transaction failure: `TRFA`;
- transaction complete/idle marker: `TROK`.

## Root blocks

SFS uses redundant root blocks. Each root contains:

- generic block header;
- structure version;
- sequence number;
- creation date;
- root option bits;
- partition first/last byte range;
- total block count;
- filesystem block size;
- bitmap base;
- first admin-space container;
- root object container;
- extent B-tree root;
- object-node tree root.

The current canonical structure version is 3 and the minimum block size is 512 bytes.

When both root copies are valid, the copy with the newer sequence number is authoritative.

## Root information

Root information maintains allocation and format state including:

- deleted-block count;
- deleted-file count;
- cached free blocks;
- creation date;
- most recently allocated block;
- most recently allocated admin-space area;
- most recently allocated extent node;
- most recently allocated object node;
- roving allocation pointer.

Cached counters are validated against physical geometry before they are trusted.

## Objects

An SFS object represents a file, directory or special object. Its fixed fields include:

- owner uid/gid;
- object-node number;
- protection flags;
- file data node + size, or directory hash table + first directory block;
- modification date;
- object type bits;
- variable name and comment.

The canonical object record parser validates terminators, maximum name length and record-size arithmetic.

The published SmartFileSystem 1.279 filename limit is 107 characters. Filesystem Support now uses the same 107-byte limit in both the canonical core and Linux adapter and tests the exact 107/108 boundary.

## Object containers

Object records are packed into object-container blocks. Containers have:

- metadata block header;
- parent object;
- next container;
- previous container;
- variable object records.

A directory can therefore span a linked chain of object containers.

## Hash tables

Directories may use dedicated hash-table blocks. Hash entries reference object chains. SFS name semantics include canonical character folding/lowercasing and 16-bit component hashing.

## Object-node tree

Object numbers are resolved through node-container structures. Leaf object nodes include:

- data pointer;
- next node;
- 16-bit name hash.

Index containers map node-number ranges to child containers. The canonical engine validates slot computation to prevent tree-index overflow.

## Extent B-tree

File data is represented by extents. Extent B-nodes carry:

- logical key;
- next/previous extent-node relation;
- block count.

B-node containers can be internal or leaf nodes. The core validates node count, node size, per-block capacity and extent physical ranges.

## Allocation bitmap

SFS bitmap blocks indicate free/used blocks. The canonical core provides:

- bitmap layout calculation;
- find-set/find-zero operations;
- bounded bit-range set/clear;
- free-count update with overflow/underflow protection.

## Admin space

Admin-space containers track regions reserved for filesystem metadata. Each admin-space descriptor identifies a region and a bit mask of blocks in use.

The allocator keeps a reserve of free blocks so metadata operations do not deadlock at zero free space. Linux adapter policy currently uses an always-free reserve and chunked allocation.

## Checksums

Structured blocks are checksummed. Validation checks the block header ID, self pointer and checksum before consuming the structure.

## Transaction and crash-recovery protocol

SFS does not use an EXT-style redo journal. Its transaction layer records the set of block modifications required to move from one valid filesystem state to another.

A transaction operation identifies a target block and carries replacement/original-state data. Operation flags distinguish at least a block with no original allocation (`OI_EMPTY`) and a target that no longer needs to be written because it was deleted (`OI_DELETE`).

When a transaction must be recoverable, the operation stream is serialized into one or more `TRST` transaction-storage blocks. Each `TRST` block has the standard SFS block header, a pointer to the next storage block and transaction bytes.

The fixed transaction-marker location is two blocks after the root object container in the normal formatted layout. In the clean state it contains a checksummed `TROK` block. During recoverable publication it can contain a `TRFA` block whose payload points to the first `TRST` block.

Mount/startup recovery of a valid `TRFA` marker is:

1. follow and validate the `TRST` chain;
2. reconstruct the saved block operations;
3. re-apply those operations to their target blocks;
4. only after successful replay replace/remove the failure marker so the volume returns to the clean `TROK` state.

Transaction-storage pointers, target block numbers, compressed record lengths and chain termination are part of the corruption boundary. Malformed recovery data must never become an arbitrary disk write.

## Canonical formatted-volume topology

The AROS format path and SmartFilesystem 1.279-compatible format output establish the normal empty-volume topology:

```text
block 0                         primary SFS root
admin-space start               ADMC
root object block               OBJC containing root object + RootInfo
root + 1                        HTAB for the root directory
root + 2                        TROK transaction marker
root + 3                        BNDC empty extent B-tree root
root + 4                        NDC object-node root
root + 5                        OBJC for .recycled when enabled
bitmap base ...                 BTMP blocks
last filesystem block           secondary SFS root
```

The exact absolute admin/root location depends on reserved-start geometry; the relationships above are format topology, not permission to infer locations without validating the root.

One `BTMP` block covers `(block_size - 12) * 8` filesystem blocks because the first 12 bytes are the standard block header. SFS bitmap bits use set = free.

## Checksum convention

SFS metadata uses additive 32-bit big-endian checksums with the checksum field zeroed during calculation. SmartFilesystem 1.279-compatible format output uses a whole-block sum of `0xFFFFFFFF` for `SFS\0`. SFS2 has a variant-specific one-less checksum rule described in the SFS2 design.

Checksum behaviour must be tested against handler-produced fixtures; it must not be replaced with a host-native checksum or an EXT-style CRC.

## Filesystem Support source design

```text
core/
  sfs_core.c/.h

linux/
  core_bridge.c
  allocation.c
  mapping.c
  io.c
  namespace.c
  lifecycle.c
  linux_adapter.h
  disk_layout.h

windows/
  README.md
```

The former ASFS file boundaries are not permanent project interfaces.

## Forensic completion notes against the current Smart File System distribution

### Published SFS0 limits

The current SFS distribution documents the SFS\0 format with these user-visible limits/capabilities:

- filesystem block sizes from 512 through 32768 bytes, with 512 recommended for performance;
- file and directory names up to 107 characters;
- volume names up to 30 characters;
- comments up to 79 characters;
- file size approximately 4 GiB (official AmigaOS tables state 4 GiB minus 2 bytes);
- SFS\0 partition size up to 128 GiB;
- 64-bit device access through NSD/TD64 for partitions that cross the classic 4 GiB device boundary;
- soft links;
- configurable case-sensitive or case-insensitive naming;
- transparent online defragmentation/optimisation;
- a recycled/deleted-files directory;
- no hard-link support in the distributed implementation.

`IFS_SFS_MAX_FILENAME` and the Linux adapter's `ASFS_MAXFN` are both 107. Qualification tests accept 107-byte names and reject 108-byte names. Historical ports that imposed smaller policy limits remain implementation policy rather than a format limit.

### Root placement and redundancy

The format has two root copies, one near the start and one near the end of the filesystem. They carry equivalent structural information and are selected by validity plus sequence number. The root records partition byte bounds as 64-bit high/low halves, which allows the implementation to detect moved/resized partition boundaries independently of block count.

### Root option bits

Important root behaviour bits include case sensitivity, read-only state and volume-name case behaviour in addition to structural version/sequence data. These bits affect namespace semantics and must be preserved across rewrites.

### Metadata block classes

A complete SFS structure inventory includes the block classes already named in this document plus:

- hash-table blocks for directory lookup;
- soft-link blocks;
- transaction-failure blocks;
- root-information state associated with allocation/deletion counters.

Each structured metadata block carries ID, checksum and self block number.

### Object types

Object bits distinguish at least directory, link/hardlink-related state and hidden objects. The distributed filesystem currently advertises soft links while hard-link operations are not implemented; on-disk type values must therefore be parsed independently from whether mutation support is exposed.

### Safe-write behaviour

The distributed filesystem explicitly claims crash-safe metadata modification: after reset/crash/power loss the volume should not require the long validation process associated with classic FFS, and at worst the newest modifications may be lost.

That behavioural guarantee means the transaction/cache write ordering is part of SFS format correctness. The presence of a transaction-failure block ID alone is not a complete recovery model; Filesystem Support must document and qualify the exact commit/publication protocol as the canonical writer is implemented.

### Recycled/deleted files

Current SFS distribution documentation describes a special directory containing up to 350 recently deleted files. This is format-visible behaviour and belongs in the complete filesystem feature description even if the current Linux migration code does not yet expose all management operations.

### Read-ahead and optimiser

Read-ahead caching and transparent defragmentation are implementation features rather than fundamental on-disk structures, but they are part of the expected SFS feature surface and should be tracked in feature-completeness tests.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.

## Verification references

- SmartFileSystem 1.279 distribution/readme: https://aminet.net/package/disk/misc/SFS
- AROS SFS low-level implementation: `aros-development-team/AROS/rom/filesys/SFS/FS/`, especially `blockstructure.h`, `objects.h`, `nodes.h`, `btreenodes.h`, `adminspaces.h`, `bitmap.h`, `transactions.h`, `transactions.c` and `filesystemmain.c`.
- SmartFilesystem 1.279 handler-compatible formatter evidence: `ChuckyGang/AmiPart/src/nativefmt.c`, documented there as byte-for-byte verified against the handler under AmiFUSE.

Where historical ports and the current SmartFilesystem handler expose different policy limits, this document records the format/handler distinction rather than silently adopting one implementation constant.
