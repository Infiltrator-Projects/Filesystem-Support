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

## Project-authored EXT3 units

EXT3's active Linux tree is now cut by Filesystem Support responsibility rather
than the old EXT3/JBD translation-unit layout. The units that have crossed the
implementation-ownership boundary are:

- `native/filesystems/ext3/core/ext3_core.c`
- `native/filesystems/ext3/core/ext3_core.h`
- `native/filesystems/ext3/linux/core_bridge.c`
- `native/filesystems/ext3/linux/allocation.c`
- `native/filesystems/ext3/linux/directory_io.c`
- `native/filesystems/ext3/linux/file_io.c`
- `native/filesystems/ext3/linux/journal_durability.c`

`allocation.c` owns the project-authored block/inode allocation and online
growth adapter. `journal_durability.c` owns the project-authored checkpoint,
commit, recovery and revoke implementation. The recut does not itself change
provenance classification; only replaced implementation bodies are promoted.

## EXT3 migration state

The following recut EXT3 units still contain materially inherited implementation
and therefore remain outside the project-authored set:

- `native/filesystems/ext3/linux/inode_adapter.c`
- `native/filesystems/ext3/linux/namespace_mutation.c`
- `native/filesystems/ext3/linux/lifecycle.c`
- `native/filesystems/ext3/linux/extended_metadata.c`
- `native/filesystems/ext3/linux/journal_core.c`
- `native/filesystems/ext3/linux/journal_transactions.c`
- `native/filesystems/ext3/linux/linux_adapter.h`
- `native/filesystems/ext3/linux/journal_internal.h`

Historical attribution remains intact in those units until the implementation
body itself is replaced. EXT3 remains one `ext3.ko`; old filenames such as
`inode.c`, `namei.c`, `super.c`, `xattr.c`, `jbd_journal.c` and
`jbd_transaction.c` are no longer active source boundaries.

## Project-authored EXT4 units

EXT4 has also been recut around Filesystem Support responsibilities. The
project-authored active units are:

- `native/filesystems/ext4/core/ext4_core.c`
- `native/filesystems/ext4/core/ext4_core.h`
- `native/filesystems/ext4/linux/core_bridge.c`
- `native/filesystems/ext4/linux/storage_guard.c`
- `native/filesystems/ext4/linux/directory_io.c`
- `native/filesystems/ext4/linux/mapping_support.c`
- `native/filesystems/ext4/linux/volume_admin.c`
- `native/filesystems/ext4/linux/journal_durability.c`
- `native/filesystems/ext4/linux/security_support.c`
- `native/filesystems/ext4/linux/embedded_jbd2.h`
- `native/filesystems/ext4/linux/truncate.h`
- `native/filesystems/ext4/linux/fsmap.h`
- `native/filesystems/ext4/linux/fast_commit.h`
- `native/filesystems/ext4/linux/ext4_jbd2.h`
- `native/filesystems/ext4/linux/mballoc.h`
- `native/filesystems/ext4/linux/extents_status.h`
- `native/filesystems/ext4/linux/ext4_extents.h`
- `native/filesystems/ext4/linux/xattr.h`

These names describe project responsibilities rather than mirroring the Linux
EXT4 source tree. Merging or renaming is not used as evidence of authorship;
the provenance boundary continues to follow the implementation body.

## EXT4 migration state

The following responsibility-named EXT4 units remain migration implementation
until their bodies are independently replaced and qualified:

- `native/filesystems/ext4/linux/extent_tree.c`
- `native/filesystems/ext4/linux/extent_cache.c`
- `native/filesystems/ext4/linux/fast_commit_engine.c`
- `native/filesystems/ext4/linux/file_io.c`
- `native/filesystems/ext4/linux/inode_allocation.c`
- `native/filesystems/ext4/linux/inline_data.c`
- `native/filesystems/ext4/linux/inode_adapter.c`
- `native/filesystems/ext4/linux/control.c`
- `native/filesystems/ext4/linux/journal_core.c`
- `native/filesystems/ext4/linux/journal_transactions.c`
- `native/filesystems/ext4/linux/multiblock_allocation.c`
- `native/filesystems/ext4/linux/namespace_mutation.c`
- `native/filesystems/ext4/linux/orphan_recovery.c`
- `native/filesystems/ext4/linux/writeback_io.c`
- `native/filesystems/ext4/linux/online_resize.c`
- `native/filesystems/ext4/linux/lifecycle.c`
- `native/filesystems/ext4/linux/extended_metadata.c`
- `native/filesystems/ext4/linux/ext4.h`
- `native/filesystems/ext4/linux/include/linux/jbd2.h`
- `native/filesystems/ext4/linux/include/trace/events/jbd2.h`

The trace-event header remains Linux-only diagnostic migration source. Its
`include/trace/events/` path is retained because the Linux tracepoint
preprocessor expects that include shape; the path does not imply that its
implementation has crossed the project-authorship boundary.

EXT4 remains one `ext4.ko`. The permanent source layout is now ours even while
the implementation-replacement ledger remains deliberately conservative.

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
