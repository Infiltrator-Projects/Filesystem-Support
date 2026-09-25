# Amiga OFS Filesystem Design

## Purpose

OFS is the Amiga Old File System. Filesystem Support implements it independently from FFS even though both share historical AmigaDOS concepts and some on-disk structure families.

The key distinction is data storage: OFS data blocks carry filesystem headers and chaining metadata; FFS data blocks are raw payload blocks.

## DOS types and variants

Canonical OFS DOS types include:

- `DOS\0` — OFS;
- `DOS\2` — international OFS;
- `DOS\4` — directory-cache OFS;
- corresponding `muF\0`, `muF\2`, `muF\4` multi-user variants.

Variant flags identify MUFS, international-name and directory-cache behaviour.

## Byte order

Amiga filesystem metadata is big-endian. All 32-bit metadata fields must be decoded/encoded explicitly; host byte order must never be assumed.

## Root block

The root block is the central volume object. It contains a head region with:

- primary type;
- hash table size;
- checksum;
- hash-table entries.

Its tail contains:

- bitmap-valid flag;
- up to 25 direct bitmap-block pointers;
- bitmap-extension pointer;
- root-change date;
- disk name;
- disk-change and creation dates;
- optional directory-cache pointer;
- secondary type identifying a root object.

The checksum covers the complete metadata block according to Amiga filesystem checksum rules.

## Object header blocks

Files and directories use header blocks containing:

- primary type;
- object key;
- block count;
- first data reference;
- checksum;
- pointer/hash table.

The tail contains:

- uid/gid;
- Amiga protection flags;
- file size;
- comment;
- modification time;
- name;
- original/link-chain fields;
- directory hash-chain link;
- parent;
- extension block;
- secondary object type.

Secondary types distinguish file, root, user directory, soft link and hard-link forms.

## OFS data blocks

OFS data blocks are structured blocks rather than raw payload. Each contains:

- block type;
- owning file key;
- sequence number;
- data payload size;
- next data-block pointer;
- checksum;
- payload bytes.

The header reduces useful payload per block. Sequence and ownership fields allow stronger structural validation than FFS raw blocks but cost space and throughput.

## File extension blocks

Large files outgrow the pointer table in their header block. Extension blocks continue the file's block pointer list. The canonical core computes logical-file-block position across the header and extension chain and rejects impossible chain geometry.

## Directories

Directory lookup uses a hash table in the directory/root header. Entries are connected through each object's hash-chain field.

Names use AmigaDOS length-prefixed name storage with a traditional maximum of 30 bytes for this format family. International variants modify case folding and hashing behaviour.

## Allocation bitmap

Free-space state is held in bitmap blocks referenced from the root block and optional bitmap-extension blocks.

Filesystem Support canonical primitives define:

- data-block range validation;
- bitmap geometry;
- block-to-bitmap/bit mapping;
- valid-bit masking for the final bitmap word;
- free-run selection.

A bit denotes whether the corresponding disk block is free/allocated according to Amiga bitmap convention.

## Links and symlinks

Hard-link file/directory objects use original/link-chain fields. Soft links store link text in a dedicated structured block. Filesystem Support has canonical symlink translation/encoding so Amiga path syntax is not leaked into host adapters.

## Protection and timestamps

Amiga protection bits include owner/group/other access restrictions plus hidden, script, pure and archived flags. Timestamps use Amiga days/minutes/ticks from the Amiga epoch and are translated by the host adapter.

## No journal

OFS is not journalled. Bitmap updates, object headers, directory hash links and file block chains must therefore be written conservatively. A crash may require external repair.

## Filesystem Support source design

```text
core/
  ofs_core.c/.h
  ofs_primitives.c/.h
  ofs_disk_layout.h      host-neutral OFS on-disk structures

linux/
  core_bridge.c
  storage.c
  namespace.c
  lifecycle.c
  linux_adapter.h

windows/
  README.md              adapter still to be implemented
```

`core/ofs_disk_layout.h` is the canonical host-neutral OFS media-layout contract. The Linux adapter consumes it through portable disk scalar aliases; Linux VFS types remain in `linux/linux_adapter.h`.

There is no shared `amiga_common` implementation layer. OFS owns its own format semantics even when an FFS rule happens to be numerically identical.

## Forensic completion notes against AmigaOS filesystem documentation

### Boot blocks and partition environment

Classic AmigaDOS volumes reserve initial blocks for boot information. The first boot block carries the DOS type identifying the filesystem variant and, when bootable, boot code/checksum state.

For hard-disk partitions the RDB partition environment supplies geometry and filesystem selection. `DOS\0` identifies classic OFS, with related DOS types selecting international and directory-cache variants.

### Root-block structure

For the classic 512-byte layout the root block is a TYPE_SHORT metadata block whose checksum is chosen so the sum of all 32-bit words in the block is zero. Important fields include:

- own key and sequence fields, zero for the root;
- hash-table size;
- checksum;
- directory hash table (72 entries in the 512-byte classic layout);
- bitmap-valid flag;
- 25 direct bitmap-block keys;
- bitmap-extension pointer;
- directory/root modification timestamp;
- volume name stored as a BCPL string, with the classic volume-name limit of 30 characters;
- disk modification and creation timestamps;
- secondary type `ST_ROOT`.

A zero bitmap-valid flag means the allocation bitmap cannot be trusted and validation/rebuild is required before normal writable use.

### User-directory and file header blocks

A directory/file header is also a TYPE_SHORT checksummed block. Directory metadata includes owner/group identity, protection bits, comment, timestamp, BCPL name, hash-chain link, parent and secondary type.

Classic comments use a length-prefixed field with an effective AmigaDOS comment limit of 79 characters.

### Hash chains

A classic 512-byte directory/root has 72 hash buckets. Each bucket points to the first object block and collisions continue through each object's hash-chain field. The object block itself is authoritative; directory-cache variants add a compact cache but do not replace the object/header chain.

### Bitmap validity and extensions

The root directly names a limited set of bitmap blocks. Larger volumes continue that list through bitmap-extension blocks. The bitmap and its validity flag are filesystem consistency state, not merely a cache.

Historical OFS implementations also have size limitations tied to old handling of root fields; Filesystem Support should model the on-disk fields correctly and treat historical implementation limits separately from format decoding.

### Directory-cache OFS

The DOS\4 directory-cache variant adds directory-list blocks linked from root/user-directory metadata. These blocks contain compact directory-entry summaries and their own checksums. They must remain consistent with the authoritative object blocks; if not, validation/rebuild is required.

### OFS data-block distinction

The structured OFS data block described earlier is the defining OFS/FFS split: OFS stores type, owner key, sequence, payload size, next pointer and checksum in every file data block. A valid OFS reader can therefore cross-check file ownership/sequence through the data chain instead of treating the entire block as payload.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
