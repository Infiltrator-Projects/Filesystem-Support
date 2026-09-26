# UBIFS Design and Ownership

## Classification

UBIFS is a journalled flash filesystem designed to operate on UBI volumes. Its
filesystem semantics are distinct from the UBI flash-volume management layer.

Filesystem Support therefore treats UBIFS as one canonical filesystem engine
with an explicit storage boundary. The canonical core owns UBIFS format,
indexing, journalling and recovery semantics; platform/storage adapters provide
validated access to an appropriate UBI-equivalent volume.

## Current implementation state

UBIFS is currently **reference/import state**.

There is no project-authored canonical UBIFS engine and no Filesystem Support
UBIFS native module claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux UBIFS implementation retained
as format, UBI-integration and recovery evidence.

The imported files retain their upstream provenance, licences, filenames and
translation-unit boundaries. The former top-level `kernel/` staging path was
misleading because unchanged upstream Linux implementation looked like
project-owned native source.

## Current directory layout

```text
native/filesystems/ubifs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux UBIFS source, reference only
```

When an independent implementation begins:

```text
native/filesystems/ubifs/
  DESIGN.md
  core/                  canonical UBIFS filesystem semantics
  linux/                 thin Linux VFS + UBI-volume adapter
  windows/               host adapter only if an equivalent safe storage layer exists
  userspace/             optional image/inspection adapter over the same core
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns UBIFS-defined behaviour including:

- superblock, master node and format feature validation;
- tree-node-cache/key/index interpretation;
- inode, directory and xattr semantics;
- logical eraseblock accounting as exposed by the filesystem format;
- journal/log/bud state and commit publication;
- orphan handling and crash recovery/replay;
- free-space budgeting and filesystem-defined garbage-collection policy;
- compression and authentication metadata defined by the supported UBIFS format;
- corruption, sequence and range validation.

The UBI volume API, raw-flash wear management, device lifetime, VFS objects,
kernel workqueues and host cache mechanics are adapter/storage-layer concerns.

## File-boundary rule

Upstream units such as `tnc.c`, `journal.c`, `commit.c`,
`recovery.c`, `replay.c`, `lpt.c`, `gc.c`, `super.c` and
`ubifs.h` remain unchanged beneath `reference/linux/`.

Project-owned source must be cut around portable UBIFS responsibilities and an
explicit UBI-storage abstraction. Moving or renaming imported Linux files is not
evidence of an independent rewrite.

## Promotion sequence

1. define the supported UBIFS format/version and required UBI-volume contract;
2. implement bounded host-neutral node, key, superblock and master decoding;
3. implement read-only index/namespace/file traversal on deterministic fixtures;
4. model log/journal/commit/replay and orphan recovery exactly;
5. qualify corruption and interrupted-commit recovery independently;
6. add a thin Linux adapter over the same canonical core and UBI boundary;
7. admit mutation only after destructive flash/UBI qualification proves the
   complete durability and recovery contract.

## Failure policy

Malformed nodes, invalid CRC/authentication data, impossible sequence state,
out-of-range logical eraseblock references, contradictory master/log/index
state and unsupported features must fail closed. UBI/flash I/O failures must
not be translated into successful filesystem publication.

## Reference provenance

The Linux-reference refresh copies `fs/ubifs` into `reference/linux/`.
Refreshing that evidence may replace only the imported reference tree and must
never overwrite future project-owned `core/`, `linux/`, `windows/` or
`userspace/` source.

## Completion rule

The current tree is reference evidence only. UBIFS becomes a Filesystem Support
first-party implementation only when its canonical engine and applicable
storage/platform adapter have independent implementation and qualification
evidence.
