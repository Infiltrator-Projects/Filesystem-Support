# TMFS Design and Ownership

## Classification

TMFS is a userspace, read-only reconstruction layer for Apple Time Machine
backups. It presents the backup namespace reconstructed from Time Machine
metadata and hard-linked HFS+ backup structures. It is not a standalone disk
filesystem format and must not duplicate HFS+ itself.

The Filesystem Support catalogue currently delegates this capability to the
external `tmfs` package.

## Current implementation state

Filesystem Support contains no first-party TMFS implementation. The current
product path is the external read-only userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If project-owned Time Machine reconstruction is admitted later, it belongs at
the userspace-service boundary:

```text
native/filesystems/tmfs/
  DESIGN.md
  userspace/            Time Machine namespace/reconstruction policy
```

There is deliberately no local-disk `core/` and no `kernel/` filesystem
module for this provider identity.

HFS+ on-disk parsing, allocation, B-tree and repair semantics remain owned by
the canonical HFS+ filesystem implementation. TMFS-specific code consumes a
filesystem view and owns only Time Machine backup reconstruction semantics.

## Ownership rules

A future implementation may own:

- Time Machine backup-set discovery and selection;
- reconstruction of hard-linked backup directory/file views;
- interpretation of Time Machine-specific metadata needed for that view;
- read-only namespace synthesis across backup snapshots;
- explicit handling of missing/inconsistent backup members.

It must not reimplement HFS+ catalog/extents/allocation semantics.

## Safety and failure behaviour

The provider is read-only. Malformed Time Machine metadata, broken hard-link
relationships, missing backup members and ambiguous reconstruction state must
remain explicit rather than silently manufacturing directory/file state.

## Completion rule

The current entry is complete only as an external read-only userspace-provider
catalogue contract. It is not a first-party filesystem implementation.
