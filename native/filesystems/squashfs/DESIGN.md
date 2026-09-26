# SquashFS Design and Ownership

## Classification

SquashFS is a compressed, read-only local filesystem image format. Filesystem
Support therefore applies the normal canonical-engine architecture, with
format decoding and decompression policy kept host-neutral.

## Current implementation state

SquashFS is currently **reference/import state**.

There is no project-authored canonical SquashFS engine and no Filesystem
Support SquashFS native module claimed by this directory. The source under
`reference/linux/` is pinned upstream Linux SquashFS code retained as format,
decompression and interoperability evidence.

The imported source retains its original provenance, licences, filenames and
translation-unit boundaries. The former top-level `kernel/` directory was a
legacy staging path and did not make that upstream implementation project code.

## Current directory layout

```text
native/filesystems/squashfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux SquashFS source, reference only
```

When an independent implementation begins:

```text
native/filesystems/squashfs/
  DESIGN.md
  core/                  canonical SquashFS format/read/decompression semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows adapter when implemented
  userspace/             optional image/inspection adapter over the same core
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns SquashFS-defined behaviour including superblock and
version validation, inode/directory tables, fragment tables, ID/xattr tables,
block-list decoding, compressed metadata/data framing, sparse data, export
indexes and supported compressor identifiers.

Compression implementations may be shared only where their byte-format
contract is genuinely generic; SquashFS framing, block sizes and metadata
interpretation remain with the SquashFS core.

## File-boundary rule

Upstream files such as `super.c`, `inode.c`, `dir.c`, `file.c`,
`block.c`, compressor wrappers and SquashFS headers remain unchanged beneath
`reference/linux/`. Project-authored files must be cut around portable format,
metadata, namespace, data and decompression responsibilities rather than copied
or cosmetically renamed from Linux.

## Promotion sequence

1. document the supported SquashFS version/features and compressor set;
2. implement bounded host-neutral superblock and metadata-table decoding;
3. implement read-only inode, directory, fragment and data traversal;
4. qualify supported compression formats against independently generated images
   and malformed/corrupt fixtures;
5. expose the same core through userspace inspection and a thin Linux adapter;
6. add other host adapters only against that same canonical engine.

SquashFS is read-only by format contract for this project; writable mutation is
not required for implementation completeness.

## Failure policy

Invalid table offsets, decompression size mismatches, malformed metadata
streams, unsupported compressors/features, out-of-range inode/data references
and corrupt xattr/fragment/index structures must fail closed.

## Reference provenance

The Linux-reference refresh copies `fs/squashfs` into
`reference/linux/`. It may replace only that imported evidence and must never
overwrite future project-owned `core/`, `linux/`, `windows/` or
`userspace/` source.

## Completion rule

The current tree is reference evidence only. SquashFS becomes a Filesystem
Support first-party implementation when its canonical read engine and
applicable adapter have independent implementation and qualification evidence.
