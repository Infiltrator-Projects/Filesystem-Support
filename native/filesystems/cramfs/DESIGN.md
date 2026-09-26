# CramFS Design and Ownership

## Classification

CramFS is a compressed read-only local/image filesystem. Its permanent
Filesystem Support shape is one canonical filesystem engine with thin host
adapters.

## Current state

CramFS is currently **reference/import state**. The files under
`reference/linux/` are the pinned upstream Linux implementation and retain
their original licences, provenance and filenames. They are not project code.

## Current directory contract

While CramFS remains in reference/import state, the filesystem root contains
only `DESIGN.md` and `reference/`, with the upstream implementation preserved
under `reference/linux/`. The future `core/`, `linux/` and `windows/`
directories appear only with independently authored code; `kernel/` and
`userspace/` are not valid substitutes.

CI enforces this boundary and keeps the imported reference source out of the
production CMake graph.

## Target layout

```text
native/filesystems/cramfs/
  DESIGN.md
  core/                 canonical CramFS format/read semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future core owns superblock validation, inode/directory interpretation,
compressed-block offset tables, decompression contract, name/path semantics and
all format/range corruption checks.

Host page-cache, VFS inode/file objects, block-device plumbing and module
lifetime stay in adapters.

## Promotion rule

Begin with host-neutral read-only parsing and independently generated images,
then add malformed-image coverage and adapter integration. CramFS is read-only
by design; a project implementation does not invent write semantics.

## Failure policy

Invalid offsets, impossible inode sizes/types, malformed names/directories,
decompression failures and unsupported format variants fail closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/cramfs` into
`reference/linux/`. Reference refreshes must never overwrite project-owned
source.
