# EXT Source Provenance and Rewrite Ledger

This document records the EXT2, EXT3 and EXT4 migration history. Its earlier
per-file authorship classifications became stale after the journal changes.
They must not be used to determine current authorship without inspecting the
current implementation bodies and their history. A filename, copyright line,
or old classification alone cannot establish whether its current body is copied.

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

EXT2 has completed its first permanent responsibility recut. The active Linux
adapter no longer uses the inherited translation-unit names.

The project-authored active units are:

- `native/filesystems/ext2/core/ext2_core.c`
- `native/filesystems/ext2/core/ext2_core.h`
- `native/filesystems/ext2/core/ext2_engine.c`
- `native/filesystems/ext2/core/ext2_engine.h`
- `native/filesystems/ext2/linux/core_bridge.c`
- `native/filesystems/ext2/linux/allocation.c`
- `native/filesystems/ext2/linux/namespace.c`
- `native/filesystems/ext2/linux/io.c`
- `native/filesystems/ext2/linux/metadata.c`
- `native/filesystems/ext2/linux/lifecycle.c`
- `native/filesystems/ext2/linux/linux_adapter.h`
- `native/filesystems/ext2/linux/Makefile`

The Linux source is now cut by Filesystem Support responsibility rather than by
the historical `balloc/ialloc/dir/namei/file/inode/xattr/super` translation
unit boundaries. Block and inode allocation are one allocation adapter;
directory enumeration and namespace mutation are one namespace adapter; file
and inode/page-cache integration are one I/O adapter; extended metadata is one
metadata adapter; and mount/module lifetime is one lifecycle adapter.

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

The live Windows adapter consumes the same canonical EXT2 engine. Its former
single `ext2_driver.c` implementation body is now cut into responsibility-owned
Windows fragments for object/I/O support, name/information translation, file
dispatch, directory dispatch, volume lifecycle and DriverEntry. The fragments
remain one WDK translation unit intentionally, so this structural recut changes
neither linkage nor filesystem semantics.

## Current EXT3 and EXT4 source state

The active EXT3 Linux tree contains `journal_compat.c` and
`journal_internal.h`; it no longer contains `journal_core.c`,
`journal_transactions.c`, or `journal_durability.c`. The active EXT4 Linux
tree likewise has no `journal_core.c`, `journal_transactions.c`,
`journal_durability.c`, `embedded_jbd2.h`, or private `include/linux/jbd2.h`
and `include/trace/events/jbd2.h` files. Prior lists naming those files as
active implementation were incorrect and have been removed.

Recent EXT3 and EXT4 changes use the Linux kernel JBD2 interfaces for journal
work. This is a runtime dependency, not evidence by itself that the source in
our modules was copied. Conversely, rewriting or restructuring files does not
by itself prove independent authorship. The current EXT3 and EXT4 implementation
bodies have **not** received a complete, file-by-file provenance determination
in this ledger. Do not report them as either wholly project-authored or wholly
inherited based on this document.

Historical attribution text still present in active source, including
`native/filesystems/ext4/linux/lifecycle.c`, requires review against the
implementation body and history. Do not delete attribution merely to make an
authorship claim or infer copied implementation solely from the comment.

The three filesystem modules remain separate: `ext2.ko`, `ext3.ko`, and
`ext4.ko`. The EXT2 assessment above describes the earlier responsibility
recut; it is not a legal determination about every line of current source.

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
