# EROFS via FUSE Provider Ownership

## Classification

`erofsfuse` is an external userspace provider for the EROFS filesystem. It is
not a separate filesystem format.

The current catalogue delegates this path to Debian's `erofsfuse` package.

## One EROFS rule

Filesystem Support owns at most one EROFS implementation.

The canonical EROFS format, inode, directory, mapping, xattr and decompression
semantics belong in `native/filesystems/erofs/core/` when independently
implemented. A userspace/FUSE path is an adapter over that same engine, not a
second parser.

## Current state

There is no project-authored code in this directory. The former `.gitkeep`
has been removed.

## Intended shape

```text
native/filesystems/erofsfuse/
  DESIGN.md
```

This provider identity deliberately has no `core/` and no `kernel/`.

A future first-party EROFS userspace mount path belongs under the canonical
`erofs/userspace/` adapter and must consume the same EROFS core as native
platform adapters.

## Completion rule

The present entry is complete only as an external provider catalogue contract.
It must not be counted as an independent EROFS implementation.
