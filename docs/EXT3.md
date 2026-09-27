# EXT3 Native Filesystem

## Purpose

Filesystem Support owns one EXT3 implementation.

EXT3 extends the EXT2 block-group family with JBD transactional durability and
recovery. The journal is part of EXT3 correctness; it is not an optional wrapper
around EXT2 and EXT3 is not implemented by registering EXT4 under another name.

Linux deployment produces exactly one independently deployable module:

```text
ext3.ko
```

The module uses Linux kernel JBD2 interfaces for journal operations. It does
not include its former private journal engine. Its journal runtime therefore
depends on kernel JBD2 support.

## Permanent responsibility layout

The active production tree is responsibility-based:

```text
native/filesystems/ext3/
  DESIGN.md

  core/
    README.md
    ext3_core.c
    ext3_core.h

  linux/
    Makefile
    core_bridge.c
    allocation.c
    directory_io.c
    file_io.c
    inode_adapter.c
    namespace_mutation.c
    extended_metadata.c
    lifecycle.c
    journal_compat.c
    linux_adapter.h
    journal_internal.h

  windows/
    README.md
```

The historical Linux migration boundaries such as `balloc.c`, `ialloc.c`,
`dir.c`, `file.c`, `inode.c`, `namei.c`, `resize.c`, `super.c`,
`xattr.c`, `jbd_checkpoint.c`, `jbd_commit.c`, `jbd_journal.c`,
`jbd_recovery.c`, `jbd_revoke.c` and `jbd_transaction.c` are retired as
active file boundaries.

Those names may remain useful when studying historical implementations, but
they are not the Filesystem Support architecture and must not be restored merely
to resemble Linux source organization.

## Canonical core

`core/` is the canonical EXT3 filesystem layer.

The current project-authored core owns host-neutral rules including:

- EXT3 superblock feature policy and compatibility validation;
- filesystem geometry and block-group constraints;
- EXT-family directory-record validation;
- indirect block-path calculations;
- EXT3/JBD on-disk format constants and checked decoding that have already been
  promoted from migration code;
- corruption/range rejection for the semantics represented in the core.

Additional host-neutral filesystem and journal semantics move into `core/`
only when their implementation bodies are independently replaced and qualified.

The canonical core must not depend on Linux VFS objects, Windows IFS objects or
GUI/package-manager code.

## Linux adapter responsibilities

The Linux directory is cut by project responsibility rather than by the
historical EXT3/JBD translation-unit layout.

### `core_bridge.c`

Binds the canonical EXT3 core into `ext3.ko`. It must not contain a parallel
set of format rules.

### `allocation.c`

Owns the project-authored Linux allocation and online-growth adapter: block and
inode allocation integration, group-accounting interaction and Linux-specific
allocation policy. Host-neutral geometry remains canonical-core territory.

### `directory_io.c`

Owns the project-authored Linux directory-I/O adapter and the VFS mechanics that
expose EXT3 directory behaviour.

### `file_io.c`

Owns the project-authored Linux regular-file adapter, including VFS file
operations and host I/O integration.

### `inode_adapter.c`

Owns Linux inode/page-cache integration. This unit remains migration
implementation until its body is independently replaced and promoted in the
provenance ledger.

### `namespace_mutation.c`

Owns Linux namespace mutation mechanics such as create/link/unlink/rename.
This unit remains migration implementation until independently replaced.

### `extended_metadata.c`

Owns Linux xattr/ACL/security metadata integration. This unit remains migration
implementation until independently replaced.

### `lifecycle.c`

Owns mount, superblock publication, module/VFS lifetime and Linux-only
filesystem lifecycle. This unit remains migration implementation until
independently replaced.

### `journal_compat.c` and `journal_internal.h`

These are the active EXT3 journal integration files. The module calls Linux
kernel JBD2 interfaces; the former `journal_durability.c`, `journal_core.c`
and `journal_transactions.c` are absent. Inspect current source and history
before asserting independent authorship of any active adapter body.

## Current provenance state

The migration classifications previously listed here named removed files and
cannot establish authorship of current implementation bodies. The current
source status and limits of that determination are recorded in
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md).

## Linux module boundary

The Linux build remains one module:

```text
obj-m += ext3.o

ext3-y := core_bridge.o allocation.o directory_io.o file_io.o inode_adapter.o \
          namespace_mutation.o lifecycle.o extended_metadata.o \
          journal_compat.o
```

There is one EXT3 filesystem module, `ext3.ko`.

Journal operations currently rely on Linux kernel JBD2 support.

## Windows boundary

`windows/` is reserved for the EXT3 IFS/WDK adapter.

No project-owned Windows EXT3 driver is claimed yet. When implemented, it must
consume the same canonical EXT3 core and must not become a second EXT3
filesystem implementation.

Reusable Windows IFS mechanics may live in
`native/platform/windows/` only when they are genuinely filesystem-neutral.

## Filesystem identity

EXT3 registers only EXT3.

A valid EXT3 mount requires the journal semantics expected by the supported
format/state. Unsupported EXT4 incompatibility features fail closed rather than
being silently accepted as EXT3.

EXT2, EXT3 and EXT4 remain independently deployable and independently owned:

```text
native/filesystems/ext2/ -> ext2.ko
native/filesystems/ext3/ -> ext3.ko
native/filesystems/ext4/ -> ext4.ko
```

Shared ancestry is not permission to collapse them into one driver.

## Journal model

EXT3's on-disk journal format uses magic `0xC03B3998` and big-endian fields.
The active Linux adapter delegates journal transactions and recovery to kernel
JBD2 interfaces. The former private journal engine described by older versions
of this document is no longer in this module. Behaviour and recovery claims
require tests against the current implementation.

## Data modes and ordering

The implementation preserves the traditional EXT3 data-mode contracts where
supported:

- `data=journal`;
- `data=ordered`;
- `data=writeback`.

Journal transaction, checkpoint and home-write ordering must preserve the
selected mode across writeback, truncate, rename, unlink and fsync.

## Orphan and resize semantics

EXT3 recovery also includes the filesystem orphan-chain mechanism used for
interrupted unlink/truncate handling. Orphan inode numbers and links must be
range checked and loops rejected.

Online growth and allocation remain block-group based. Geometry must be checked
before new group metadata is published.

## Rewrite rule

The active EXT3 implementation must not be regenerated from Linux or another
filesystem implementation.

External implementations may be studied as behavioural, interoperability and
test evidence. Production source crosses the project-authorship boundary only
when the implementation body is independently replaced for this repository.

The retired upstream-style filenames must not be recreated as a source-shaping
step.

## Qualification

EXT3 is not complete merely because `ext3.ko` builds.

Qualification must cover, where applicable:

- independently manufactured EXT3 media;
- supported journal states and replay;
- malformed filesystem and journal structures;
- block/inode allocation and exhaustion;
- directory and namespace mutation;
- file I/O and fsync ordering;
- xattrs, ACLs and security metadata;
- online growth;
- unclean shutdown/orphan/recovery behaviour;
- repeated mount/unmount;
- independent checker or implementation verification.

The authoritative architecture and provenance contracts are
[`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md),
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md), and
[`../native/filesystems/ext3/DESIGN.md`](../native/filesystems/ext3/DESIGN.md).
