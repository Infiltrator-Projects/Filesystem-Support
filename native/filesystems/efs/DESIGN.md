# SGI EFS Design and Ownership

## Classification

EFS is Silicon Graphics' legacy Extent File System, predating XFS. It is a
conventional local block filesystem and the present catalogue exposes it as
read-only.

## Current state

EFS is currently **reference/import state**. The pinned Linux implementation is
preserved under `reference/linux/` with its original licensing, provenance
and source boundaries. Filesystem Support does not claim it as project-authored
code.

## Target layout

```text
native/filesystems/efs/
  DESIGN.md
  core/                 canonical EFS format/read semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The core owns EFS-defined superblock/geometry validation, inode layout,
extent/block mapping, directory/name semantics, symlink interpretation and
corruption/range checks.

Linux VFS inode/file/dentry objects, buffer cache and mount/module lifecycle
belong only in the Linux adapter.

## Read-only rule

The project must not infer write semantics from the existence of inode/file
operations in another implementation. EFS remains read-only until exact
allocation/update/recovery semantics are independently documented and a
deliberate write milestone is admitted.

## Promotion sequence

Implement bounded host-neutral parsing first, qualify against independent EFS
images and malformed fixtures, then add a thin read-only host adapter.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/efs` into
`reference/linux/`. Reference updates must never overwrite project-owned
source.
