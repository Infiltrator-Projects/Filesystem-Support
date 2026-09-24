# EXT2 Filesystem Design

## Purpose

This document describes the EXT2 on-disk format, its important structures and features, and the Filesystem Support implementation architecture.

EXT2 is a non-journalled block-group filesystem. It is deliberately kept distinct from EXT3 and EXT4: an EXT2 driver may understand compatibility fields needed to inspect media, but it must not silently mount a journalled EXT3 volume as EXT2.

## Media layout

EXT2 divides the volume into block groups. The canonical logical layout is:

```text
volume
├── reserved boot area
├── superblock
├── group descriptor table
├── block group 0
│   ├── block bitmap
│   ├── inode bitmap
│   ├── inode table
│   └── data blocks
├── block group 1
│   └── ...
└── block group N
```

For the normal primary layout, the superblock begins 1024 bytes from the start of the filesystem. With 1 KiB blocks it occupies block 1; with larger blocks it resides within block 0. Sparse-super layouts replicate superblock/group-descriptor metadata only in selected groups.

## Superblock

The superblock is the filesystem-wide authority for geometry and feature negotiation. The canonical core decodes and validates at least:

- total inode count;
- total block count;
- reserved/free block counts;
- free inode count;
- first data block;
- logarithmic block size;
- blocks per group;
- inodes per group;
- first non-reserved inode;
- inode size;
- compatible, incompatible and read-only-compatible feature masks.

Filesystem Support accepts block sizes from 1024 through 65536 bytes and validates inode geometry before any later address calculation is trusted.

The format magic is `0xEF53`.

## Block groups and group descriptors

Each group descriptor identifies the group's:

- block bitmap;
- inode bitmap;
- inode table;
- free-block count;
- free-inode count;
- used-directory count.

The driver validates that these structures lie inside the owning group and that metadata blocks are marked allocated. Global free counts and per-group counts must not underflow or overflow during mutation.

## Allocation model

EXT2 uses one bit per block in the block bitmap and one bit per inode in the inode bitmap. Allocation is group-oriented to preserve locality.

Filesystem Support keeps Linux reservation-window policy in the Linux adapter while the canonical core owns host-neutral geometry, block-group bounds, sparse-super placement and absolute-block-to-group mapping.

## Inodes

Classic EXT2 inodes are at least 128 bytes. Important fields include:

- mode/type and permissions;
- uid/gid;
- file size;
- atime/ctime/mtime/deletion time;
- link count;
- allocated-sector count;
- inode flags;
- generation;
- extended-attribute block pointer;
- block mapping array.

The classic mapping array has 15 entries:

```text
0..11   direct block pointers
12      single indirect
13      double indirect
14      triple indirect
```

The canonical engine computes the indirect path and rejects logical blocks outside the addressable range.

## Directories

A directory is a file containing variable-length directory records. A record contains:

- inode number;
- record length;
- name length;
- optional file type when the FILETYPE incompat feature is enabled;
- name bytes.

Record lengths are aligned and may not cross the containing filesystem block. The canonical engine validates insertion, splitting, deletion/coalescing, initial `.` / `..` layout and inode-number bounds.

Indexed-directory compatibility bits may exist on media, but actual indexed-directory behaviour must only be claimed where implemented and qualified.

## Extended attributes and ACLs

EXT2 extended attributes are stored in a filesystem block referenced from the inode's `i_file_acl` field. Filesystem Support validates:

- xattr block magic;
- one-block layout;
- reference count;
- entry bounds and alignment;
- value offsets and padded sizes;
- absence of impossible external-value references for this implementation.

The Linux adapter exposes user, trusted, security and POSIX ACL namespaces and uses copy-on-write when a shared xattr block must be changed.

## Symlinks, links and special files

Small symlink payloads may be stored inline in the inode block array; longer symlinks use data blocks. Hard links share an inode and increment its link count. Device/FIFO/socket types are represented by inode mode and host integration.

## Feature policy

The canonical EXT2 core currently recognises/supports the following important feature classes:

- COMPAT: extended attributes;
- INCOMPAT: file type in directory entries and META_BG;
- RO_COMPAT: sparse superblocks, large files and btree-directory compatibility.

Journal-related feature bits are explicitly detected. A journalled filesystem is rejected as EXT2.

## Crash consistency

EXT2 has no journal. Metadata ordering therefore matters. A crash can leave allocation bitmaps, inode state, directory entries or free counts inconsistent. The design relies on conservative write ordering, synchronous paths where requested, and offline checking after unclean shutdown.

## Filesystem Support source design

Current permanent source responsibilities are:

```text
core/
  ext2_core.c/.h       format decoding, geometry, directory rules
  ext2_engine.c/.h     host-neutral volume/inode/file engine

linux/
  core_bridge.c        compiles canonical core into ext2.ko
  allocation.c         block and inode allocation adapter
  namespace.c          directory iteration and namespace mutation
  io.c                 file/inode/page-cache integration
  metadata.c           xattrs, ACLs and security metadata
  lifecycle.c          mount, superblock and module lifecycle
  linux_adapter.h      Linux-private model and contracts

windows/
  ext2_driver.*        Windows IFS/WDK adapter
```

The same canonical engine is intended to serve both operating systems.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
