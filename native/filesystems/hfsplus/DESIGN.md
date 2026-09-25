# HFS+ / HFSX Design and Ownership

## Classification

HFS+ (Mac OS Extended) and HFSX are conventional local block filesystems.
HFSX is the case-sensitive variant of the same filesystem family; it is not a
second independent filesystem implementation.

Filesystem Support therefore applies the canonical local-filesystem
architecture:

```text
                 hfsplus/core/
          canonical HFS+/HFSX semantics
               /                \
              /                  \
     hfsplus/linux/         hfsplus/windows/
       thin VFS                thin IFS
       adapter                 adapter
```

## Current implementation state

HFS+ is currently **reference/import state**.

There is no project-authored HFS+ canonical engine and no Filesystem Support
HFS+ native driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux HFS+ implementation copied by
the reference-refresh workflow.

Those files retain their upstream SPDX identifiers, copyrights, filenames and
implementation boundaries. Moving the reference source beneath
`reference/linux/` is provenance/layout hygiene only; it does not turn that
implementation into project-authored code.

The reference tree is not the permanent Filesystem Support source architecture.

## Current directory layout

```text
native/filesystems/hfsplus/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux HFS+ source, reference only
```

When an independent rewrite begins, project-authored source is added beside the
reference tree:

```text
native/filesystems/hfsplus/
  DESIGN.md
  core/                  canonical HFS+/HFSX implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/inspection adapter over the same core
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future canonical core owns HFS+-defined and HFSX-defined behaviour that is
independent of the host operating system, including:

- volume-header decoding, validation and feature/state policy;
- allocation bitmap and block-allocation semantics;
- catalog-file B-tree keys, records, traversal and mutation;
- extents-overflow B-tree and fork extent mapping;
- attribute-file B-tree and extended-attribute records;
- file and directory CNID/object identity rules;
- data and resource fork representation;
- Unicode name encoding, normalization/comparison and catalog key ordering;
- HFSX case-sensitive/case-folding policy as defined by the format;
- timestamps, Finder metadata and filesystem-defined flags;
- hard-link and symbolic-link representation;
- wrapper/embedded HFS volume handling where required for HFS+ discovery;
- journal state and recovery semantics only for variants explicitly supported;
- corruption, bounds and structural validation;
- filesystem-defined mutation and crash-consistency rules.

Linux VFS objects, dentries/inodes, page cache, buffer heads, kernel Unicode
helpers, credentials, mount/module lifetime and Linux error translation belong
in the Linux adapter.

Windows IRPs, VCB/FCB/CCB lifetime, Cache Manager integration, native security
presentation and NTSTATUS translation belong in the Windows adapter.

## HFS+ and HFSX rule

HFS+ and HFSX share one canonical implementation.

Case-sensitive HFSX behaviour and any HFSX-specific catalog-key rules must be
explicit negotiated filesystem state in the canonical core. They must not be
implemented as a second host-specific filesystem engine.

Classic HFS remains a separate filesystem under
`native/filesystems/hfs/`; shared ancestry is not permission to merge HFS and
HFS+ implementations.

## File-boundary rule

The upstream reference filenames such as `btree.c`, `catalog.c`,
`extents.c`, `inode.c`, `super.c`, `unicode.c` and `xattr.c` remain
unchanged under `reference/linux/` so their provenance is obvious.

Project-authored HFS+ files must be cut around Filesystem Support
responsibilities and portable invariants. Copying, renaming, merging,
recommenting or moving an upstream file is never evidence that its
implementation has been rewritten.

## Promotion sequence

HFS+ leaves reference/import state only through an explicit rewrite:

1. define the supported HFS+/HFSX media variants and feature/journal limits;
2. implement bounded host-neutral volume-header and fork/extents decoding;
3. implement catalog/extents/attribute B-tree traversal with independent
   fixtures;
4. implement Unicode/name comparison rules and HFSX policy in the canonical
   core;
5. qualify read-only traversal against independently produced HFS+/HFSX media
   and malformed/corrupt fixtures;
6. add a thin Linux adapter over the canonical core;
7. add mutation only after allocation, B-tree update, journal/recovery and
   crash-consistency contracts are explicitly defined and destructively
   qualified;
8. add a Windows adapter against the same canonical engine when ready;
9. remove inherited provenance only from implementation bodies that have
   actually been independently replaced.

## Safety and failure behaviour

Unknown or unsupported format features, invalid B-tree nodes, impossible
extent/fork geometry, allocation-bitmap contradictions, invalid CNIDs,
malformed Unicode/catalog keys, corrupt attribute records and unsafe journal
state must fail closed.

Write support must never be inferred merely because the reference Linux driver
contains write paths.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/hfsplus` into
`reference/linux/`.

Refreshing that reference evidence may update the upstream snapshot, but it must
never overwrite future project-authored `core/`, `linux/`, `windows/` or
`userspace/` source.

## Completion rule

A Filesystem Support HFS+ implementation is not claimed until a canonical core
and at least one adapter have independent implementation and qualification
evidence. The current reference tree is evidence only.
