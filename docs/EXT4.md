# EXT4 Native Filesystem

## Purpose

Filesystem Support owns one EXT4 implementation.

EXT4 is a modern extent-capable, journalled block-group filesystem with 64-bit
geometry, checksummed metadata, advanced allocation, inline data, fast commit,
encryption/casefold integration and other EXT4-defined features.

It is not an EXT2/EXT3 compatibility registration layer.

Linux deployment produces exactly one independently deployable module:

```text
ext4.ko
```

The module uses Linux kernel JBD2 interfaces. Its former private journal
engine is absent; journal operations depend on kernel JBD2 support.

## Permanent responsibility layout

The active source tree is already recut around Filesystem Support
responsibilities:

```text
native/filesystems/ext4/
  DESIGN.md

  core/
    README.md
    ext4_core.c
    ext4_core.h

  linux/
    Makefile
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

    ext4.h
    ext4_extents.h
    ext4_jbd2.h
    extents_status.h
    fast_commit.h
    fsmap.h
    mballoc.h
    truncate.h
    xattr.h


  windows/
    README.md
```

The historical upstream boundaries such as `extents.c`,
`extents_status.c`, `mballoc.c`, `fast_commit.c`, `inline.c`,
`resize.c`, `mmp.c`, `orphan.c`, `fsmap.c`, `page-io.c`,
`readpage.c`, `crypto.c`, `verity.c` and the upstream JBD2 translation
units are not the permanent Filesystem Support file layout.

Those historical names may remain useful reference terminology, but they must
not be restored merely to resemble Linux source organization.

## Canonical core

`core/` is the canonical host-neutral EXT4 layer.

The project-authored canonical core owns format/feature rules that have already
crossed the implementation-ownership boundary, including:

- EXT4 feature negotiation and fail-closed compatibility policy;
- block, cluster and bigalloc geometry rules;
- EXT4 format constants and bounded decoding represented by the current core;
- host-neutral corruption/range rejection represented by the current core.

More filesystem semantics move into `core/` only when their implementation
body is independently rewritten and qualified. A source recut alone is not a
semantic extraction.

Linux VFS objects, Linux page-cache/bio/workqueue mechanics and Windows IFS
objects never belong in the canonical core.

## Linux responsibility files

The Linux directory is cut by project responsibility, not upstream
translation-unit history.

### Current source and provenance

The active Linux tree is listed in the layout above. The earlier per-file lists
in this document classified removed journal files and relied on migration
records that are no longer current. File names, comments and responsibility
recuts do not establish authorship of current implementation bodies. Consult
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md) for the current
limitations, and review the bodies and history before making an authorship
claim.

Journal operations call Linux kernel JBD2 interfaces. The private
`journal_core.c`, `journal_durability.c`, `journal_transactions.c`,
`embedded_jbd2.h` and private JBD2 include files are absent.

## Linux module boundary

The active Kbuild contract produces one module:

```text
obj-m += ext4.o

ext4-y := core_bridge.o storage_guard.o directory_io.o \
          extent_tree.o extent_cache.o fast_commit_engine.o file_io.o \
          inode_allocation.o inline_data.o inode_adapter.o control.o \
          mapping_support.o \
          multiblock_allocation.o namespace_mutation.o orphan_recovery.o \
          writeback_io.o online_resize.o lifecycle.o volume_admin.o \
          extended_metadata.o security_support.o
```

The result is exactly one Filesystem Support `ext4.ko`.

Changing source-file boundaries must not create a mandatory helper module or
collapse EXT4 into EXT2/EXT3.

## Windows boundary

`windows/` is reserved for the EXT4 IFS/WDK adapter.

No project-owned Windows EXT4 filesystem driver is claimed yet. When
implemented, it must consume the same canonical EXT4 core and must not become a
second EXT4 implementation.

Reusable Windows IFS infrastructure belongs in
`native/platform/windows/` only when it is genuinely filesystem-neutral.

## Filesystem identity

EXT4 registers only EXT4.

Because EXT2, EXT3 and EXT4 share ancestry, some valid feature combinations can
be difficult to distinguish solely by a synthetic label. The implementation
must therefore rely on the actual format/feature contract rather than inventing
a fake discriminator.

Filesystem Support still maintains independent deployments:

```text
native/filesystems/ext2/ -> ext2.ko
native/filesystems/ext3/ -> ext3.ko
native/filesystems/ext4/ -> ext4.ko
```

## Major EXT4 format responsibilities

The canonical filesystem model must ultimately own the host-neutral semantics
for, as supported:

- superblock and 64-bit feature/geometry state;
- block groups, flex groups and bigalloc clusters;
- traditional block mapping and extent trees;
- block/inode/multiblock allocation semantics;
- directory records and HTree indexing;
- xattrs, EA inodes and metadata checksums;
- inline data;
- JBD2 journal format and recovery invariants;
- fast-commit format and replay rules;
- MMP;
- quotas/project IDs;
- encryption/casefold format state;
- fs-verity metadata;
- orphan tracking/orphan files;
- online resize;
- corruption/range validation.

Linux integration with fscrypt, verity, quotas, page cache, bios, workqueues and
VFS objects remains adapter work.

## JBD2

EXT4 journal operations currently use Linux kernel JBD2 interfaces. The old
private journal implementation is no longer part of `ext4.ko`. The journal's
on-disk structures are big-endian; EXT filesystem metadata is little-endian.
Recovery and durability behaviour must be verified against the current code.

## Fast commit

FAST_COMMIT is not a substitute for the full JBD2 transaction engine.

The fast-commit log represents a bounded set of metadata deltas and must fall
back to a normal full transaction whenever an operation cannot be represented
safely. Replay must reconstruct idempotent resulting state and must not depend
on undurable data.

## Checksums and fail-closed validation

EXT4 metadata checksum coverage is structure-specific; there is no single
generic "checksum this block" rule.

Depending on enabled features, validation includes checksums for structures such
as:

- superblock;
- group descriptors;
- block/cluster and inode bitmaps;
- inodes;
- extent blocks;
- directory leaves and HTree nodes;
- xattr blocks;
- MMP state.

Unknown incompatibility features fail mounting. Unknown read-only-compatible
features prohibit writable mounting.

Contradictory geometry, invalid tree depth/order, out-of-range extents,
impossible allocation state, malformed journal state and unsupported format
combinations fail closed.

## Rewrite and provenance rule

EXT4 production source must not be refreshed from or mechanically reshaped from
Linux.

External implementations may be studied for behaviour, compatibility and test
evidence. A claim that a unit is project-authored requires examination of its current
implementation body and history; the previous file-by-file classification is
stale.

Renaming, merging, recommenting or moving inherited implementation does not
change provenance.

The authoritative classification is
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md).

## Qualification

EXT4 is not complete because `ext4.ko` compiles.

Qualification must cover the supported combinations of:

- independently manufactured media;
- normal read/write workloads;
- extent and indirect mapping;
- allocation and exhaustion;
- metadata checksum verification;
- journal and fast-commit recovery;
- directory/HTree mutation;
- xattr/ACL/security metadata;
- inline data;
- orphan handling;
- resize;
- mount/unmount and unclean recovery;
- malformed-media rejection;
- independent checker/implementation verification.

Feature recognition is not the same thing as feature qualification.

The authoritative architecture and source-ownership contracts are
[`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md),
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md), and
[`../native/filesystems/ext4/DESIGN.md`](../native/filesystems/ext4/DESIGN.md).
