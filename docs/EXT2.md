# EXT2 Native Filesystem

## Purpose

Filesystem Support owns one EXT2 implementation.

EXT2 filesystem semantics are implemented once in the canonical `core/`. Linux
and Windows are operating-system adapters around that same implementation; they
must not become independent EXT2 filesystems.

EXT2 remains deliberately separate from EXT3 and EXT4. Linux deployment
produces exactly one loadable module:

```text
ext2.ko
```

There is no project-owned helper filesystem module and no compatibility path
that silently mounts journalled EXT3 media as EXT2.

## Permanent source layout

The current production layout is the permanent responsibility-based layout:

```text
native/filesystems/ext2/
  DESIGN.md

  core/
    ext2_core.c
    ext2_core.h
    ext2_engine.c
    ext2_engine.h

  linux/
    Makefile
    core_bridge.c
    allocation.c
    namespace.c
    io.c
    metadata.c
    lifecycle.c
    linux_adapter.h

  windows/
    Directory.Build.props
    ext2_driver.c
    ext2_driver.h
    adapter_support.inc
    name_translation.inc
    file_dispatch.inc
    directory_dispatch.inc
    volume_lifecycle.inc
    driver_entry.inc
    filesystem_support_ext2.inf
    filesystem_support_ext2.rc
    filesystem_support_ext2.sln
    filesystem_support_ext2.vcxproj
```

The retired Linux-style boundaries such as `balloc.c`, `ialloc.c`,
`dir.c`, `namei.c`, `file.c`, `inode.c`, `super.c` and `xattr.c`
are not the Filesystem Support architecture and must not be reintroduced merely
because upstream Linux uses or historically used similar translation units.

Likewise, the former `kernel/` staging directory and `linux/canonical.c`
migration boundary are retired.

## Canonical core responsibilities

`core/` is the filesystem, not a parser helper layer.

It owns host-neutral EXT2 behaviour including:

- superblock decoding and validation;
- feature negotiation and rejection policy;
- block-size, inode-size and block-group geometry;
- group-descriptor decoding and bounds validation;
- sparse-superblock placement rules;
- absolute-block to block-group/offset mapping;
- direct, single-, double- and triple-indirect logical block mapping;
- directory-record sizing, validation, insertion and deletion/coalescing rules;
- initial `.` and `..` directory-record layout;
- portable volume/inode/file engine behaviour;
- corruption and range rejection;
- any other EXT2 rule that Linux and Windows must interpret identically.

If a filesystem rule would otherwise need to be implemented in both host
adapters, it belongs in `core/`.

## Linux adapter responsibilities

The Linux adapter owns only Linux integration and Linux-specific policy.

### `core_bridge.c`

Compiles/binds the canonical EXT2 core into the Linux module. It must not grow a
second set of EXT2 format rules.

### `allocation.c`

Owns Linux-side block/inode allocation integration, reservation/locality policy
and VFS/kernel accounting mechanics. Host-neutral group geometry and mapping
rules stay in the canonical core.

### `namespace.c`

Owns Linux directory iteration and VFS namespace mutation: lookup/create/link,
unlink, mkdir/rmdir, mknod, rename and the Linux object/lifetime mechanics
needed to expose canonical EXT2 namespace rules.

### `io.c`

Owns Linux inode/file/page-cache/address-space integration, including regular
file I/O, inode lifecycle/mapping presentation, truncate, mmap/fsync and
host-specific file operations.

### `metadata.c`

Owns Linux xattr, ACL and security-label integration plus the Linux-side
metadata cache/lifetime mechanisms required to expose canonical EXT2 metadata
rules.

### `lifecycle.c`

Owns VFS filesystem registration, mount/remount, superblock publication,
freeze/sync/statfs, teardown, module lifetime and other Linux-only filesystem
lifecycle mechanics.

### `linux_adapter.h`

Owns the Linux-private in-memory model and contracts shared by the adapter
translation units. It must not become a second on-disk format specification.

## Linux module boundary

`native/filesystems/ext2/linux/Makefile` builds one composite module:

```text
obj-m += ext2.o
ext2-y := core_bridge.o allocation.o namespace.o io.o metadata.o lifecycle.o
```

The result is one independently deployable `ext2.ko`.

Changing source-file boundaries must never create helper modules or couple EXT2
to EXT3/EXT4.

## Windows adapter responsibilities

The Windows adapter consumes the same canonical core through
`../core/ext2_engine.h`.

The adapter is cut by responsibility but deliberately compiled as one WDK
translation unit at the current stage:

- `ext2_driver.c` — small translation-unit shell;
- `ext2_driver.h` — Windows-private VCB/FCB/CCB and dispatch contracts;
- `adapter_support.inc` — object lifetime, locking and canonical-core block-I/O bridge;
- `name_translation.inc` — Unicode/path and information translation plus IRP buffers;
- `file_dispatch.inc` — create/read/write/cleanup/close and file-information IRPs;
- `directory_dispatch.inc` — directory enumeration/control IRPs;
- `volume_lifecycle.inc` — mount/verify/lock/dismount and filesystem/device control;
- `driver_entry.inc` — DriverEntry and major-function registration.

These fragments are Windows integration. EXT2 on-disk semantics must remain in
the canonical core.

Reusable IFS infrastructure may move to `native/platform/windows/` only when
it is genuinely filesystem-neutral.

## Media and feature contract

EXT2 is a non-journalled block-group filesystem.

The primary superblock begins at byte offset 1024. The canonical core validates
the filesystem magic `0xEF53`, revision, block/inode geometry, block groups,
feature masks and all later addresses before they are trusted.

EXT2 block groups contain the block bitmap, inode bitmap, inode table and data
blocks. Group descriptors identify those structures and carry free
block/inode/directory counts.

Classic EXT2 inode block mapping uses 15 entries:

```text
0..11   direct
12      single indirect
13      double indirect
14      triple indirect
```

Directories contain variable-length records aligned within filesystem blocks.
When the FILETYPE incompat feature is active, the directory entry carries an
explicit file-type byte.

Extended attributes are stored in the filesystem-defined xattr block referenced
by the inode. Linux presentation of user/trusted/security/POSIX ACL namespaces
belongs to the Linux adapter; xattr block validity and format rules remain
filesystem semantics.

Fast symlinks may store their payload in the inode block-pointer area.

## Format versus implementation claims

A field or feature existing in the historical EXT-family format does not mean
Filesystem Support claims that behaviour.

The implementation explicitly rejects journalled media as EXT2. EXT3/EXT4
feature recognition used to reject incompatible media is not EXT3/EXT4
implementation.

Media outside the project's qualified compatibility range must not be advertised
as supported merely because the field widths can represent it.

## Validation and failure model

Validation is fail-closed.

At mount/open time the implementation must reject, as applicable:

- wrong magic or unsupported revision/features;
- journalled EXT3 media presented as EXT2;
- contradictory block/inode geometry;
- out-of-range group metadata;
- invalid direct/indirect mapping;
- malformed directory records;
- impossible inode references;
- corrupt xattr structures;
- allocation/accounting underflow or overflow.

EXT2 has no journal, so crash consistency depends on conservative metadata write
ordering and offline checking after unclean shutdown.

## Provenance and rewrite status

EXT2 has crossed the project-authorship boundary.

The authoritative provenance ledger is
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md). It records the active
canonical core and Linux responsibility units as project-authored implementation.

The Windows adapter consumes the same canonical EXT2 engine; its responsibility
fragments are intentionally kept together as one WDK translation unit while
their adapter-only ownership is made explicit.

The active EXT2 production tree must never be refreshed from or mechanically
reshaped from upstream Linux source. External implementations may be studied as
behavioural/interoperability evidence only.

Renaming, merging, recommenting or moving external code is not a rewrite.

## EXT2 versus EXT3 and EXT4

The three filesystems remain independently owned:

```text
native/filesystems/ext2/ -> ext2.ko
native/filesystems/ext3/ -> ext3.ko
native/filesystems/ext4/ -> ext4.ko
```

Shared ancestry of their formats is not permission to collapse their
implementations or modules.

Only genuinely filesystem-neutral infrastructure may move to shared Filesystem
Support platform code or Infiltratr Common, and only when that sharing does not
couple the three filesystems.

## Qualification

Compilation alone is not completion.

EXT2 qualification must cover, where applicable:

- independently manufactured media;
- read/write workloads;
- malformed-media rejection;
- mount/unmount cycles;
- allocation exhaustion;
- rename/link/unlink and directory mutation;
- xattr/ACL/security metadata;
- crash/unclean-state handling appropriate to non-journalled EXT2;
- Linux VFS behaviour;
- Windows IFS behaviour;
- independent verification by another implementation/checker.

The authoritative architectural rules remain
[`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md),
[`EXT_SOURCE_PROVENANCE.md`](EXT_SOURCE_PROVENANCE.md), and the per-filesystem
[`DESIGN.md`](../native/filesystems/ext2/DESIGN.md).
