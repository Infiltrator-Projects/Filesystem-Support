# EXT2 Native Driver

## Purpose

The EXT2 implementation is a deliberately self-contained Linux VFS module owned by Filesystem Support.

Its deployment target is exactly one loadable kernel module:

```text
ext2.ko
```

There are no separate EXT2 helper applications and there are no separate support modules such as `mbcache.ko`. Code required only by EXT2 is compiled into `ext2.ko`.

EXT2 is intentionally independent from EXT3 and EXT4. It must not register, mount or impersonate either filesystem.

## Source ownership and migration layout

The architectural rule is **one EXT2 filesystem implementation**. The
portable filesystem implementation belongs in `core/`; `linux/` and
`windows/` are OS wrappers around that same core, not separate EXT2
implementations. Most filesystem logic should therefore converge into
`core/`. Only host-specific VFS/KO or IFS/WDK integration remains in the
wrappers.

The current amount of code under `linux/` reflects migration history, not the
target split. Moving a file into `linux/` does not classify its filesystem
semantics as Linux-owned; those semantics must still be extracted or rewritten
into `core/` as the migration proceeds.


EXT2 is in active rewrite state.  The currently inherited Linux-style
translation-unit names under `native/filesystems/ext2/linux/` are migration
boundaries, not the final Filesystem Support architecture.

Files such as `file.c`, `inode.c`, `super.c`, `dir.c` and `namei.c`
remain useful temporary boundaries while each subsystem is replaced and
qualified.  Their names and locations do not constrain the final implementation.

The target source ownership is:

```text
native/filesystems/ext2/
  core/       canonical EXT2 format and filesystem semantics
  linux/      Linux VFS/module/block-device adapter
  windows/    Windows IFS/WDK adapter
```

The final directory need not contain one file corresponding to every Linux
source file.  Rewritten code is grouped by the responsibilities that make sense
for this project.  A migration unit may therefore be split when it contains
both portable filesystem semantics and Linux-specific VFS glue, or consolidated
when several tiny boundaries do not improve cohesion.

The required architectural result is one canonical EXT2 implementation consumed
by both operating-system adapters.  Linux still produces exactly one
`ext2.ko`; changing source-file boundaries does not imply additional kernel
modules.

The former `kernel/` staging directory has now been retired. The active Linux
adapter and remaining Linux-side migration units live under `linux/`. As each
mixed unit is rewritten, portable filesystem semantics move into `core/` while
Linux-only VFS/module code remains under `linux/`.

## Module boundary

`Makefile` produces one composite module:

```text
ext2.ko
```

The core objects are:

```text
balloc.o
dir.o
file.o
ialloc.o
inode.o
namei.o
super.o
```

When EXT2 extended attributes are enabled, `xattr.o` is added to the same `ext2.ko`.

No other `.ko` is produced by the EXT2 tree.

## File responsibilities

### `super.c`

Owns filesystem registration and module lifetime, mount and remount processing, superblock validation, feature compatibility checks, filesystem statistics, freeze/unfreeze behaviour, sync behaviour, inode-cache lifetime, quota hooks and teardown.

EXT2 must fail closed when the media advertises the EXT3 journal compatibility feature. A journalled filesystem is not accepted as EXT2.

The intended rule is:

```text
EXT2 media -> may mount as EXT2
journalled EXT3 media -> reject
EXT4 media/features -> reject when unsupported/incompatible
```

Compatibility fields that exist in the on-disk extended-filesystem superblock remain present where required to preserve structure offsets and to identify unsupported media. Recognising another format is not the same as implementing it.

### `inode.c`

Owns inode lifecycle, block mapping, truncation, read/write inode conversion, address-space operations, inode flags, file-operation selection and both normal and fast symlink inode operations.

The former tiny `symlink.c` is deliberately folded into this subsystem.

### `balloc.c`

Owns EXT2 block allocation and release, reservation windows, block-group accounting, sparse-superblock calculations and free-block accounting.

This remains separate because it is a large, coherent allocation subsystem.

### `ialloc.c`

Owns inode allocation and release, directory-placement policy, inode-group selection and free-inode/directory accounting.

### `dir.c`

Owns EXT2 directory-record parsing and validation, lookup support, directory iteration, insertion, deletion, empty-directory handling and directory file operations.

### `namei.c`

Owns namespace mutation and pathname-facing inode operations such as create, link, unlink, mkdir, rmdir, mknod and rename.

It remains separate from `dir.c`: directory-record manipulation and VFS namespace operations are related but distinct responsibilities.

### `file.c`

Owns regular-file operations, buffered and direct I/O, DAX file paths where enabled, mmap, fsync, open/release behaviour, file attributes and EXT2-specific ioctls.

The former `ioctl.c` is folded into this file because those operations belong to the regular-file interface and did not justify a separate translation unit.

Diagnostic direct-I/O tracepoints are intentionally not part of this implementation.

### `xattr.c`

Owns the complete optional EXT2 metadata subsystem:

- user extended attributes;
- trusted extended attributes;
- security-label extended attributes;
- POSIX ACL encoding, decoding and storage;
- EXT2 xattr block parsing, lookup, update and deduplication;
- the private metadata-block cache used by EXT2 xattrs.

The former `xattr_user.c`, `xattr_trusted.c`, `xattr_security.c`, `acl.c`, `acl.h`, `xattr.h`, `mbcache.c` and private `mbcache.h` are deliberately consolidated here and in `ext2.h`.

The metadata cache is implementation code, not a separately loadable service. When xattrs are disabled, the xattr translation unit and its cache code are omitted from `ext2.ko`.

### `ext2.h`

Owns the private EXT2 declarations and on-disk structures required across the implementation.

It also contains the private declarations formerly split across the xattr, ACL and metadata-cache headers.

Fields and constants that are part of the historical extended-filesystem on-disk structure remain when they are required for correct parsing, offsets or rejection of incompatible features. Dead EXT3-only policy constants are not retained.

## Mount and validation model

EXT2 uses the Linux VFS block-filesystem model.

At mount time the driver:

1. allocates its in-memory superblock state;
2. establishes the block size needed to read the on-disk superblock;
3. validates the EXT magic value;
4. validates revision and feature flags;
5. rejects unsupported incompatible features;
6. explicitly rejects journalled media;
7. validates block and inode geometry;
8. reads and validates group descriptors;
9. establishes allocation/accounting state;
10. creates the xattr cache only when xattrs are enabled;
11. reads and validates the root inode;
12. publishes the mounted filesystem to VFS.

Validation is fail-closed. Corrupt or contradictory geometry must not be normalised into a mountable filesystem.

## EXT2 versus EXT3 and EXT4

The three implementations are intentionally independent.

```text
native/filesystems/ext2/ -> ext2.ko
native/filesystems/ext3/ -> ext3.ko
native/filesystems/ext4/ -> ext4.ko
```

Shared ancestry of the formats is not permission to collapse them into one driver.

For the current development phase, duplicated source is acceptable. Later refactoring may extract genuinely common code only when doing so preserves independent module ownership and does not create a mandatory helper module.

## Optional features

Optional kernel configuration paths remain part of EXT2 where they are actual EXT2 capabilities, including:

- extended attributes;
- POSIX ACLs;
- security labels;
- quotas;
- DAX where the target kernel and block device support it;
- compatibility ioctls where required by the kernel configuration.

Optional functionality must not force unrelated code into the module. In particular, the private metadata cache is linked only when EXT2 xattrs are enabled.

## Deliberately excluded material

The EXT2 tree deliberately excludes:

- upstream Kconfig presentation text;
- direct-I/O tracepoint source and definitions;
- dead EXT3 journal-mode policy constants;
- a compatibility path that mounts journalled EXT3 media as EXT2;
- separate `mbcache.ko`;
- separate helper applications;
- EXT3 or EXT4 registration.

These exclusions remain part of the EXT2 rewrite contract; the EXT trees are not refreshed from upstream while rewrite work is active.


## Source-rewrite policy

The active EXT2 implementation must not be regenerated from Linux or any other
filesystem implementation.  External implementations may be studied as
behavioural evidence, but their source is not an input to the production tree.

The former Linux-source import and shaping workflow has been retired.  No
automation may fetch an upstream EXT2 tree and copy, transform, merge or
re-comment it into `native/filesystems/ext2/`.

Conversion is file-by-file and explicit.  A production file is considered
project-authored only after its implementation has been replaced by an
Infiltrator implementation designed for this repository's contracts.  Merely
changing comments, file boundaries, symbol names or Kbuild layout is not a
rewrite.

Current conversion state:

- `core/ext2_core.c`, `core/ext2_core.h`, `core/ext2_engine.c` and
  `core/ext2_engine.h` are the project-owned canonical core.
- `linux/canonical.c` is the Linux adapter for that canonical core.
- `linux/file.c` has been replaced with the Infiltrator regular-file/VFS
  implementation.
- Remaining Linux migration units are migration work until their implementation has
  been independently replaced and validated.

Legal/provenance notices are removed from a source file only when the inherited
implementation in that file has actually been replaced.  This rule prevents a
cosmetic provenance edit from being mistaken for a source rewrite.

## Current development rule

Do not optimise for similarity with upstream Linux directory layout.

Optimise for a correct, understandable and independently owned EXT2 implementation with one filesystem folder and one filesystem module.

Future changes should reduce duplication or complexity only when they make EXT2 clearer or safer without re-coupling it to EXT3, EXT4 or a shared support module.
