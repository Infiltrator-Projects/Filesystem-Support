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

linux/
  core_bridge.c
  storage.c
  namespace.c
  lifecycle.c
  linux_adapter.h
  disk_layout.h

windows/
  README.md              adapter still to be implemented
```

There is no shared `amiga_common` implementation layer. OFS owns its own format semantics even when an FFS rule happens to be numerically identical.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
