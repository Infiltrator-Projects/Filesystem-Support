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

EXT3 has crossed the ownership boundary for the following active units:

- `native/filesystems/ext3/core/ext3_core.c`
- `native/filesystems/ext3/core/ext3_core.h`
- `native/filesystems/ext3/linux/canonical.c`
- `native/filesystems/ext3/linux/balloc.c`
- `native/filesystems/ext3/linux/dir.c`
- `native/filesystems/ext3/linux/file.c`
- `native/filesystems/ext3/linux/ialloc.c`
- `native/filesystems/ext3/linux/resize.c`
- `native/filesystems/ext3/linux/jbd_checkpoint.c`
- `native/filesystems/ext3/linux/jbd_commit.c`
- `native/filesystems/ext3/linux/jbd_recovery.c`
- `native/filesystems/ext3/linux/jbd_revoke.c`

The checkpoint engine was replaced again in September 2026 around the project's
own transaction-ring, batching, I/O-retirement and journal-tail invariants.  Its
ownership classification is based on the replacement implementation, not on
comment removal or symbol renaming.

For the other units above, the forensic migration review found that their
current implementations had already diverged substantially from the historical
Linux EXT3/JBD bodies and no longer retained third-party author blocks.  They
are therefore recorded here instead of being incorrectly described as
migration-era source.

## EXT3 migration state

The following active EXT3 Linux units still retain material inherited
implementation and remain explicitly outside the project-authored set:

- `native/filesystems/ext3/linux/inode.c`
- `native/filesystems/ext3/linux/namei.c`
- `native/filesystems/ext3/linux/super.c`
- `native/filesystems/ext3/linux/xattr.c`
- `native/filesystems/ext3/linux/jbd_journal.c`
- `native/filesystems/ext3/linux/jbd_transaction.c`

Their historical attribution must remain intact until each implementation body
is actually replaced.  CI intentionally treats removal of that provenance
before replacement as a failure.

EXT3 remains a single `ext3.ko`.  The rewrite continues subsystem by subsystem,
moving format semantics into the canonical core and keeping only Linux VFS,
block-device and kernel-lifetime policy in the Linux adapter.

## Project-authored EXT4 units

EXT4 has already crossed the ownership boundary for its canonical core and for
a substantial set of Linux-facing feature units.  CI treats the following as
project-authored and rejects reintroduction of the historical EXT/Linux author
blocks:

- `native/filesystems/ext4/core/ext4_core.c`
- `native/filesystems/ext4/core/ext4_core.h`
- `native/filesystems/ext4/linux/canonical.c`
- `native/filesystems/ext4/linux/embedded_jbd2.h`
- `native/filesystems/ext4/linux/truncate.h`
- `native/filesystems/ext4/linux/fsmap.h`
- `native/filesystems/ext4/linux/fast_commit.h`
- `native/filesystems/ext4/linux/crypto.c`
- `native/filesystems/ext4/linux/verity.c`
- `native/filesystems/ext4/linux/block_validity.c`
- `native/filesystems/ext4/linux/balloc.c`
- `native/filesystems/ext4/linux/dir.c`
- `native/filesystems/ext4/linux/mmp.c`
- `native/filesystems/ext4/linux/ext4_jbd2.c`
- `native/filesystems/ext4/linux/readpage.c`
- `native/filesystems/ext4/linux/fsmap.c`
- `native/filesystems/ext4/linux/migrate.c`
- `native/filesystems/ext4/linux/move_extent.c`
- `native/filesystems/ext4/linux/sysfs.c`
- `native/filesystems/ext4/linux/indirect.c`
- `native/filesystems/ext4/linux/jbd2_checkpoint.c`
- `native/filesystems/ext4/linux/jbd2_commit.c`
- `native/filesystems/ext4/linux/jbd2_recovery.c`
- `native/filesystems/ext4/linux/jbd2_revoke.c`
- `native/filesystems/ext4/linux/ext4_jbd2.h`
- `native/filesystems/ext4/linux/mballoc.h`
- `native/filesystems/ext4/linux/extents_status.h`
- `native/filesystems/ext4/linux/ext4_extents.h`
- `native/filesystems/ext4/linux/xattr.h`

This list records the implementation ownership already enforced by the build
workflow.  The previous ledger incorrectly described all Linux EXT4 source
outside the canonical core as migration-era code.

## EXT4 migration state

The remaining large EXT4 implementation bodies are still being replaced
feature by feature.  In particular the inode, namespace, mount, allocation,
page-I/O, xattr and embedded-JBD2 transaction/journal bodies are not promoted
merely because adjacent headers or helper units have been rewritten.

Historical attribution remains required wherever inherited implementation is
still materially present.  EXT4 continues to build as one `ext4.ko`, and the
rewrite must preserve EXT4-only registration plus the complete feature set
advertised by the module.

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
