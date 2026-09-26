# SCO BFS Design and Ownership

## Classification

BFS here means the SCO/UnixWare Boot File System. It is a conventional local
block filesystem, so its permanent Filesystem Support architecture is one
canonical filesystem engine with thin operating-system adapters.

## Current state

BFS is currently **reference/import state**. The Linux source is preserved
unchanged under `reference/linux/`; Filesystem Support does not claim that
implementation as project-authored code.

The upstream files retain their original names, licences and provenance.
`dir.c`, `file.c` and `inode.c` are reference translation units, not the
required permanent project file boundaries.

## Current directory contract

While BFS remains in reference/import state, the filesystem root contains only
`DESIGN.md` and `reference/`, with the copied Linux implementation below
`reference/linux/`. The future `core/`, `linux/` and `windows/` directories do
not exist until an independent rewrite actually begins; `kernel/` and
`userspace/` are not valid substitutes for that architecture.

CI enforces this present-state boundary and keeps the imported tree out of the
production CMake source graph.

## Target layout

```text
native/filesystems/bfs/
  DESIGN.md
  core/                 canonical BFS format and filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows IFS/WDK adapter when implemented
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future `core/` owns BFS-defined superblock/inode/directory layout,
allocation and block mapping, filename/directory rules, metadata validation,
free-space/accounting rules and any exact mutation/recovery semantics.

Linux VFS inode/file/dentry objects, buffer/page interfaces, mount lifecycle and
kernel error translation remain in `linux/`. Windows IFS objects and Cache
Manager/IRP integration remain in `windows/`.

## Promotion rule

BFS leaves reference/import state only after host-neutral format decoding,
independent fixtures, malformed-media tests and a real adapter are implemented.
Write support requires a separately reviewed durability/recovery contract.

Moving or renaming upstream source does not cross the authorship boundary.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/bfs` into
`reference/linux/`. It must never overwrite future project-owned source.
