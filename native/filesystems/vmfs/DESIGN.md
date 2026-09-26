# VMware VMFS3 / VMFS5 Design and Ownership

## Classification

This entry represents VMware VMFS3/VMFS5 filesystem formats. They are
persistent shared-disk filesystem formats, even though the current Debian
provider is userspace/read-only.

The current Filesystem Support catalogue delegates access to `vmfs-tools`.

## Current implementation state

Filesystem Support contains no first-party VMFS3/VMFS5 implementation. The
current product path is the external read-only userspace provider managed
through the catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If a first-party implementation is admitted:

```text
native/filesystems/vmfs/
  DESIGN.md
  core/                 canonical VMFS3/VMFS5 format/filesystem semantics
  userspace/            image/device/FUSE access over the canonical core
  linux/                optional thin native Linux adapter if justified
  windows/              optional thin Windows adapter if justified
  reference/            optional external evidence with explicit provenance
```

VMFS6 remains a separate filesystem entry and must not be silently folded into
this implementation.

## Canonical ownership

A future `core/` owns only documented/qualified VMFS3/VMFS5 semantics:
volume/extent geometry, metadata structures, inode/file mapping, directory and
namespace rules, allocation/locking metadata, journal/recovery state and
corruption validation.

Host block I/O, FUSE presentation and platform object lifetimes remain
adapters.

## Safety and failure behaviour

Shared-disk and clustered metadata must never be mutated without exact locking,
ownership and recovery semantics. Until those contracts are independently
implemented and qualified, first-party support should remain read-only and fail
closed on unknown metadata states.

## Completion rule

The current entry is complete only as an external read-only provider catalogue
contract. It is not a first-party VMFS implementation.
