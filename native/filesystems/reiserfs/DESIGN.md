# ReiserFS Design and Ownership

## Classification

ReiserFS is a conventional local journalled block filesystem. Filesystem
Support therefore applies the standard one-engine architecture:

```text
                 reiserfs/core/
          canonical ReiserFS semantics
               /              \
              /                \
     reiserfs/linux/       reiserfs/windows/
       thin VFS              thin IFS
       adapter               adapter
```

Filesystem-defined tree, item, allocation, journalling and namespace semantics
belong in the host-neutral core. Operating-system objects and lifecycle belong
only in platform adapters.

## Current implementation state

ReiserFS is currently **reference/import state**.

There is no project-authored canonical ReiserFS engine and no Filesystem
Support ReiserFS native module claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux ReiserFS implementation copied
by the Linux-reference refresh workflow.

The imported files retain their upstream licensing, copyright, filenames and
implementation boundaries. Their size and maturity are reference evidence, not
project authorship.

The former top-level `kernel/` staging directory was misleading because it
made unchanged upstream Linux source look like a project-native implementation.
The exact imported blobs are therefore kept behind the explicit reference
boundary.

## Current directory layout

```text
native/filesystems/reiserfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux ReiserFS source, reference only
```

When an independent rewrite begins:

```text
native/filesystems/reiserfs/
  DESIGN.md
  core/                  canonical ReiserFS filesystem implementation
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future `core/` owns ReiserFS-defined behaviour including:

- superblock, format version and feature interpretation;
- keyed balanced-tree and item ordering/traversal semantics;
- formatted/unformatted item representation and tail packing;
- allocation bitmap and object-ID semantics;
- directory item and filename/hash behaviour;
- file mapping and indirect-item rules;
- journal format, transaction ordering, replay and recovery;
- xattr/ACL storage rules defined by the supported format;
- resize rules that alter persistent filesystem geometry;
- corruption, bounds and consistency validation.

Linux buffer/page-cache APIs, VFS inode/file/dentry objects, procfs, kernel
locking, credentials and module/mount lifetime are Linux-adapter concerns.

## File-boundary rule

Upstream files such as `do_balan.c`, `fix_node.c`, `ibalance.c`,
`lbalance.c`, `stree.c`, `journal.c`, `inode.c` and `super.c`
remain unchanged beneath `reference/linux/`.

Project-authored source must be cut around Filesystem Support responsibilities
and portable invariants rather than preserving or cosmetically renaming the
Linux translation units. Moving, merging or recommenting imported code is never
evidence that it has crossed the authorship boundary.

## Promotion sequence

1. document the supported ReiserFS format/revision and feature set;
2. implement bounded host-neutral superblock, key/item and tree decoding;
3. qualify read-only tree, namespace and file traversal with independent images
   and malformed-media fixtures;
4. implement allocation and journal interpretation/recovery in the canonical
   core;
5. add a thin Linux adapter over that same core;
6. add mutation only with explicit transaction/durability/recovery contracts
   and destructive qualification;
7. add Windows support against the same canonical engine when appropriate;
8. retire reference reliance subsystem-by-subsystem only after independent
   replacement and qualification.

## Failure policy

Malformed tree nodes/items, invalid key ordering, out-of-range block references,
contradictory allocation state, unsupported features and unsafe journal/replay
state must fail closed. A reference Linux implementation being able to mount a
volume is not evidence that a future project writer may safely mutate it.

## Reference provenance

The reference refresh pins the selected upstream Linux source and copies
`fs/reiserfs` into `reference/linux/`. Refreshing that evidence may replace
only the imported reference tree and must never overwrite future project-owned
`core/`, `linux/` or `windows/` source.

## Completion rule

The current tree is reference evidence only. ReiserFS becomes a Filesystem
Support first-party implementation only when its canonical engine and
applicable platform adapter have independent implementation and qualification
evidence.
