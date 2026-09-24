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

## Forensic completion notes against the PFS3 distribution's disk-structure guide

### Boot blocks and root position

The hard-disk format guide describes the first two logical blocks as AmigaDOS boot blocks. The first longword carries the PFS disk type and the remaining boot-block area is normally empty for the described hard-disk format.

The root block follows directly at logical block 2. It is cached as the central volume-control structure.

### Root block structure and reserved area

The root contains the fields already described plus direct index references in the remainder of the root block. In the classic 512-byte root layout, the important fixed offsets are:

```text
0x000  disk type
0x004  option/mode bits
0x008  update datestamp
0x00c  creation day/minute
0x010  creation tick + protection
0x014  volume name storage
0x034  last reserved block
0x038  first reserved block
0x03c  reserved blocks free
0x040  reserved-block size + root-block cluster size
0x044  ordinary blocks free
0x048  always-free reserve
0x04c  data-area roving pointer
0x050  delete-directory pointer (older layout)
0x054  disk size
0x058  root-block extension pointer
0x060  bitmap-index references begin
0x074  anode-index references in non-superindex layout
```

All filesystem metadata such as directories, allocation metadata and index/anode structures lives in the reserved area. Ordinary file data lives outside it.

The original guide specifies 1024-byte reserved blocks even when the device/logical block size is 512 bytes; the root-cluster field describes how many real blocks cover the root plus reserved bitmap.

### Update datestamps and atomic publication

The root datestamp is an update counter, not merely wall-clock time. Reserved metadata blocks carry related datestamps so repair tools can order versions of metadata.

PFS3's consistency design uses atomic metadata-state transitions. A complete Filesystem Support writer therefore needs to model metadata version/publication order, not only decode individual valid blocks.

### Block IDs

Reserved metadata block types include at least:

- `DB` — directory block;
- `AB` — anode block;
- `IB` — anode-index block;
- `BM` — bitmap block;
- `MI` — bitmap-index block;
- `DD` — delete-directory block;
- `EX` — root-block extension;
- `SB` — super-index block.

### Two allocation bitmaps

PFS3 maintains separate allocation coverage for:

1. the reserved metadata area;
2. the ordinary data area.

A data bitmap block contains a small header plus bitmap words; a set bit denotes an available block. Index blocks locate bitmap blocks for larger volumes.

The reserved bitmap begins immediately after the root block inside the root cluster and maps 1024-byte reserved blocks.

### Bitmap and anode index blocks

Index blocks contain an ID/header, datestamp, sequence number and an array of block references. The original 1 KiB reserved-block layout has 253 index entries after the 12-byte header.

Holes in an index are valid: a zero reference means that indexed block does not exist.

### Anode blocks and numbering

An anode block contains header state followed by fixed-size 12-byte anodes. In the described 1 KiB layout there are 84 anodes per block.

An anode is:

```text
cluster_size
first_block
next_anode
```

An unfragmented file can therefore use one anode; fragmented files chain anodes. A completely zero anode is free.

With split-anode mode, high and low halves of the anode number identify the anode-block sequence and slot. Without it, anodes are numbered linearly.

Special low anodes include the root-directory anode and a bad-block-list anode; ordinary file/directory anodes begin above the reserved range.

### Directory blocks

A directory is itself represented by an anode chain. Directory blocks are reserved metadata blocks, traditionally 1 KiB each, containing:

- block ID/header state and datestamp;
- directory head anode number;
- parent directory anode;
- variable directory entries.

Every block in one directory refers back to the same head directory anode.

### Directory entry layout

A directory entry is variable length and word aligned. Important fields include:

- record length;
- entry type;
- head anode number;
- file size;
- creation date/time;
- protection bits;
- name length/name;
- filenote length/filenote;
- optional directory-extension fields.

A zero-length/zero marker terminates the used entry area.

### Directory-extension fields

When directory-extension mode is active, packed optional 16-bit fields can represent:

- link information;
- uid;
- gid;
- high protection bits;
- rollover-file virtual size;
- rollover pointer;
- final presence-bit mask.

The presence-bit mask itself is mandatory when the mode is enabled.

### Hard links and soft links

Hard links use a link-list encoded through anode-shaped records that identify the object's directory, link directory and next link-list node. Directory entries for links refer through this link metadata to the original object.

Soft links are represented as file-like objects of soft-link type whose data contains the target path.

### Delete directory

PFS3 implements a hidden delete directory (`.deldir`) containing recently deleted/overwritten file references. The older form stores 31 rotating entries in one delete-directory block. Later super-delete-directory mode can use multiple blocks, with the extension record carrying its size and roving position.

Delete-directory entries can become invalid once their freed data blocks are reused; readers must validate before presenting an entry as recoverable.

### Root-block extension

The root extension stores format information that does not fit in the base root, including:

- extension options;
- datestamp;
- filesystem format/revision identifier;
- root and volume dates;
- postponed-operation state;
- reserved-area roving pointer;
- data roving bit;
- current anode sequence;
- delete-directory roving/size;
- configured filename size;
- super-index references;
- delete-directory ownership/protection/date;
- references to multiple delete-directory blocks.

A non-zero postponed-operation field on a quiescent/inhibited volume represents an incomplete operation and must be treated as recovery state.

### Super-index mode

SUPERINDEX exists to scale beyond the direct anode-index capacity of the root. The root extension points to super-index blocks, which in turn index anode-index blocks.

The root's ordinary index area is interpreted differently when SUPERINDEX is active; a parser must decide layout from the mode bit before treating the bytes as anode-index references.

### Rollover files

PFS3 supports rollover files with a fixed allocated capacity and a virtual live length/pointer that wraps through the allocated area. These semantics are encoded through directory-extension fields and are part of the format, not an ordinary sparse-file convention.

### PFS identifiers

The original hard-disk structure guide describes the root/boot identity `PFS\1`. Modern PFS3aio code also recognises a `PFS\2` on-disk identity for large-file/reserved-block extensions, while the Amiga FileSystem.resource can register handler/RDB aliases including `PFS\1`, `PDS\1`, `PFS\3` and `PDS\3`.

Those namespaces must not be conflated: the handler identifier used to select a filesystem implementation and the root block's on-disk identity are related but are not always the same value.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
