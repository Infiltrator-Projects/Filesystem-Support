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

### Root and root-info structures

The canonical root/root-info records already documented are the minimum low-level structures known to the current core. A complete SFS2 implementation must additionally document the SFS2 versions of:

- object containers and object records;
- hash tables;
- node containers/object-node mapping;
- extent B-tree nodes;
- bitmap blocks;
- admin-space containers;
- soft-link representation;
- transaction/recovery publication rules.

Until those structures are independently validated against real SFS\2 media and the shipping handler, this document is complete as an audit of the current canonical model but not yet a complete writer specification.

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
