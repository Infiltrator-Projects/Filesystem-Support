# EXT4 Filesystem Design

## Purpose

EXT4 is an extent-capable, journalled block-group filesystem descended from the EXT family but with substantially richer geometry, metadata integrity and scalability features. Filesystem Support treats EXT4 as its own canonical filesystem, not as an EXT2/EXT3 compatibility mode.

## High-level media layout

```text
EXT4 volume
├── superblock
├── group descriptor tables
├── flex/block groups
│   ├── block/cluster bitmaps
│   ├── inode bitmaps
│   ├── inode tables
│   └── data/metadata
├── journal (JBD2)
├── optional MMP block
├── optional quota/project metadata
└── optional encryption/verity metadata
```

## Superblock and feature model

EXT4 uses the EXT superblock family with additional 64-bit counts, checksum seeds, cluster geometry, flexible block-group data and feature flags.

Filesystem Support's canonical feature model recognises important incompat features including:

- FILETYPE;
- RECOVER;
- META_BG;
- EXTENTS;
- 64BIT;
- MMP;
- FLEX_BG;
- EA_INODE;
- CSUM_SEED;
- LARGEDIR;
- INLINE_DATA;
- ENCRYPT;
- CASEFOLD.

Recognised read-only-compatible features include:

- SPARSE_SUPER;
- LARGE_FILE;
- BTREE_DIR;
- HUGE_FILE;
- GDT_CSUM;
- DIR_NLINK;
- EXTRA_ISIZE;
- QUOTA;
- BIGALLOC;
- METADATA_CSUM;
- PROJECT;
- VERITY;
- ORPHAN_PRESENT.

Feature negotiation is fail-closed: unknown incompatible features prevent mounting, while unknown RO-compatible features prevent writable mounting.

## Block groups, flex groups and bigalloc

Traditional groups still contain allocation metadata and inode tables, but FLEX_BG allows metadata for several groups to be placed together. BIGALLOC changes allocation granularity from blocks to clusters.

The canonical core validates:

- first data block;
- block/cluster size relationship;
- blocks/clusters per group;
- inode size and first inode;
- descriptor geometry;
- 64-bit group ranges;
- sparse-super placement;
- bigalloc/extent compatibility.

## Inodes

EXT4 inodes extend the EXT inode model with larger inode sizes and extra inode fields. Depending on feature state they can carry:

- traditional indirect block maps;
- extent trees;
- extra timestamps and inode generation data;
- project ID;
- inline data;
- xattr/EA-inode references;
- encryption and verity state.

## Extent trees

With EXTENTS enabled, `i_block` begins with an extent header. The tree contains:

- extent header: magic, entries, maximum entries, depth, generation;
- internal extent indexes: logical key + child physical block;
- leaf extents: logical start, physical start and length.

Depth 0 means leaf extents are stored directly in the inode. Greater depth adds index levels. Every node must be bounded by the containing block/inode capacity, sorted by logical key, and free of overlapping/overflowing physical ranges.

## Allocation

EXT4 uses bitmap allocation plus the multiblock allocator. Delayed allocation and extent allocation allow the driver to choose larger contiguous runs once actual writeback size is known.

Per-group free-space accounting, buddy/multiblock state and global counters must remain transactionally consistent with journal updates.

## Directories

EXT4 directories retain EXT variable-length records and may use HTree indexing. The canonical engine validates record lengths, block boundaries, inode ranges and directory hashes.

Large-directory and directory-link-count features alter scalability limits. CASEFOLD changes name comparison semantics when enabled for an inode/filesystem.

## Extended attributes, ACLs and EA inodes

Xattrs can be stored:

- inside inode extra space;
- in external xattr blocks;
- in dedicated EA inodes where the feature permits.

Linux-facing namespaces include user, trusted, security and POSIX ACL data. The implementation must checksum/validate xattr structures where metadata checksums are active.

## Inline data

INLINE_DATA permits small file or directory data to live inside inode/xattr space rather than allocating external data blocks. Conversion from inline to block/extent storage must be journalled atomically.

## JBD2 journal

EXT4 uses JBD2. Its core transaction model is still descriptor/data/revoke/commit in a circular journal, with stronger feature evolution than EXT3 JBD.

The Filesystem Support journal responsibilities are split into:

- journal durability: EXT4 glue, checkpoint, commit, revoke and recovery;
- journal core: journal object/ring lifecycle;
- journal transactions: handles, credits, transaction state and buffer ownership.

## Fast commit

FAST_COMMIT logs a compact set of metadata changes for eligible operations so recovery can avoid replaying a complete full transaction. It must fall back to a normal full commit whenever an operation cannot be represented safely.

Fast-commit state must never make recovery dependent on data that was not durably written.

## Metadata checksums

METADATA_CSUM and related features protect selected metadata structures with checksums. The checksum seed may derive from the filesystem UUID or an explicit checksum seed feature.

Checksummed structures must be verified before use, and rewritten checksums must be recomputed after any field mutation.

## Multiple Mount Protection

MMP stores a periodically updated record used to detect simultaneous writers to the same filesystem. A writable mount must respect MMP state and fail closed when another active writer cannot be ruled out.

## Encryption and casefold

ENCRYPT integrates filename/content policy with the host fscrypt framework. CASEFOLD enables Unicode-aware case-insensitive directory semantics where configured. The filesystem format owns the feature state; the Linux adapter connects it to host crypto/name services.

## fs-verity

VERITY permits files to be protected by a Merkle-tree based authenticity structure. Filesystem Support's Linux adapter integrates EXT4 verity metadata with the kernel verity framework.

## Orphan handling

Unlinked-but-open or partially truncated inodes require crash-safe orphan tracking. ORPHAN_PRESENT and the orphan recovery engine ensure mount-time cleanup can finish operations interrupted by a crash.

## Online resize

EXT4 can grow through new groups and descriptor metadata. Checked arithmetic and group-geometry validation are mandatory before publishing new filesystem size or descriptor state.

## Filesystem Support source design

```text
core/
  ext4_core.c/.h

linux/
  core_bridge.c
  storage_guard.c
  directory_io.c
  extent_tree.c
  extent_cache.c
  fast_commit_engine.c
  file_io.c
  inode_allocation.c
  inline_data.c
  inode_adapter.c
  control.c
  mapping_support.c
  multiblock_allocation.c
  namespace_mutation.c
  orphan_recovery.c
  writeback_io.c
  online_resize.c
  lifecycle.c
  volume_admin.c
  extended_metadata.c
  security_support.c
  journal_durability.c
  journal_core.c
  journal_transactions.c
```

Headers in the Linux adapter define the EXT4/JBD2 private contracts, but the long-term design continues to move host-neutral semantics into `core/`.

## Current implementation boundary

The permanent translation-unit layout is project-owned. Some large EXT4 implementation bodies remain in migration/rewrite state and retain their historical provenance until independently replaced. Source recutting does not by itself change that classification.

## Forensic completion notes against the kernel EXT4 on-disk documentation

### Byte order

EXT4 filesystem structures are little-endian. JBD2 journal structures are big-endian. Checksums are computed over the byte representation defined by the format, not host-native structure values.

### Compatible feature classes omitted by a simple incompat list

A full feature audit must consider all three superblock feature words.

Important compatible features include historical/preallocation flags, HAS_JOURNAL, EXT_ATTR, RESIZE_INODE, DIR_INDEX, SPARSE_SUPER2, FAST_COMMIT and ORPHAN_FILE.

Important incompatible states also include JOURNAL_DEV and legacy/unsupported feature identifiers in addition to FILETYPE, RECOVER, META_BG, EXTENTS, 64BIT, MMP, FLEX_BG, EA_INODE, CSUM_SEED, LARGEDIR, INLINE_DATA, ENCRYPT and CASEFOLD.

Read-only-compatible state includes the fields already described plus flags such as filesystem-wide read-only images and historical snapshot/replica identifiers. Unknown incompatible features prohibit mounting; unknown read-only-compatible features prohibit a writable mount.

### Block-group descriptor contents

Depending on descriptor size and 64-bit mode, a group descriptor can contain:

- block/cluster bitmap address;
- inode bitmap address;
- inode table address;
- free block/cluster count;
- free inode count;
- used-directory count;
- descriptor flags;
- exclude-bitmap reference;
- block-bitmap checksum;
- inode-bitmap checksum;
- count of unused inode-table entries;
- group descriptor checksum;
- high halves of addresses/counts/checksums when 64BIT is active.

Lazy-initialisation state is carried by group flags such as block-bitmap uninitialised, inode-bitmap uninitialised and inode-table zeroed. Those flags alter whether bitmap/table contents are immediately authoritative and must be handled before trusting uninitialised metadata.

### Special inodes

The traditional reserved inode map includes:

- inode 1 — bad blocks;
- inode 2 — root;
- inode 3 — user quota;
- inode 4 — group quota;
- inode 5 — boot loader;
- inode 6 — undelete;
- inode 7 — resize/reserved group descriptors;
- inode 8 — journal;
- inode 11 — traditional first ordinary inode.

Modern superblock fields can additionally point to lost+found, project-quota and orphan-file inodes.

### Extended inode fields

When inode size exceeds the original 128-byte EXT inode, the extended area can include:

- extra inode-size field;
- high checksum bits;
- nanosecond/epoch-extension bits for ctime, mtime and atime;
- creation time and sub-second creation time;
- high version bits;
- project ID.

The inode checksum covers filesystem checksum identity/seed, inode number, generation and inode bytes according to the active checksum scheme.

### Directory block integrity

Classic directory entries use inode, record length, name length, file type and name. Encrypted+casefolded directories can append hash/minor-hash data to the entry.

With metadata checksums, a linear directory leaf ends with a synthetic directory-entry tail carrying the block checksum. HTree index blocks use their own checksum tail. Both checksum forms incorporate filesystem checksum identity plus directory inode/generation and the appropriate block contents.

### HTree structure

An indexed directory has a root index followed by index nodes and leaf directory blocks. Index entries map a name hash to a logical directory block. Hash ordering, entry count/limit and child block references are structural invariants; the tree is not a generic filesystem-block B-tree.

### Metadata checksum coverage

EXT4 checksums protect multiple metadata classes, including:

- superblock;
- group descriptors;
- block/cluster and inode bitmaps;
- inodes;
- extent blocks;
- directory leaves and HTree nodes;
- xattr blocks;
- MMP block.

The checksum ingredients differ per structure; a single generic "checksum this block" rule is insufficient.

### JBD2 details

The JBD2 journal common header is 12 bytes and uses magic `0xC03B3998`, block type and transaction sequence. The journal superblock records block size, journal length, first log block, sequence, log start, error state, feature words, UUID and checksum-related state.

JBD2 supports external journals. With an external journal the filesystem superblock identifies it by UUID/device rather than journal inode alone.

Descriptor tag layouts vary with 64BIT and checksum-v2/v3 features. Revoke block-number width likewise depends on 64BIT. Commit blocks can carry checksum type/value and commit time.

### Fast-commit record model

Fast-commit space is a TLV log. Important tag classes include:

- HEAD;
- ADD_RANGE;
- DEL_RANGE;
- CREAT;
- LINK;
- UNLINK;
- PAD;
- TAIL.

Replay is designed around idempotent resulting state rather than blindly repeating the original high-level operation. A traditional full JBD2 commit invalidates earlier fast commits.

### Orphan file

Traditional EXT orphan tracking uses a linked list. With ORPHAN_FILE, the superblock references a dedicated inode whose blocks contain arrays of orphan inode numbers followed by a tail magic and checksum. While valid entries may exist, ORPHAN_PRESENT is set so mount recovery knows it must process the orphan file.

### Atomic block writes

Modern Linux EXT4 also supports hardware-backed untorn direct-I/O writes when the device supplies atomic-write capability. Single-filesystem-block atomic writes require an appropriate filesystem block size; multi-block atomic writes use BIGALLOC cluster alignment. This is a host capability layered on EXT4 extent allocation, not a software copy-on-write transaction feature.

### Format versus current Filesystem Support implementation

This document now describes the format surface more broadly than the code currently implements. Feature recognition in `ext4_core.h` must therefore be audited separately from feature completeness. A bit being known to the format documentation must not cause the driver to advertise safe read-write support until the corresponding validation, mutation and recovery paths have been qualified.

## Design rules used by Filesystem Support

- The canonical `core/` is the filesystem. It owns format semantics, validation, allocation/mapping rules, namespace rules, recovery rules and corruption policy whenever those rules are host-neutral.
- `linux/` is a Linux VFS/block-device/module adapter, not a second filesystem implementation.
- `windows/` is a Windows IFS/WDK adapter, not a second filesystem implementation.
- A filesystem must fail closed on malformed media rather than silently accepting impossible geometry, out-of-range references or inconsistent metadata.
- On-disk compatibility is a format contract. Source-file layout is not; project source is cut by responsibility.
- Feature claims in this document distinguish the filesystem format from the current implementation state.

## Qualification expectations

A filesystem is not considered complete merely because it compiles. Qualification should include independently manufactured media, normal read/write workloads, malformed-media rejection, mount/unmount cycles, allocation exhaustion, rename/link/unlink cases, recovery where applicable, and independent verification by a separate implementation or checker where one exists.
