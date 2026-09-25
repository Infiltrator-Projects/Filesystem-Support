# ISO9660 Design and Ownership

## Classification

ISO9660 is a read-only optical/image filesystem family. Joliet, Rock Ridge and
zisofs are extensions or presentation/data variants of the same filesystem
rather than separate Filesystem Support implementations.

## Current implementation state

ISO9660 is currently **reference/import state**.

There is no project-authored canonical ISO9660 engine or Filesystem Support
ISO9660 native driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux isofs implementation retained
as semantic and interoperability evidence.

The reference files keep their upstream licences, copyrights, filenames and
implementation boundaries. Relocating them beneath `reference/linux/` is a
provenance correction, not a rewrite.

## Current layout

```text
native/filesystems/iso9660/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux isofs evidence
```

A future first-party implementation is added beside that evidence:

```text
native/filesystems/iso9660/
  DESIGN.md
  core/                  canonical ISO9660/Joliet/Rock Ridge semantics
  linux/                 thin Linux VFS/module adapter
  windows/               thin Windows IFS/WDK adapter when implemented
  userspace/             optional image/mount adapter over the same core
  reference/linux/       preserved upstream evidence
```

## Canonical-core ownership

The future core owns host-neutral ISO-defined behaviour including:

- volume descriptor sequence parsing and validation;
- primary/supplementary volume descriptor geometry;
- directory record decoding and bounds checks;
- extent/interleave and multi-extent file mapping;
- ISO9660 filename/version semantics;
- Joliet UCS-2 name interpretation;
- Rock Ridge/SUSP records, POSIX metadata and symlink representation;
- zisofs compressed-file framing where explicitly supported;
- timestamp and identifier decoding;
- corruption, range and descriptor consistency validation.

Host VFS objects, page/buffer cache, block-device mechanics and module lifetime
belong in adapters.

## Read-only rule

Mounted ISO9660 media is read-only by design. Filesystem Support must not invent
filesystem mutation merely because image-building tools exist. Image creation
is a separate userspace concern.

## Provider rule

The separate `fuseiso` catalogue identity is only an external userspace
provider. Any future Filesystem Support userspace ISO mount belongs under
`iso9660/userspace/` and consumes this same canonical core.

## Promotion rule

ISO9660 leaves reference/import state only after host-neutral descriptor,
directory and extent parsing are independently implemented and qualified against
independently produced ISO9660/Joliet/Rock Ridge images plus malformed fixtures.

Moving or renaming upstream files such as `inode.c`, `rock.c`,
`joliet.c` or `compress.c` never constitutes a rewrite.

## Failure policy

Malformed descriptor lengths, invalid directory records, out-of-range extents,
contradictory volume geometry, invalid Rock Ridge/SUSP chains, invalid Joliet
names and decompression failures must fail closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/isofs` into
`reference/linux/`. Refreshes may update evidence but must never overwrite
future project-owned implementation directories.

## Completion rule

The current tree is reference evidence only. A Filesystem Support ISO9660
implementation is not claimed until a canonical core and relevant adapter have
independent implementation and qualification evidence.
