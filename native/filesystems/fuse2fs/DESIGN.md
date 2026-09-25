# fuse2fs Provider Ownership

## Classification

fuse2fs is an external userspace/FUSE provider that can mount EXT2, EXT3 and
EXT4 media. It is a delivery/access mechanism spanning three distinct
filesystems, not a fourth filesystem implementation.

The current catalogue delegates this capability to Debian's `fuse2fs`
package.

## EXT independence rule

Filesystem Support deliberately keeps EXT2, EXT3 and EXT4 as independent
canonical filesystem implementations. fuse2fs must not introduce a shared
alternative parser, allocator, directory engine or journal implementation that
bypasses those canonical cores.

If project-owned userspace mounting is later added, the correct destinations
are the owning filesystem trees, for example:

```text
native/filesystems/ext2/userspace/
native/filesystems/ext3/userspace/
native/filesystems/ext4/userspace/
```

Each userspace adapter consumes that filesystem's own `core/`.

## Current state

There is no project-authored source in this directory. The former `.gitkeep`
has been removed.

## Intended shape

```text
native/filesystems/fuse2fs/
  DESIGN.md
```

There is intentionally no `core/` and no `kernel/` here.

## Completion rule

This entry is complete only as an external provider catalogue contract. It is
never counted as an independent EXT-family implementation.
