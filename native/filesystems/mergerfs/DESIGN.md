# mergerfs Design and Ownership

## Classification

mergerfs is a userspace/FUSE pooling and union-namespace provider over existing
directory trees. It does not define an on-disk filesystem format and it does not
own the storage semantics of the filesystems backing its branches.

## Current implementation state

Filesystem Support contains no first-party mergerfs implementation.

The current product path is an external userspace provider managed through the
catalogue and action backend. The former `.gitkeep` represented no
implementation and is removed by this layout pass.

## Intended layout

```text
native/filesystems/mergerfs/
  DESIGN.md
  userspace/            pool/placement/namespace provider, if implemented
```

There is deliberately no disk-format `core/` and no project kernel module.

## Ownership

A future first-party implementation may own branch selection, path/namespace
merging, create/search policies, free-space/placement policy, rename/copy
behaviour across branches and explicit failure handling when a branch
disappears.

It must not duplicate the underlying filesystems' allocation, metadata,
durability, repair or recovery engines. Generic mount-service lifecycle,
credential handling and path validation should be shared where appropriate.

## Safety and failure behaviour

Branch roots and paths must remain confined to configured trees. Cross-branch
rename/copy behaviour, partial writes, ENOSPC handling and branch disappearance
must have explicit semantics rather than silently losing or duplicating data.

## Completion rule

The current entry is complete only as an external userspace pooling-provider
catalogue contract. It is not a project-authored block filesystem.
