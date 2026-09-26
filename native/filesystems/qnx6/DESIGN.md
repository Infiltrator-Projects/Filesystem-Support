# QNX6 Filesystem Design and Ownership

## Classification

QNX6 Power-Safe is a conventional local block filesystem. Filesystem Support
therefore applies the standard canonical-engine architecture:

```text
                    qnx6/core/
             canonical QNX6 semantics
                  /             \
                 /               \
          qnx6/linux/         qnx6/windows/
          thin VFS             thin IFS
          adapter              adapter
```

Filesystem-defined structures, validation and recovery semantics belong in the
host-neutral core. Operating-system objects and lifecycle rules belong only in
their adapters.

## Current implementation state

QNX6 is currently **reference/import state**.

There is no project-authored canonical QNX6 engine and no Filesystem Support
QNX6 native driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux QNX6 implementation copied by
`.github/workflows/import-linux-filesystems.yml`.

Those files retain their upstream provenance, licences, filenames and source
boundaries. They are evidence about QNX6 interoperability, not project-authored
production code.

The former top-level `kernel/` staging path was misleading because unchanged
upstream Linux source appeared to be a Filesystem Support native module. The
reference source is therefore kept behind the explicit provenance boundary.

## Current directory layout

```text
native/filesystems/qnx6/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux QNX6 source, reference only
```

When an independent implementation begins:

```text
native/filesystems/qnx6/
  DESIGN.md
  core/                  canonical QNX6 filesystem implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns QNX6-defined behaviour including:

- superblock and filesystem geometry interpretation;
- Power-Safe metadata/snapshot selection and consistency rules;
- inode and directory structures;
- block mapping and allocation metadata;
- checksums and byte-order handling defined by the format;
- namespace and filename semantics;
- corruption and range validation;
- exact recovery/publication rules required by the supported format.

Linux VFS objects, page/buffer-cache APIs, kernel locking, credentials and
mount/module lifetime remain Linux-adapter responsibilities.

## File-boundary rule

The imported `dir.c`, `inode.c`, `namei.c`, `qnx6.h` and
`super_mmi.c` remain unchanged beneath `reference/linux/`.

Project-authored implementation files must be organised around portable QNX6
responsibilities rather than mechanically copying, renaming or recommenting the
Linux reference source.

## Promotion sequence

1. document the supported QNX6 format/revision and byte-order rules;
2. implement bounded host-neutral superblock, inode, directory and mapping
   decoding;
3. model Power-Safe metadata selection/recovery explicitly;
4. qualify read-only traversal against independent QNX6 media and malformed
   fixtures;
5. implement a thin Linux adapter over the canonical engine;
6. add mutation only with exact transaction/recovery semantics and destructive
   qualification;
7. add a Windows adapter against the same canonical engine when appropriate.

## Failure policy

Invalid superblocks, contradictory Power-Safe metadata state, malformed
directories, out-of-range block references, checksum failures and unsupported
format states must fail closed. A successful read-only mount by the reference
Linux driver is not evidence of safe project-owned mutation.

## Reference provenance

The Linux reference refresh currently pins Linux v6.12.107 and copies
`fs/qnx6` into `reference/linux/`. Refreshing that evidence may replace only
the imported reference tree and must never overwrite future project-owned
`core/`, `linux/` or `windows/` source.

## Completion rule

The current tree is reference evidence only. QNX6 becomes a Filesystem Support
first-party implementation only when the canonical engine and applicable host
adapter have independent implementation and qualification evidence.
