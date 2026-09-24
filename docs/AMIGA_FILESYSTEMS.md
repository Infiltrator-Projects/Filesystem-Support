# Amiga filesystem architecture

Filesystem Support maintains five independent canonical filesystem
implementations:

- OFS
- FFS
- SFS
- SFS2
- PFS3

Historical relationships do not make their on-disk semantics interchangeable.

## Permanent layout

```text
native/filesystems/ofs/
  core/
  linux/
  windows/

native/filesystems/ffs/
  core/
  linux/
  windows/

native/filesystems/sfs/
  core/
  linux/
  windows/

native/filesystems/sfs2/
  core/
  linux/
  windows/

native/filesystems/pfs3/
  core/
  linux/
  windows/
```

Each `core/` owns that filesystem's format interpretation, allocation and
mapping, directory and metadata rules, validation, mutation and recovery
semantics. Linux and Windows are host adapters around that filesystem's own
core.

## Independence rule

OFS, FFS, SFS, SFS2 and PFS3 must remain implementation-independent.

Code that implements filesystem semantics is not shared between filesystem
directories, even when two formats currently use identical arithmetic or
layout rules. If OFS and FFS need the same checksum, bitmap or name operation,
each filesystem owns its own implementation in its own core. This prevents a
future change to one filesystem from silently changing another.

Only genuinely filesystem-neutral infrastructure may be shared through
`native/core/` or Infiltratr Common. Filesystem-specific parsing, allocation,
mapping, namespace, metadata, recovery and format helpers stay with the owning
filesystem.

## Current ownership state

OFS and FFS now own separate canonical cores and permanent responsibility-cut
Linux adapters. Each Linux tree is reduced to `core_bridge.c`, `storage.c`,
`namespace.c`, `lifecycle.c`, `linux_adapter.h` and `disk_layout.h`.
Neither depends on a shared Amiga filesystem layer and neither retains the
migration filenames `affs.h` or `amigaffs.h`.

SFS now uses the same project layout principle: `core_bridge.c`,
`allocation.c`, `mapping.c`, `io.c`, `namespace.c`, `lifecycle.c`,
`linux_adapter.h` and `disk_layout.h`. The old ASFS file boundaries and
historical Changes file are not active source.

SFS2 and PFS3 currently expose project-owned canonical format cores. Their host
adapters remain separate work and must bind directly to those cores rather than
introducing alternate implementations.

## Binary rule

The intended Linux result is one independently deployable module per
filesystem: `ofs.ko`, `ffs.ko`, `sfs.ko`, `sfs2.ko`, and `pfs3.ko`
as each implementation reaches qualification.
