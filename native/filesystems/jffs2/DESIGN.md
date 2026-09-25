# JFFS2 Design and Ownership

## Classification

JFFS2 is a log-structured filesystem for raw flash/MTD media. It has
filesystem-defined node, eraseblock, garbage-collection and recovery semantics
and therefore belongs in one canonical filesystem implementation with thin host
adapters.

## Current implementation state

JFFS2 is currently **reference/import state**.

There is no project-authored JFFS2 canonical engine or Filesystem Support JFFS2
native driver claimed by this directory. The source under `reference/linux/`
is the pinned upstream Linux JFFS2 implementation retained as format,
behavioural and interoperability evidence.

Reference files retain their upstream licensing, copyrights, filenames and
implementation boundaries. Relocating them under `reference/linux/` does not
change authorship.

## Current layout

```text
native/filesystems/jffs2/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux JFFS2 evidence
```

A future first-party implementation is added beside that reference tree:

```text
native/filesystems/jffs2/
  DESIGN.md
  core/                  canonical JFFS2 format/filesystem semantics
  linux/                 thin Linux MTD/VFS/module adapter
  windows/               thin adapter if a meaningful raw-flash target exists
  userspace/             optional image/qualification adapter
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future core owns JFFS2-defined host-neutral behaviour including:

- raw node headers, inode/dirent/xattr node formats and CRC validation;
- eraseblock accounting and clean/dirty/free/obsolete space classification;
- inode fragment reconstruction and version ordering;
- directory and link semantics;
- compression framing/selection for explicitly supported codecs;
- summary information and mount-time scan/reconstruction rules;
- garbage-collection selection and relocation invariants;
- write-buffer and NAND-specific filesystem ordering where format-defined;
- xattr/ACL object relationships as represented on flash;
- crash/power-loss recovery invariants;
- corruption and range validation.

Linux MTD objects, VFS objects, kernel threads/workqueues, page cache,
credentials and module lifetime belong in the Linux adapter.

## Flash-media rule

JFFS2 is not a normal block filesystem. The platform adapter must expose the
required erase/read/write/OOB capabilities explicitly rather than hiding MTD
semantics behind assumptions made for disks.

The canonical filesystem must not depend on Linux `struct mtd_info`; it should
consume a bounded flash-media interface representing the operations and
geometry actually required by JFFS2.

## File-boundary rule

Upstream filenames such as `scan.c`, `nodelist.c`, `gc.c`, `erase.c`,
`write.c` and `wbuf.c` remain unchanged in `reference/linux/`.

Project-owned source must be organised around canonical flash/filesystem
responsibilities. Moving, renaming, merging or recommenting copied Linux source
is never a rewrite.

## Promotion sequence

JFFS2 leaves reference/import state only through deliberate independent work:

1. define supported flash geometry, cleanmarker/OOB and compression features;
2. implement bounded host-neutral raw-node decoding and CRC validation;
3. reconstruct inode/directory state from independent raw-flash fixtures;
4. implement eraseblock accounting and mount scan/recovery;
5. qualify malformed/torn/power-loss media cases;
6. implement read-only access through a flash abstraction;
7. add a thin Linux MTD/VFS adapter;
8. implement mutation/GC only with explicit erase/write ordering and destructive
   power-loss qualification;
9. add another host adapter only where a real raw-flash platform contract exists.

## Safety and failure behaviour

Invalid node lengths, bad CRCs, impossible versions, overlapping/out-of-range
flash regions, contradictory eraseblock accounting and unsafe recovery state
must fail closed or be treated as recoverable obsolete data only where the
format rules explicitly allow it.

Power-loss behaviour is part of correctness. Successful ordinary I/O tests are
not sufficient evidence for a writer.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/jffs2` into
`reference/linux/`. Refreshes may update evidence but must never overwrite
future project-authored source.

## Completion rule

The current tree is reference evidence only. JFFS2 is not a Filesystem Support
native implementation until the canonical engine and applicable adapter are
independently implemented and qualified.
