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
  ext2_driver.c          WDK translation-unit shell
  ext2_driver.h          Windows-private adapter model/contracts
  adapter_support.inc    object lifetime, locking and canonical-core block-I/O bridge
  name_translation.inc   Unicode/path conversion, information helpers and IRP buffers
  file_dispatch.inc      create/read/write/cleanup/close and file-information IRPs
  directory_dispatch.inc directory enumeration/control IRPs
  volume_lifecycle.inc   mount/verify/lock/dismount and filesystem/device control
  driver_entry.inc       DriverEntry and major-function registration
```

The canonical core also owns the host-neutral allocation-map invariants,
data-block range guards, extended-attribute block validation/hash rules and
POSIX-ACL on-disk sizing used by the Linux adapter. Linux retains buffer-cache
access, quota/credential policy, VFS xattr namespace exposure and publication;
those host mechanisms call the canonical format rules rather than maintaining
private EXT2 disk semantics.

The same canonical engine serves both operating systems. The Windows adapter is
source-cut by responsibility but deliberately remains one WDK translation unit
at this stage. That preserves established static helper and object-lifetime
behaviour while eliminating the monolithic source boundary. Reusable IFS
mechanics can later move into `native/platform/windows/` only when doing so
does not pull EXT2 semantics out of `core/`.

## Forensic completion notes against the kernel EXT2 specification

### Byte order and block-size distinction

EXT2 metadata is little-endian on disk.

The historical Linux EXT2 documentation describes normal filesystem block sizes of 1024, 2048 and 4096 bytes, with 8192-byte support on Alpha systems. The current Filesystem Support canonical validator accepts geometry up to 65536 bytes because the EXT-family structures can be represented at that size, but that implementation acceptance range must not be confused with the historical Linux EXT2 compatibility guarantee. Media above the documented historical range requires explicit interoperability qualification before it is advertised as EXT2-compatible.

Each block bitmap occupies one filesystem block and therefore describes at most `block_size * 8` blocks. The same one-block rule applies to the inode bitmap, which bounds the number of inodes represented by one group.

### Superblock state beyond basic geometry

A complete EXT2 superblock description also includes:

- mount time and last write time;
- current mount count and maximum mount count;
- filesystem state flags (clean/error state);
- error-handling policy;
- revision level;
- creator operating-system field;
- UUID and volume label;
- last mounted path;
- reserved-block policy and default uid/gid;
- feature masks and algorithm/usage fields added by dynamic revision.

Revision 0 uses the original fixed inode geometry. Dynamic revision adds variable inode size, first non-reserved inode and feature negotiation.

### Backup superblocks

Without sparse-super, backup superblocks and group descriptor copies occur in every block group. With sparse-super they occur in groups 0 and 1 and groups whose numbers are powers of 3, 5 or 7.

### Reserved and special inodes

The traditional reserved range begins with inode 1. Important conventional inode numbers include:

- 1 — bad-block inode;
- 2 — root directory;
- 5 — boot-loader inode;
- 6 — undelete directory;
- 11 — traditional first non-reserved inode for revision 0.

Dynamic revision can store a different `s_first_ino`.

### Inode details and format-reserved capabilities

The inode format includes mode/type, uid/gid, low/high file size fields, atime/ctime/mtime/dtime, link count, 512-byte-sector count, flags, generation, xattr/ACL location and the 15-entry block map.

The format also reserves or historically defined fields/flags for fragments, secure deletion, undelete, compression, synchronous updates, immutable/append-only files, no-atime, directory indexing and per-inode journalling hints. A format field existing does not mean Filesystem Support implements the corresponding behaviour.

The 60-byte `i_block` area can also contain a short ("fast") symbolic-link payload instead of block pointers when the symlink is small enough.

### Directory entry details

Classic directory entries contain inode, record length, name length and name. When FILETYPE is enabled the high byte formerly used by the 16-bit name length is reused as a file-type code. Standard codes cover unknown, regular file, directory, character device, block device, FIFO, socket and symbolic link.

The format name limit is 255 bytes. Empty space inside a directory block is represented by record-length slack or entries whose inode number is zero; records must still remain correctly aligned and contained inside one filesystem block.

### Allocation locality and free-space invariants

The format groups inode table, inode bitmap and block bitmap with nearby data to improve locality. A valid group descriptor must not place these metadata structures outside the group, and bitmap bits for the metadata itself must be allocated. Superblock/global free counts are cached summaries and must agree with the bitmaps after a clean check.

### What EXT2 deliberately does not provide

EXT2 itself has no journal and therefore has no committed-transaction replay mechanism. Space exists in the broader format evolution for features such as journalling, compression, ACLs and other extensions, but a strict EXT2 mount must reject incompatible journal/recovery state rather than treating it as plain EXT2.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
