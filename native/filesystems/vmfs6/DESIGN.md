# VMware VMFS6 Design and Ownership

## Classification

VMFS6 is a persistent VMware shared-disk filesystem format and is maintained as
a separate filesystem from the VMFS3/VMFS5 entry. The current Debian path is
userspace and read-only through `vmfs6-tools`.

## Current implementation state

Filesystem Support contains no first-party VMFS6 implementation. The current
product path is the external read-only userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

```text
native/filesystems/vmfs6/
  DESIGN.md
  core/                 canonical VMFS6 format/filesystem semantics
  userspace/            image/device/FUSE access over the canonical core
  linux/                optional thin native Linux adapter if justified
  windows/              optional thin Windows adapter if justified
  reference/            optional evidence with explicit provenance
```

VMFS3/VMFS5 remain independently owned under `native/filesystems/vmfs/`.
Code may be shared only when it is genuinely format-neutral infrastructure.

## Canonical ownership

A future `core/` owns qualified VMFS6 volume/extent geometry, metadata
structures, file/inode mapping, directory rules, allocation/locking semantics,
journal/recovery state and corruption validation.

Platform block I/O, FUSE presentation and host object lifetime remain adapters.

## Safety and failure behaviour

VMFS6 is a shared-disk filesystem. Mutation must not be admitted until the
locking/ownership, transaction and recovery semantics are exact and
independently qualified. Unknown or contradictory metadata state fails closed.

## Completion rule

The current entry is complete only as an external read-only provider catalogue
contract. It is not a first-party VMFS6 implementation.
