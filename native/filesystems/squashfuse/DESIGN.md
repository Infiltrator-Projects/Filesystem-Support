# SquashFUSE Provider Ownership

## Classification

SquashFUSE is a userspace/FUSE access path for the SquashFS filesystem format.
It is not a second SquashFS format or semantic implementation.

The Filesystem Support catalogue currently delegates this provider identity to
the external SquashFUSE package.

## Current implementation state

Filesystem Support contains no first-party SquashFUSE implementation. The
previous `.gitkeep` represented no source or ownership boundary and is removed
by this layout pass.

## Intended directory shape

```text
native/filesystems/squashfuse/
  DESIGN.md
```

There is deliberately no independent `core/` and no `kernel/` here.

If Filesystem Support later provides a userspace SquashFS mount, it belongs
under:

```text
native/filesystems/squashfs/userspace/
```

and must consume the same canonical SquashFS core as every other platform
adapter.

## Ownership rule

SquashFS format decoding, inode/directory semantics, fragments, xattrs,
compressed data framing and decompression-format policy belong exclusively to
the canonical `squashfs/core/`.

A userspace adapter may own FUSE request translation, mount lifecycle and
userspace cache policy, but it must not duplicate those filesystem semantics.

## Completion rule

This entry is complete only as an external provider catalogue contract. It is
not a project-authored SquashFS implementation.
