# EXT3 Filesystem Design

## Purpose

EXT3 extends the EXT2 block-group format with ordered, crash-recoverable metadata transactions through JBD. It remains a distinct filesystem implementation: the journal is part of EXT3 correctness, not an optional compatibility wrapper.

## Base filesystem layout

EXT3 retains the EXT2-style superblock, block groups, group descriptors, allocation bitmaps, inode tables, direct/indirect file mapping and variable-length directory records.

The filesystem-level layout is therefore:

```text
EXT3 volume
├── superblock + feature state
├── group descriptor metadata
├── block/inode bitmaps
├── inode tables
├── file/directory data
└── journal
    ├── journal superblock
    ├── descriptor records
    ├── logged data/metadata blocks
    ├── revoke records
    └── commit records
```

## Geometry and feature negotiation

The canonical core validates:

- block size and inode size;
- blocks/inodes per group;
- first data block;
- group count and group bounds;
- sparse-super placement;
- META_BG descriptor placement;
- supported incompat and read-only-compatible feature masks.

Important recognised EXT3 feature bits include FILETYPE, RECOVER and META_BG, plus SPARSE_SUPER, LARGE_FILE and BTREE_DIR.

## Inodes and file mapping

EXT3 retains the classic 15-pointer inode mapping:

- 12 direct block pointers;
- one single-indirect pointer;
- one double-indirect pointer;
- one triple-indirect pointer.

The canonical core computes indirect paths independently of Linux buffer-head mechanics.

## Directories

Directories use EXT-style variable records and may use HTree indexing. Filesystem Support contains host-neutral validation for record geometry and the historical EXT hash families:

- legacy;
- half-MD4;
- TEA;
- signed/unsigned variants.

Directory records must remain block-local, aligned and bounded by the filesystem inode count.

## JBD journal format

The EXT3 journal uses magic `0xC03B3998` and typed records. Important block types are:

- descriptor block;
- commit block;
- journal superblock v1/v2;
- revoke block.

A descriptor identifies blocks participating in a transaction. Logged block images follow the descriptor. A commit record makes the transaction durable. Revoke records suppress replay of blocks that were subsequently freed or superseded.

The journal is a circular log. Sequence numbers distinguish transactions across ring wraparound.

## Recovery model

Recovery is conceptually three phases:

1. scan the journal to identify transaction boundaries and the valid replay range;
2. build revoke state;
3. replay committed, non-revoked block images in transaction order.

Recovery must reject malformed tags, impossible block numbers, bad journal headers and unsupported journal features before they can cause writes outside valid filesystem storage.

## Data modes

EXT3 supports the traditional Linux data modes:

- `data=journal`: file data is journalled with metadata;
- `data=ordered`: metadata is journalled and newly written file data is forced to disk before the metadata commit that exposes it;
- `data=writeback`: metadata is journalled without the ordered-data guarantee.

The transaction layer must preserve the semantics of the selected mode across writeback, truncate, rename, unlink and fsync.

## Checkpointing and revoke

Committed metadata eventually leaves the journal and is checkpointed to its home location. Revoke handling prevents stale logged blocks from resurrecting freed/reused metadata during recovery.

The implementation must advance journal head/tail state only after the corresponding durability conditions have been satisfied.

## Resize and allocation

EXT3 supports online growth semantics through group addition/extension. Allocation remains block-group based, with inode and block bitmaps, local free counters and global accounting.

Resize calculations use checked arithmetic and validate that newly published group metadata is addressable before updating the superblock.

## Filesystem Support source design

```text
core/
  ext3_core.c/.h          host-neutral EXT3/JBD format rules

linux/
  core_bridge.c           canonical-core build bridge
  allocation.c            block/inode allocation + online growth
  directory_io.c          Linux directory I/O
  file_io.c               Linux file I/O
  inode_adapter.c         Linux inode/page-cache integration
  namespace_mutation.c    create/link/unlink/rename operations
  extended_metadata.c     xattrs/ACL/security metadata
  lifecycle.c             mount/super/module lifecycle
  journal_durability.c    checkpoint/commit/recovery/revoke
  journal_core.c          journal object/ring lifecycle
  journal_transactions.c  handle and transaction state machine
  linux_adapter.h         EXT3 Linux-private model
  journal_internal.h      embedded journal contract
```

The journal remains embedded in `ext3.ko`; there is no separately deployed JBD helper module.

## Current implementation boundary

Some EXT3 Linux units still contain inherited implementation while they are being replaced. Their provenance remains explicit. The permanent file boundaries above are nevertheless Filesystem Support responsibility boundaries rather than the historical Linux EXT3/JBD source layout.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
