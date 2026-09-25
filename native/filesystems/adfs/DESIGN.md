# ADFS Design and Ownership

## Classification

ADFS (Acorn Disc Filing System) is a conventional local block filesystem used
by RISC OS/Acorn systems. Filesystem Support therefore applies the normal
canonical-engine architecture:

```text
                  adfs/core/
          canonical ADFS semantics
               /             \
              /               \
       adfs/linux/          adfs/windows/
       Linux VFS             Windows IFS
       adapter               adapter
```

Filesystem rules belong in the canonical engine. Host VFS/IFS objects and
kernel lifecycle belong only in the host adapter.

## Filesystem semantics that belong in the canonical core

The current Linux reference confirms several filesystem-defined areas that must
not remain buried in a Linux adapter when ADFS is rewritten:

- disc-record validation and filesystem geometry;
- sector size and ID-length constraints;
- the allocation/free-space map and its zone/fragment representation;
- indirect disc addresses and object/block mapping;
- classic F and F+ directory formats and their record validation;
- object identity, parent identity, size and directory semantics;
- RISC OS load/execute addresses and 12-bit filetype representation;
- RISC OS owner/public/locked/directory/execute attribute semantics;
- filename limits and filename/filetype presentation rules;
- corruption detection for malformed maps, directory records and geometry;
- read/write ordering and recovery rules for any future mutation support.

These are ADFS semantics. They must be implemented once in `core/` and
consumed by every operating-system adapter.

## Adapter responsibilities

The Linux adapter may own Linux VFS registration, block-device and page/buffer
integration, inode/file/dentry objects, mount option translation, credentials
and Linux error/lifetime rules.

The Windows adapter may own IFS/WDK registration, IRPs, VCB/FCB/CCB lifetime,
Cache Manager integration, native security/attribute presentation and NTSTATUS
translation.

Neither adapter may independently reimplement the allocation map, directory
formats, filetype encoding or other ADFS-defined behaviour.

## Current implementation state

ADFS is currently **reference/import state**.

There is no project-authored canonical ADFS engine and no Filesystem Support
ADFS Linux or Windows adapter claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux ADFS implementation copied by
`.github/workflows/import-linux-filesystems.yml`.

The imported source currently demonstrates a Linux VFS implementation with F
and F+ directory handling, allocation-map traversal, inode/file operations and
optional write support. That capability is evidence about a mature external
implementation; it is not a claim that Filesystem Support has independently
implemented or qualified those behaviours.

Reference files retain their upstream filenames, SPDX identifiers, copyrights
and translation-unit layout. They are intentionally not renamed into
Filesystem Support responsibility names until their implementation bodies have
actually been independently replaced.

## Current directory layout

```text
native/filesystems/adfs/
  DESIGN.md
  reference/
    linux/               pinned Linux ADFS source, reference only
```

When the rewrite starts, project-authored code is added beside—not inside—the
reference tree:

```text
native/filesystems/adfs/
  DESIGN.md
  core/                  canonical ADFS implementation
  linux/                 thin native Linux adapter
  windows/               thin native Windows adapter when implemented
  reference/linux/       preserved upstream evidence
```

The reference tree is not linked by the product CMake build and is not an
Infiltrator ADFS module.

## Promotion rule

ADFS leaves reference/import state only after an explicit rewrite milestone:

1. document the supported ADFS format variants and geometry limits;
2. implement host-neutral disc-record, map, directory and object parsing;
3. qualify parsing against independent ADFS images and malformed fixtures;
4. add read-only mapping/file access through the canonical engine;
5. add a thin Linux adapter without copying filesystem semantics into it;
6. add write support only with explicit transaction/recovery and destructive
   qualification;
7. add a Windows adapter against the same canonical engine when ready;
8. remove inherited provenance only from implementation bodies that have
   actually been independently replaced.

Moving or renaming upstream Linux source is never evidence of authorship.

## Failure policy

Unknown or contradictory disc geometry, invalid map fragments, out-of-range
indirect addresses, malformed directory structures and unsupported variants
must fail closed. A future writer must not mutate media until the complete
affected structure is understood and independently validated.

## Reference provenance

The refresh workflow currently pins Linux v6.12.107 and copies `fs/adfs` to
`reference/linux/`. Refreshing that evidence may change the reference tree,
but it must never overwrite future project-authored `core/`, `linux/` or
`windows/` implementation.
