# ROMFS Design and Ownership

## Classification

ROMFS is a small read-only local block filesystem. Filesystem Support applies
the normal canonical-engine architecture:

```text
                   romfs/core/
            canonical ROMFS semantics
                 /             \
                /               \
        romfs/linux/         romfs/windows/
        thin VFS             thin host
        adapter              adapter
```

Its simplicity makes ROMFS useful for format and interoperability tests, but it
is no longer the project's first architectural proof. The repository-wide
native-filesystem plan now uses EXT2 as that proof.

## Current implementation state

ROMFS is currently **reference/import state**.

There is no project-authored canonical ROMFS engine and no Filesystem Support
ROMFS module claimed by this directory. The source under `reference/linux/`
is pinned upstream Linux ROMFS source retained as format and interoperability
evidence.

The imported files retain their original provenance, licences, filenames and
implementation boundaries. The former top-level `kernel/` directory was a
legacy staging path and did not make that source project-authored.

## Current directory layout

```text
native/filesystems/romfs/
  DESIGN.md
  README.md
  reference/
    linux/               pinned upstream Linux ROMFS source, reference only
```

When an independent implementation begins:

```text
native/filesystems/romfs/
  DESIGN.md
  core/                  canonical ROMFS format/read semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows adapter when implemented
  userspace/             optional image/inspection adapter over the same core
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns ROMFS-defined behaviour including:

- superblock/magic and image-size validation;
- 16-byte alignment and object-header decoding;
- pathname and directory traversal rules;
- regular-file, directory, symlink and special-object interpretation;
- hard-link target resolution;
- file payload bounds;
- ROMFS checksum validation where applicable;
- corruption and integer/range rejection.

Linux VFS objects, mmap/page-cache integration, block/device access and
mount/module lifecycle remain adapter concerns.

## File-boundary rule

The upstream `super.c`, `storage.c`, `mmap-nommu.c` and
`internal.h` remain unchanged under `reference/linux/`.

Project-authored source must be organised around host-neutral ROMFS format and
namespace responsibilities rather than by renaming the reference Linux units.

## Promotion sequence

1. document the supported ROMFS format and validation rules;
2. implement a bounded host-neutral parser and read-only traversal engine;
3. qualify it against independently generated ROMFS images and malformed
   fixtures;
4. expose the same core through userspace inspection;
5. add a thin Linux adapter and compare it side-by-side with an independent
   ROMFS implementation;
6. add other host adapters only against the same canonical core.

ROMFS is read-only by format intent for this project; a writable filesystem
implementation is not required merely to claim complete ROMFS reading.

## Failure policy

Invalid magic, impossible image size, unaligned/out-of-range object offsets,
unterminated names, link cycles and payload references outside the image must
fail closed.

## Reference provenance

The Linux-reference refresh copies `fs/romfs` into `reference/linux/`.
Refreshing that evidence may replace only the imported tree and must never
overwrite future project-owned `core/`, `linux/`, `windows/` or
`userspace/` source.

## Completion rule

The current tree is reference evidence only. ROMFS becomes a Filesystem Support
first-party implementation when its canonical read engine and applicable
adapter have independent implementation and qualification evidence.
