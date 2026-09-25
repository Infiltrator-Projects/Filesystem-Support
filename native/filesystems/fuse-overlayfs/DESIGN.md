# fuse-overlayfs Design and Ownership

## Classification

fuse-overlayfs is a userspace implementation of overlay/union filesystem
semantics, commonly used for rootless containers. It is not an on-disk
filesystem format.

The current catalogue delegates this capability to Debian's
`fuse-overlayfs` package.

## Current state

Filesystem Support contains no first-party fuse-overlayfs implementation. The
former `.gitkeep` represented no code and has been removed.

## Target architecture

If a first-party overlay provider is ever admitted, it belongs at the
userspace-service boundary:

```text
native/filesystems/fuse-overlayfs/
  DESIGN.md
  userspace/            overlay namespace/copy-up/whiteout policy, if admitted
```

There is deliberately no local-disk `core/` and no project kernel module in
this provider identity.

## Ownership

A future implementation may own overlay-specific behaviour such as lower/upper
layer lookup precedence, copy-up, whiteout/opaque directory semantics,
redirect/index/metacopy compatibility policy where explicitly supported, and
merged namespace presentation.

It must not duplicate the underlying filesystems' allocation, directory
storage, repair or durability engines.

## Safety

Layer roots and paths must remain confined to the configured trees. Copy-up,
rename and whiteout operations require explicit crash/failure behaviour and
must not silently expose lower-layer data after a failed mutation.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
It is not a project-authored local filesystem implementation.
