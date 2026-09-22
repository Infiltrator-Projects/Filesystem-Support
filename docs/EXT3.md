# EXT3 Native Driver

## Purpose

The EXT3 implementation is a standalone Linux VFS filesystem driver owned by Filesystem Support.

Its deployment target is exactly one loadable kernel module:

```text
ext3.ko
```

EXT3 is not implemented by registering EXT4 under another name. Its semantic starting point is the last standalone Linux EXT3 driver, Linux v4.2, because Linux removed the separate EXT3 driver after that release.

The historical JBD journal engine and metadata cache are embedded into `ext3.ko`. No `jbd.ko` or `mbcache.ko` is produced.

## Source layout

The active implementation lives at:

```text
native/filesystems/ext3/kernel/
```

The shaped tree contains 18 files:

```text
Makefile
ext3.h
journal.h

balloc.c
dir.c
file.c
ialloc.c
inode.c
namei.c
resize.c
super.c
xattr.c

jbd_checkpoint.c
jbd_commit.c
jbd_journal.c
jbd_recovery.c
jbd_revoke.c
jbd_transaction.c
```

This is deliberately larger than EXT2 because journaling is a real EXT3 subsystem and its transaction, commit, checkpoint, recovery and revoke engines are substantial enough to remain separate.

## Filesystem identity

EXT3 registers only:

```text
ext3
```

A valid EXT3 mount requires a journal or an explicit request to create one. The driver does not register EXT2 or EXT4 aliases.

Unsupported EXT4-only incompatibility features must fail closed rather than silently being accepted as EXT3.

## Preserved EXT3 feature set

The driver deliberately retains the complete feature set of the last standalone EXT3 implementation, including:

- internal journals;
- external journal devices;
- journal recovery after an unclean shutdown;
- `data=journal`;
- `data=ordered`;
- `data=writeback`;
- configurable commit behaviour and barriers supported by the EXT3/JBD implementation;
- indexed directories and directory hashing;
- file-type directory entries;
- sparse superblocks;
- large files;
- meta block groups where supported by EXT3;
- online resize support;
- reserved resize inode handling;
- user extended attributes;
- trusted extended attributes;
- security-label extended attributes;
- POSIX ACLs;
- quotas and journalled quota handling when the target kernel supplies quota support;
- orphan tracking and replay-safe truncate semantics;
- normal EXT3 inode, block, directory, namespace, symlink and file operations.

The EXT3-specific xattr, ACL and security paths are built into the module rather than delegated to external helper modules.

## Source responsibilities

### `super.c`

Owns EXT3 registration, mount and remount handling, superblock validation, journal discovery/loading/creation, recovery-state management, freeze/unfreeze, sync, quota integration, module lifecycle and the small EXT3-to-JBD adapter.

### `journal.h`

Owns the private JBD types and interfaces used inside `ext3.ko`. It is private implementation material, not a module ABI.

### `jbd_*.c`

These files implement the embedded EXT3 journal engine:

- transaction lifecycle;
- journal commit;
- checkpointing;
- recovery/replay;
- revoke handling;
- journal core and cache lifetime.

Their symbols are not exported as a separate kernel service and they do not contain their own module entry/exit points.

### `balloc.c`

Owns block allocation, block-group accounting, reservation windows and bitmap helpers.

### `ialloc.c`

Owns inode allocation, inode-group selection and inode accounting.

### `dir.c`

Owns directory parsing, iteration, indexed-directory hashing and directory-record operations.

### `namei.c`

Owns VFS namespace mutation such as create, link, unlink, rename, mkdir, rmdir and mknod.

### `inode.c`

Owns inode mapping, truncate, orphan-sensitive inode updates, address-space operations and symlink inode operations.

### `file.c`

Owns regular-file operations, fsync and EXT3 file ioctls.

### `resize.c`

Owns EXT3 online resize and resize-inode operations.

### `xattr.c`

Owns EXT3 extended attributes, POSIX ACL storage, security labels and the private metadata-block cache used for xattr block sharing.

### `ext3.h`

Owns the shared EXT3 on-disk and in-memory declarations plus the small private declarations formerly split across ACL, xattr and namei headers.

## One-module rule

The module build is intentionally:

```text
all EXT3 implementation code
        +
embedded JBD
        +
embedded xattr metadata cache
        ->
ext3.ko
```

There is one `module_init()` and one `module_exit()` in the complete tree.

## Why Linux v4.2 is used as the EXT3 semantic source

Linux v4.2 is the last release containing a standalone `fs/ext3` driver. Later kernels route EXT3 through EXT4.

That later arrangement is not the architecture of this project. Filesystem Support wants independent EXT2, EXT3 and EXT4 implementations, so importing the final standalone EXT3 implementation is a cleaner starting point than carving EXT4-specific mechanisms out of a modern EXT4 driver.

The code will continue to be modernised as our own implementation, but EXT3 semantics must remain EXT3 semantics.

## Import and shaping

The import workflow fetches:

```text
Linux v4.2 fs/ext3
Linux v4.2 fs/jbd
Linux v4.2 mbcache
```

Then `tools/shape-ext3.sh`:

1. embeds JBD and mbcache into EXT3;
2. removes their separate module/export surfaces;
3. removes JBD tracing that is not filesystem functionality;
4. merges tiny organisational source files into their owning subsystem;
5. keeps every substantive EXT3 feature path;
6. produces the fixed 18-file tree;
7. verifies a single module lifecycle;
8. verifies all three EXT3 data-journaling modes remain;
9. verifies EXT3 journal feature handling remains;
10. rejects accidental EXT2/EXT4 registration or EXT4 implementation code.

## Development rule

EXT3 must evolve independently.

Common code may eventually be extracted only after EXT2, EXT3 and EXT4 have independently proven the same mechanism and sharing does not create another required helper `.ko`.

Correct filesystem semantics take priority over matching upstream Linux file organisation.
