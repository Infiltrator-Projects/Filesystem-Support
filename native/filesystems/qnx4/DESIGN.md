# QNX4 Filesystem Design and Ownership

## Classification

QNX4 is a conventional local block filesystem used by QNX 4 systems.
Filesystem Support therefore applies the standard canonical-engine model:

```text
                    qnx4/core/
             canonical QNX4 semantics
                  /             \
                 /               \
          qnx4/linux/         qnx4/windows/
          thin VFS             thin IFS
          adapter              adapter
```

Filesystem-format rules belong in the host-neutral core. Linux and Windows
objects, cache APIs, mount lifecycle and error translation belong only in their
platform adapters.

## Current implementation state

QNX4 is currently **reference/import state**.

There is no project-authored canonical QNX4 engine and no Filesystem Support
QNX4 native driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux QNX4 implementation copied by
`.github/workflows/import-linux-filesystems.yml`.

Those files retain their upstream SPDX/licence/copyright notices, filenames and
translation-unit boundaries. They are reference evidence, not project-authored
production code.

The former top-level `kernel/` staging path was misleading because it made
unchanged upstream Linux source look like a Filesystem Support native module.
The imported blobs are therefore kept behind the explicit reference boundary.

## Current directory layout

```text
native/filesystems/qnx4/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux QNX4 source, reference only
```

When an independent implementation begins:

```text
native/filesystems/qnx4/
  DESIGN.md
  core/                  canonical QNX4 format/filesystem implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns QNX4-defined behaviour including:

- filesystem geometry and superblock validation;
- inode and directory record interpretation;
- allocation bitmap semantics and free-space validation;
- file block/extent mapping defined by the format;
- namespace and filename rules;
- filesystem-defined metadata and timestamps;
- corruption, bounds and consistency validation;
- exact mutation/recovery ordering if writable support is later admitted.

Linux VFS inode/file/dentry objects, page/buffer APIs, kernel credentials,
locking and module/mount lifetime are Linux-adapter concerns rather than QNX4
format semantics.

## File-boundary rule

Upstream files such as `bitmap.c`, `dir.c`, `inode.c`, `namei.c` and
`qnx4.h` remain unchanged below `reference/linux/`.

Project-authored code must be organised around Filesystem Support
responsibilities and portable filesystem invariants rather than by renaming
those Linux files. Moving or recommenting imported source is never evidence of
an independent rewrite.

## Promotion sequence

1. document the supported QNX4 format variants and geometry;
2. implement bounded host-neutral superblock/inode/directory/allocation parsing;
3. qualify read-only traversal with independently manufactured QNX4 images and
   malformed-media fixtures;
4. add a thin Linux adapter over the canonical engine;
5. add mutation only with explicit allocation/update/recovery rules and
   destructive qualification;
6. add a Windows adapter against the same canonical engine when appropriate;
7. remove inherited provenance only from implementation bodies that have
   actually been independently replaced.

## Failure policy

Invalid filesystem geometry, out-of-range inode or data references,
contradictory allocation state, malformed directory records and unsupported
format states must fail closed. Read-only compatibility of the current external
Linux provider must not be mistaken for validated first-party write support.

## Reference provenance

The Linux reference refresh currently pins Linux v6.12.107 and copies
`fs/qnx4` into `reference/linux/`. Refreshing that evidence may replace only
the imported reference tree and must never overwrite future project-owned
`core/`, `linux/` or `windows/` source.

## Completion rule

The current tree is reference evidence only. QNX4 becomes a Filesystem Support
first-party implementation only when its canonical engine and applicable
platform adapter have independent implementation and qualification evidence.
