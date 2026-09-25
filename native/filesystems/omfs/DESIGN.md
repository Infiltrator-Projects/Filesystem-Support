# OMFS Design and Ownership

## Classification

OMFS is the Optimized MPEG Filesystem used by certain media-recording devices.
It is a conventional local on-disk block filesystem, so Filesystem Support
applies the normal canonical-engine architecture.

```text
                  omfs/core/
          canonical OMFS semantics
              /             \
             /               \
       omfs/linux/         omfs/windows/
       thin VFS             thin IFS
       adapter              adapter
```

## Current implementation state

OMFS is currently **reference/import state**.

The source below `reference/linux/` is the pinned upstream Linux OMFS driver.
It is retained as format, interoperability and failure-behaviour evidence only;
Filesystem Support does not claim it as project-authored implementation.

The former `kernel/` staging directory was misleading because it looked like
production native Filesystem Support code. The upstream blobs are therefore
preserved unchanged under the explicit reference/provenance boundary.

## Current directory layout

```text
native/filesystems/omfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux OMFS source, reference only
```

When an independent implementation begins, project-owned code is added beside
the reference tree:

```text
native/filesystems/omfs/
  DESIGN.md
  core/                  canonical OMFS format/filesystem implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future `core/` owns OMFS-defined behaviour that is independent of host
objects, including as supported:

- volume/superblock and geometry interpretation;
- inode/object record validation;
- allocation bitmap/free-space semantics;
- directory/name lookup and namespace rules;
- extent/file block mapping;
- checksum/integrity rules present in the format;
- mutation ordering and recovery invariants;
- malformed-media and range rejection.

Linux inode/file/dentry objects, buffer/page-cache interfaces, block-device
plumbing, module registration and Linux error/lifetime rules belong in
`linux/`.

Windows IRPs, VCB/FCB/CCB lifetime, Cache Manager integration and NTSTATUS
translation belong in `windows/`.

## File-boundary rule

Upstream files such as `bitmap.c`, `dir.c`, `file.c`, `inode.c`,
`omfs.h` and `omfs_fs.h` retain their original names, licences and
provenance under `reference/linux/`.

Project-authored source must be cut around canonical filesystem responsibilities
and thin host adapters. Moving, renaming or recommenting copied Linux source is
not an independent rewrite.

## Promotion sequence

1. document the supported OMFS variants, geometry and format limits;
2. implement host-neutral volume, inode/object and allocation decoding;
3. qualify read-only parsing against independently produced media/images and
   malformed fixtures;
4. implement directory and file mapping through the canonical engine;
5. add a thin Linux adapter over that core;
6. add mutation only with explicit allocation/update/recovery qualification;
7. add a Windows adapter against the same canonical implementation when useful.

## Failure policy

Invalid volume geometry, impossible inode/object references, corrupt allocation
state, malformed directory records, out-of-range extents and unsupported
variants must fail closed.

A future writer must not modify media until all affected structures and the
operation's recovery boundary are independently understood and verified.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/omfs` into
`reference/linux/`. Refreshing that evidence may replace only the copied
reference tree and must never overwrite future project-owned `core/`,
`linux/` or `windows/` source.
