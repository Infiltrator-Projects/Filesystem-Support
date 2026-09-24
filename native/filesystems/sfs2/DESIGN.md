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

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
