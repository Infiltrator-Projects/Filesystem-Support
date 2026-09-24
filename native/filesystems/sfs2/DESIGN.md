# Amiga SFS2 Filesystem Design

## Purpose

SFS2 is maintained as a distinct filesystem, not an SFS compatibility switch. It preserves the general structured-block/tree approach of SFS while changing root identity, structure version and several record sizes/capabilities.

## Format identity

Key canonical constants are:

- root ID: `0x53465302` (`SFS\2`);
- structure version: 4;
- block-header size: 12 bytes;
- root record size used by the canonical decoder: 128 bytes;
- root-info size: 36 bytes;
- fixed object prefix: 27 bytes;
- extent-node size: 16 bytes;
- maximum filename length: 107 bytes;
- maximum encoded file size: `0x0000FFFFFFFFFFFF` (48 bits).

## Root block

The canonical root record contains:

- block ID;
- block checksum;
- block self pointer;
- version;
- sequence;
- creation date;
- 64-bit first byte of partition;
- 64-bit last byte of partition;
- total blocks;
- block size;
- bitmap base;
- admin-space container;
- root object container;
- extent B-node root;
- object-node root.

All references are range-checked against the decoded filesystem geometry.

## Redundant roots

As with SFS, multiple root copies are ranked by sequence number. Selection occurs only after each candidate independently passes identity, version, block-size, count and pointer validation.

## Root information

The 36-byte root-info record contains:

- deleted blocks;
- deleted files;
- free blocks;
- creation date;
- last allocated block;
- last allocated admin-space block;
- last allocated extent node;
- last allocated object node;
- roving pointer.

Free/cached counters must remain within total filesystem capacity.

## File size representation

SFS2 uses a 48-bit file size split into a 32-bit high portion and 16-bit low portion in its object representation. The canonical core owns encode/decode and rejects sizes above the format maximum.

## Object records

Objects retain a variable-length name/comment tail after a fixed prefix. The canonical parser validates:

- available record space;
- name termination;
- filename maximum;
- comment termination;
- record-size overflow.

## Extents and trees

SFS2 uses extent-based file mapping and object/node tree structures. The current canonical engine validates extent key, next pointer, block count and total-media range.

Further tree mutation belongs in the SFS2 canonical core as the implementation grows; it must not be implemented independently in Linux and Windows wrappers.

## Bitmap layout and allocation

Bitmap geometry is derived from block size and total blocks. Allocation code must preserve cached free counts, metadata reserve requirements and on-disk checksum/self-pointer invariants.

## Checksums and structured blocks

Structured blocks carry an ID, checksum and self pointer. The canonical validator recomputes the checksum and confirms physical-location identity before accepting the block.

## Byte order and shared structure family

All multi-byte SFS2 metadata fields are big-endian.

SFS2 reuses the SFS structured-block architecture. Unless listed as changed below, the corresponding block class keeps the SFS form and role:

- `ADMC` — admin-space container;
- `OBJC` — object container;
- `HTAB` — directory hash table;
- `NDC ` — object-node/index container;
- `BNDC` — B-tree node container;
- `BTMP` — allocation bitmap;
- `SLNK` — soft-link payload;
- `TRST` — transaction storage;
- `TRFA` — incomplete-transaction marker;
- `TROK` — clean transaction marker.

Every structured metadata block starts with the 12-byte ID/checksum/self-pointer header. Ordinary file data does not require that metadata header.

## Exact SFS2 deltas from SFS0

Handler-compatible format evidence identifies these SFS2 disk-layout changes:

- root ID changes from `SFS\0` to `SFS\2`;
- root structure version changes from 3 to 4;
- each object fixed header gains a 16-bit `sizeh` field after the normal 32-bit size/directory field and before modification time;
- the object fixed prefix therefore grows from 25 to 27 bytes;
- a file's 48-bit size is `(size32 << 16) | sizeh`; `size32` is the high 32 bits and `sizeh` the low 16 bits;
- an extent leaf node grows from 14 to 16 bytes;
- the SFS2 checksum result is one less than the corresponding SFS0 checksum.

Directory objects keep the same union positions; the extra 16-bit field still changes all following offsets and must be skipped before reading modification time, object bits, name and comment.

## Object containers and object records

An `OBJC` block contains the standard header, parent object-node number, next and previous object-container block numbers, then packed variable-length objects.

SFS2 objects begin on 2-byte boundaries. Their fixed 27-byte prefix contains owner uid/gid, object-node number, protection, file-data/directory-hash pointer, 32-bit size/directory-first-block field, 16-bit `sizeh`, modification time and object bits. A NUL-terminated name and NUL-terminated comment follow, with padding to the next 2-byte boundary where required.

Object bits cover hidden, undeletable, quick-directory, link/hard-link-reserved and directory state. Soft links use link state plus `SLNK` payload storage.

## Directory hash tables

An `HTAB` block contains the standard header, the parent object-node number and an array of object-node numbers that head hash chains. A directory may legally have no hash block; lookup must then scan the object-container chain.

## Object-node tree

`NDC ` blocks map stable object-node numbers to the object-container blocks holding the objects. A container records the first represented node number and node-range information; leaf levels hold object-node records while index levels hold child block references.

An object-node record carries a data/block reference, next hash-chain node and a 16-bit name hash. This indirection lets object containers move without changing object identity.

## Extent B-tree

`BNDC` blocks contain a B-tree header with node count, leaf/internal state and node size. Internal nodes map keys to child blocks. SFS2 extent leaves are 16 bytes. Node count, node size, ordering, child references, run length and physical range are validation invariants.

## Admin space and bitmap

`ADMC` blocks form a linked list of metadata-reservation descriptors. Each descriptor identifies an admin-space region and contains a bit field for metadata blocks in use.

`BTMP` is the standard 12-byte block header followed by bitmap words. One bitmap block covers `(block_size - 12) * 8` filesystem blocks, with set bits meaning free.

## Soft links

`SLNK` carries the standard block header, parent/next/previous linkage and the link string. Host path conversion is an adapter/canonical-engine concern; it is not a second on-disk format.

## Transaction and recovery protocol

SFS2 retains the SFS `TRST`/`TRFA`/`TROK` transaction model. A clean formatted volume carries `TROK` at the transaction-marker position. An interrupted recoverable update uses `TRFA` pointing to a linked `TRST` operation stream. Recovery reconstructs and reapplies that operation stream before restoring the clean marker state.

All recovery writes use SFS2's version-4 root rules, 27-byte object layout, 16-byte extent-node layout and SFS2 checksum rule.

## Normal empty-volume topology

```text
block 0                         primary SFS\2 root (version 4)
admin-space start               ADMC
root object block               OBJC + RootInfo
root + 1                        HTAB
root + 2                        TROK
root + 3                        BNDC extent root (16-byte leaf nodes)
root + 4                        NDC object-node root
root + 5                        .recycled OBJC when enabled
bitmap base ...                 BTMP blocks
last filesystem block           secondary SFS\2 root
```

Both root copies are validated independently before sequence selection.

## SFS2 checksum rule

The checksum field is zero during calculation and SFS words are interpreted in big-endian form. SmartFilesystem 1.279-compatible output uses a whole-block sum of `0xFFFFFFFE` for `SFS\2`, one less than the `0xFFFFFFFF` SFS0 convention.

This is a material format distinction. Applying the SFS0 checksum rule unchanged to SFS2 metadata is not format-correct.

## Filesystem Support source design

Current state:

```text
core/
  sfs2_core.c/.h        canonical root/root-info, object and extent rules

linux/
  README.md             Linux adapter not yet implemented

windows/
  README.md             Windows adapter not yet implemented
```

The canonical core is already independent from SFS. Future adapters must compile and call this core rather than creating a second SFS2 implementation.

## Forensic completion notes against current SFS2 product documentation

### SFS2 is not simply "SFS with a larger integer"

The current SFS distribution and AmigaOS documentation identify SFS\2 (`0x53465302`) as the large-file/large-volume format. It is not compatible with an SFS\0 volume merely by toggling a mount option.

The user-visible compatibility baseline is:

- filename limit: 107 characters;
- recommended block size: 512 bytes, although other block sizes are supported by the handler;
- nominal partition limit: 1 TiB at 512-byte blocks, scaling with block size in the distributed handler (documentation gives up to 64 TiB at 32 KiB blocks);
- official AmigaOS table file-size limit: 1 TiB;
- bootable support in the AmigaOS SFS handler;
- no recovery tools in the AmigaOS table for SFS2.

### Encoded width versus supported limit

The current Filesystem Support core models the SFS2 file-size field as 48 bits (`high_32 + low_16`) and therefore has a much larger numerical encoding ceiling than 1 TiB.

That 48-bit encoding ceiling is not the same thing as the officially supported SFS2 file-size limit. The design document must preserve both facts:

1. the field representation decoded by the core;
2. the supported/qualified filesystem limit.

The writer must not advertise the full 48-bit numerical range until authoritative format evidence and cross-implementation tests show that values above the published limit are valid.

### Low-level structure cross-check status

The low-level structure gap from the first documentation pass is now closed for the public/primary implementation surface. Object/container, hash, node, B-tree, bitmap, admin-space, soft-link and transaction block families were cross-checked against the AROS SFS implementation, while the exact SFS2 deltas and empty-volume output were cross-checked against a formatter documented as byte-for-byte verified against SmartFilesystem 1.279.

That closes the documentation gap. It does not mean the current Filesystem Support SFS2 core implements every writer path; implementation completeness remains a separate engineering and qualification status.

### Compatibility warning

Historical "SFS2" implementations existed before the large-file SFS\2 format used by later SFS releases. Contemporary SFS release notes explicitly warned that the newer SFS\2 format was not compatible with older SFS2 variants. Qualification fixtures must therefore record the producing implementation/version and not assume every volume labelled "SFS2" shares one disk format.

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
- AROS SFS low-level structure and transaction implementation: `aros-development-team/AROS/rom/filesys/SFS/FS/`.
- SFS2 handler-compatible layout deltas and format topology: `ChuckyGang/AmiPart/src/nativefmt.c`, documented there as byte-for-byte verified against SmartFilesystem 1.279 under AmiFUSE.
- Independent parser corroboration: `aaru-dps/Aaru/Aaru.Filesystems/SFS/` for version 4, 27-byte object layout, `sizeh` position and 48-bit file-size assembly.

These sources establish the documented structure surface. Handler-produced real-media fixtures remain required before Filesystem Support advertises complete writable support.
