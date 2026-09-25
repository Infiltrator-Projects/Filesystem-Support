# exFAT Design and Ownership

## Classification

exFAT is a conventional local removable-media filesystem. Filesystem Support
must implement its format semantics once and share them across host adapters.

## Current state

exFAT is currently **reference/import state**. The pinned upstream Linux
implementation is preserved under `reference/linux/` with original licensing,
copyright and filenames. It is not project-authored Filesystem Support code.

## Target layout

```text
native/filesystems/exfat/
  DESIGN.md
  core/                 canonical exFAT format/filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows IFS/WDK adapter
  userspace/            optional userspace adapter over the same core
  reference/linux/      pinned upstream Linux evidence
```

The separate `exfat-fuse` catalogue identity is a provider choice, not a
second exFAT implementation.

## Canonical ownership

The core owns exFAT-defined boot-region/geometry validation, FAT and allocation
bitmap semantics, cluster chains, directory-entry sets, stream/name entries,
upcase-table/name comparison, checksums, timestamps/attributes, file mapping,
allocation and exact mutation/recovery rules.

Linux VFS objects, NLS APIs, page cache and module lifetime stay in the Linux
adapter. Windows IFS objects and platform security/attribute presentation stay
in the Windows adapter.

## Promotion sequence

Begin with bounded format validation and read-only directory/file access,
qualified against independently produced exFAT media and malformed fixtures.
Then implement allocation/mutation with explicit transaction/recovery and
destructive testing. All adapters consume the same core.

## Failure policy

Invalid boot checksums, impossible geometry, contradictory FAT/bitmap state,
bad directory-entry sets, invalid cluster chains and unsupported features fail
closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/exfat` into
`reference/linux/`. Reference refreshes must never overwrite project-owned
implementation source.
