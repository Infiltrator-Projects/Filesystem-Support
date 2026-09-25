# Classic HFS Design and Ownership

## Classification

HFS is the classic Macintosh Hierarchical File System. It is a conventional
local block filesystem and therefore belongs in the canonical-engine
architecture.

## Current state

HFS is currently **reference/import state**. The pinned upstream Linux HFS
source under `reference/linux/` is preserved as compatibility evidence and is
not project-authored Filesystem Support code.

## Target layout

```text
native/filesystems/hfs/
  DESIGN.md
  core/                 canonical HFS format/filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows IFS/WDK adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future core owns HFS master-directory-block and volume geometry, allocation
bitmap, extents overflow and catalog B-tree semantics, file/resource forks,
directory/catalog records, classic Mac metadata/attributes, name translation
policy and exact mutation/recovery invariants.

Linux VFS objects, page/buffer APIs, mount/module lifetime and native permission
presentation remain adapter concerns.

## File-boundary rule

Upstream `btree.c`, `catalog.c`, `extent.c`, `inode.c`, `super.c` and
related files retain their original names only in `reference/linux/`.
Project-owned source is cut by portable HFS responsibility, not copied Linux
translation units.

## Promotion sequence

Start read-only with bounded MDB, allocation, catalog and extent parsing against
independently created HFS images and malformed fixtures. Add writes only after
catalog/extents/allocation publication and recovery/verification rules are
explicit and destructively qualified.

## Failure policy

Invalid allocation/catalog/extents B-tree nodes, contradictory volume
geometry, out-of-range extents, malformed names and unsupported HFS states fail
closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/hfs` into
`reference/linux/`. Reference refreshes must never overwrite future
project-owned source.
