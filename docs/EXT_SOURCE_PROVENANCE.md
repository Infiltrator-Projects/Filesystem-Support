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

## Project-authored EXT2 units

The following active EXT2 units are maintained as project-authored source:

- `native/filesystems/ext2/core/ext2_core.c`
- `native/filesystems/ext2/core/ext2_core.h`
- `native/filesystems/ext2/core/ext2_engine.c`
- `native/filesystems/ext2/core/ext2_engine.h`
- `native/filesystems/ext2/kernel/canonical.c`
- `native/filesystems/ext2/kernel/file.c`
- `native/filesystems/ext2/kernel/Makefile`

The regular-file unit was replaced on 23 September 2026. Its implementation
uses the Linux VFS/IOMAP/DAX interfaces as platform APIs but does not retain the
previous implementation body or third-party author block.

## EXT2 migration units still to replace

These active kernel units remain migration work and must retain any existing
legal provenance until their implementation is genuinely replaced:

- `balloc.c`
- `dir.c`
- `ext2.h`
- `ialloc.c`
- `inode.c`
- `namei.c`
- `super.c`
- `xattr.c`

## EXT3 migration state

The active EXT3 kernel tree is still migration-era implementation and is not
yet classified as project-authored implementation. Rewrite it subsystem by
subsystem while preserving the one-`ext3.ko` architecture and EXT3-only
semantics.

## EXT4 migration state

The active EXT4 kernel tree is still migration-era implementation and is not
yet classified as project-authored implementation. Rewrite it subsystem by
subsystem while preserving the one-`ext4.ko` architecture, EXT4-only
registration and complete supported feature set.

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
