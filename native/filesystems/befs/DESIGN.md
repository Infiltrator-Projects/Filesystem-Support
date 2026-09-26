# BeFS Design and Ownership

## Classification

BeFS is the BeOS filesystem and is a conventional local block filesystem.
Filesystem Support therefore applies the normal canonical-engine architecture:

```text
                  befs/core/
          canonical BeFS semantics
              /             \
             /               \
       befs/linux/         befs/windows/
       thin VFS             thin IFS
       adapter              adapter
```

## Canonical-core ownership

The imported Linux source shows format-specific areas that must ultimately live
in the host-neutral core rather than a Linux wrapper:

- superblock and endian/format interpretation;
- BeFS inode and filesystem type structures;
- B+tree lookup/traversal used by the namespace and metadata;
- datastream/direct/indirect block mapping;
- directory/name interpretation;
- attribute/metadata decoding supported by the format;
- block/address bounds and corruption validation.

Linux VFS inode/file objects, buffer/page interfaces, mount plumbing and kernel
lifetime rules belong only in the Linux adapter.

## Current implementation state

BeFS is currently **reference/import state**.

The source under `reference/linux/` is the pinned upstream Linux BeFS driver.
It is primarily useful as a mature read-oriented compatibility reference for
legacy BeOS media. Filesystem Support does not claim that source as its own
implementation.

The reference tree keeps its upstream SPDX identifiers, copyrights, filenames,
ChangeLog and source boundaries intact. Project-authored code must not be
created by simply renaming or recommenting those files.

## Current directory layout

```text
native/filesystems/befs/
  DESIGN.md
  reference/
    linux/                  pinned upstream Linux BeFS source
```

While BeFS remains in reference/import state, CI therefore treats
`DESIGN.md` and `reference/` as the only valid filesystem-root entries. The
appearance of `core/`, `linux/`, `windows/`, `userspace/` or `kernel/` is a
promotion event and must arrive with independently authored implementation and
qualification, not as a directory rename. The imported tree is excluded from
the production CMake graph.

The future project layout is:

```text
native/filesystems/befs/
  DESIGN.md
  core/                     canonical BeFS semantics
  linux/                    thin Linux VFS/module adapter
  windows/                  thin Windows IFS/WDK adapter when implemented
  reference/linux/          preserved upstream evidence
```

## File-boundary rule

Upstream files such as `btree.c`, `datastream.c`, `linuxvfs.c` and
`super.c` remain in the reference tree with their original names.

The permanent project implementation should use responsibility boundaries that
separate host-neutral B+tree/datastream/format semantics from operating-system
integration. It must not preserve `linuxvfs.c` as the architectural centre of
the filesystem merely because that is how the reference driver is organised.

## Promotion sequence

1. document the supported BeFS variants and endian/geometry rules;
2. implement bounded host-neutral superblock/inode/datastream decoding;
3. implement read-only B+tree/directory lookup with independent fixtures;
4. add malformed-media/corruption tests;
5. expose read-only access through a thin Linux adapter;
6. add write support only after exact allocation/update/recovery semantics are
   documented and destructively qualified;
7. add Windows support against the same canonical engine when ready.

## Failure policy

Unsupported variants, invalid B+tree nodes, out-of-range datastream runs,
contradictory endian/geometry fields and malformed directory/metadata records
must fail closed.

## Reference provenance

The refresh workflow currently pins Linux v6.12.107 and copies `fs/befs` into
`reference/linux/`. Refreshing reference evidence must never overwrite future
project-authored source.
