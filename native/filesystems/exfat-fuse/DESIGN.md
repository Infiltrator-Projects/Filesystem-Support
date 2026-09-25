# exFAT via FUSE Provider Ownership

## Classification

`exfat-fuse` is an external userspace provider for the exFAT filesystem. It
is not a separate filesystem format.

## One exFAT rule

Filesystem Support may have only one exFAT implementation. Format parsing,
allocation bitmap/FAT handling, directory entries, name/upcase rules and
mutation/recovery semantics belong in `native/filesystems/exfat/core/`.

A userspace path is an adapter over that same core, not an independent parser.

## Current state

There is no project-authored code in this directory. The former `.gitkeep`
has been removed. The catalogue currently delegates to Debian's
`exfat-fuse` package.

## Intended shape

```text
native/filesystems/exfat-fuse/
  DESIGN.md
```

There is intentionally no `core/` or `kernel/` here.

A future project-owned userspace exFAT mount belongs under
`native/filesystems/exfat/userspace/` and consumes the canonical exFAT core.

## Completion rule

This entry is complete only as an external provider catalogue contract and is
never counted as an independent exFAT implementation.
