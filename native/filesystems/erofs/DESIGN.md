# EROFS Design and Ownership

## Classification

EROFS is a compressed immutable/read-only filesystem used for image and
embedded workloads. It is one filesystem regardless of whether access is via a
kernel adapter or a FUSE/userspace provider.

## Current state

EROFS is currently **reference/import state**. The pinned Linux implementation
is preserved under `reference/linux/` with upstream licensing and provenance.
It is not project-authored Filesystem Support code.

## Current directory contract

While EROFS remains in reference/import state, the filesystem root contains only
`DESIGN.md` and `reference/`, with the pinned Linux implementation preserved
under `reference/linux/`. The future `core/`, `linux/`, `windows/` and
`userspace/` directories appear only with independently authored implementation;
`kernel/` is not a valid alternate source tree.

CI enforces this boundary and rejects accidental production linkage of the
imported reference source.

## Target layout

```text
native/filesystems/erofs/
  DESIGN.md
  core/                 canonical EROFS format/read/decompression semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows adapter when implemented
  userspace/            optional provider glue over the same core
  reference/linux/      pinned upstream Linux evidence
```

The separate catalogue identity `erofsfuse` is a provider choice, not another
EROFS semantic implementation.

## Canonical ownership

The core owns superblock/feature validation, inode/directory layout, compressed
and uncompressed data mapping, chunk/index interpretation, xattrs, supported
compression-format framing and corruption/range checks.

Host page cache, fscache, VFS objects, kernel decompressor APIs and mount/module
lifetime remain adapter mechanisms.

## Read-only rule

EROFS is read-only by design for mounted filesystem semantics. Filesystem
Support must not invent writable mutation behaviour. Image-construction tools
are a separate userspace creation concern and do not turn the mounted
filesystem into a writer.

## Promotion sequence

Implement bounded host-neutral format parsing and data mapping, qualify with
independent images and malformed fixtures, then attach native/userspace
adapters to that same core.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/erofs` to
`reference/linux/`. Reference updates must never overwrite future project
source.
