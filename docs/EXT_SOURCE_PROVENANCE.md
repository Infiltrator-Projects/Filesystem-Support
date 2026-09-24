# EXT Source Provenance and Rewrite Ledger

This document is the authoritative migration ledger for the active EXT2, EXT3
and EXT4 implementation source.

The project rule is simple: production implementation source must be written
for Filesystem Support. External implementations may be studied for observable
behaviour, media compatibility, edge cases and test vectors, but copied or
transformed implementation source is not an acceptable final state.

A file moves into the **project-authored** set only when its implementation has
actually been replaced. Renaming symbols, merging files, changing comments,
changing Kbuild layout or embedding support code into one `.ko` does not by
itself qualify as a rewrite.

## Directory-layout status

The former `kernel/` staging paths have been retired. Active Linux adapter and
migration source now lives under each filesystem's `linux/` directory.

A rewritten implementation may move from a migration path such as
`linux/inode.c` into `core/` and/or remain in `linux/` when its filesystem
semantics and operating-system glue are separated.  Provenance classification follows the
implementation, not the filename or directory.

The permanent layout is chosen by responsibility and cohesion.  Filesystem
semantics belong to the canonical core; Linux-specific interfaces belong to the
Linux adapter; Windows-specific interfaces belong to the Windows adapter.

## Project-authored EXT2 units

The following active EXT2 units are maintained as project-authored source:

- `native/filesystems/ext2/core/ext2_core.c`
- `native/filesystems/ext2/core/ext2_core.h`
- `native/filesystems/ext2/core/ext2_engine.c`
- `native/filesystems/ext2/core/ext2_engine.h`
- `native/filesystems/ext2/linux/canonical.c`
- `native/filesystems/ext2/linux/file.c`
- `native/filesystems/ext2/linux/namei.c`
- `native/filesystems/ext2/linux/ialloc.c`
- `native/filesystems/ext2/linux/inode.c`
- `native/filesystems/ext2/linux/xattr.c`
- `native/filesystems/ext2/linux/super.c`
- `native/filesystems/ext2/linux/Makefile`

The regular-file, namespace, inode-allocation, directory, block-allocation and
Linux private-model units have been replaced with project-owned implementations.
Their Linux code uses kernel interfaces as platform APIs but does not retain the
previous implementation bodies or third-party author blocks.

## EXT2 migration state

The active EXT2 Linux implementation has crossed the project-ownership boundary.
No active EXT2 Linux implementation unit remains classified as inherited
migration source.

Migration is tracked by implementation ownership rather than by file count.
Several format rules formerly embedded in the inherited Linux units are already
owned by the canonical core even though the containing Linux files have not yet
crossed the full provenance boundary.

Current EXT2 semantic extractions include:

- superblock decoding, feature policy and geometry validation;
- group-descriptor decoding, group bounds and descriptor validation;
- logical-file-block to direct/indirect path mapping;
- directory record length encoding/decoding and corruption validation;
- directory insertion sizing and split eligibility;
- initial `.` / `..` directory record layout;
- directory deletion/coalescing span validation; and
- absolute block to block-group/offset mapping used by allocator paths; and
- sparse-superblock group placement and feature policy.

The directory, allocator, inode lifecycle/mapping, mount/superblock, extended
metadata and Linux private-model replacements have now crossed the
implementation-ownership boundary.

## EXT3 migration state

EXT3 now has a project-authored canonical core for feature compatibility policy:

- `native/filesystems/ext3/core/ext3_core.c`
- `native/filesystems/ext3/core/ext3_core.h`

The Linux wrapper links that exact core through `linux/canonical.c`. The rest of
the active EXT3 `linux/` tree remains migration-era implementation and must be
replaced subsystem by subsystem while preserving the one-`ext3.ko` architecture
and EXT3-only semantics.

## EXT4 migration state

EXT4 now has a project-authored canonical core for feature compatibility and
bigalloc format invariants:

- `native/filesystems/ext4/core/ext4_core.c`
- `native/filesystems/ext4/core/ext4_core.h`

The Linux wrapper links that exact core through `linux/canonical.c`. The rest of
the active EXT4 `linux/` tree remains migration-era implementation and must be
replaced feature by feature while preserving the one-`ext4.ko` architecture,
EXT4-only registration and the complete supported feature set.

## Retired EXT mechanisms

The following EXT-specific mechanisms are deliberately removed and must not
return:

- any workflow step that imports or overwrites EXT2, EXT3 or EXT4 from upstream;
- EXT2/EXT3/EXT4 source-shaping scripts;
- the generic EXT imported-source hardening transformer;
- the automated inherited-source recommenting tool.

The repository may continue to import or refresh copied upstream source for
other filesystems that have not yet entered rewrite state. Those trees remain
reference/import implementations and retain their upstream provenance until
their own rewrite begins.

## Promotion rule

Before moving a migration unit into the project-authored set:

1. define the behaviour and invariants the replacement must provide;
2. write the implementation around the project architecture rather than by
   mechanically transforming an external source file;
3. compile it against the intended platform API;
4. exercise malformed-media and normal-media behaviour where applicable;
5. verify that required filesystem features have not regressed;
6. remove inherited legal provenance only after the inherited implementation
   is no longer present in that unit; and
7. add the rewritten unit to the CI source-boundary checks.

This ledger records engineering provenance. It is not a substitute for legal
advice about copyright, licensing or third-party claims.
