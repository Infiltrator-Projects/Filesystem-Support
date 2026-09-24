# Amiga PFS3 Filesystem Design

## Purpose

PFS3 is an Amiga filesystem family based around reserved metadata blocks, anodes, directory blocks, optional extension metadata and mode bits controlling format capabilities.

Filesystem Support's canonical PFS3 core currently recognises PFS1/PFS2 disk identities used by the supported format family and validates the structures required to extend toward a complete PFS3 implementation.

## Disk identity

Recognised disk types include:

- `PFS\1` — `0x50465301`;
- `PFS\2` — `0x50465302`.

The root record carries an option/mode word. Filesystem Support defines mode bits for:

- hard-disk layout;
- split anodes;
- directory extension;
- delete directory;
- explicit size field;
- extension block;
- datestamp support;
- super index;
- super delete directory;
- extended roving allocation;
- long filenames;
- large files;
- stored geometry.

Invalid combinations are rejected by root-geometry validation.

## Root record

The canonical root record contains:

- disk type;
- options/mode flags;
- datestamp;
- creation day/minute/tick;
- protection;
- 32-byte disk-name field;
- first and last reserved metadata block;
- reserved-block free count;
- reserved block size;
- root-block cluster;
- normal free-block count;
- always-free reserve;
- roving allocation pointer;
- delete-directory pointer;
- disk size;
- extension-block pointer.

The disk name permits at most 31 characters plus terminator/storage.

The root validator checks media bounds, logical and reserved block sizes, root-cluster alignment, reserved-range ordering and free-count consistency.

## Reserved metadata area

PFS3 separates a reserved metadata region from ordinary filesystem data. The root's first/last reserved fields and reserved free counter govern this space.

The canonical maximum reserved-block geometry is bounded so corrupted fields cannot cause arithmetic or allocation outside media.

## Root cluster

The root block can occupy a cluster of reserved blocks. The canonical core limits root-cluster size and validates alignment against the reserved-block geometry.

## Extension record

When extension mode is present, an extension record begins with ID `0x4558` and contains:

- extension options;
- datestamp;
- format version;
- reserved-space roving pointer;
- roving bit;
- current anode sequence;
- delete-directory roving position;
- delete-directory size;
- configured filename size.

The effective filename size is validated against the supported range of 30 through 107 bytes.

## Anodes

Anodes describe file extents. The canonical anode record contains:

- cluster size;
- starting block number;
- next anode.

Anode chains therefore map a file to one or more contiguous physical runs. Every extent is checked against total media size before use.

## Anode blocks

Anode blocks are identified by `AB` and contain a header plus anode records. The canonical view tracks:

- datestamp;
- sequence;
- node count.

Record count is derived from the actual block size rather than trusted blindly.

## Directory blocks

Directory blocks are identified by `DB`. Their header view includes:

- datestamp;
- directory anode;
- parent anode.

Variable-length directory entries follow.

## Directory entries

The fixed directory-entry prefix is 20 bytes with the name beginning at byte 18. The decoder validates:

- record size;
- even alignment;
- available bytes;
- name length;
- comment bounds;
- optional extra-field words;
- unknown extra-field masks.

The canonical view exposes anode, low file-size bits, creation timestamp, extra flags and extra-word count.

## Extra fields and large-file support

Up to 11 extra-field words are recognised by the canonical format layer. Format mode bits determine whether extended size and other optional metadata are meaningful.

Large-file mode and filename-size extension must be validated together with the extension record rather than guessed from directory contents.

## Delete directory

The format can reserve delete-directory structures for undelete/recycle behaviour. Canonical limits include up to 32 delete-directory blocks with 31 entries per block for the represented layout.

## Allocation policy

The root tracks both normal free blocks and an always-free reserve. Allocation must not consume the reserve needed for metadata progress.

A roving pointer allows the allocator to avoid restarting every search at block zero. Extended-roving mode adds more detailed state in the extension record.

## Filesystem Support source design

Current state:

```text
core/
  pfs3_core.c/.h         root, extension, directory and anode format engine

linux/
  README.md              Linux adapter not yet implemented

windows/
  README.md              Windows adapter not yet implemented
```

The future Linux and Windows drivers must use the same canonical anode, directory, root and extension rules.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
