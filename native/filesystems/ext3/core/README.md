# EXT3 canonical core

This directory is the canonical host-neutral EXT3 filesystem layer.

EXT3 remains in active rewrite state, but the core is no longer merely a future
placeholder. The following project-authored files are active:

- `ext3_core.c`
- `ext3_core.h`

They own the EXT3 format/feature and other host-neutral semantics that have
already crossed the implementation-ownership boundary.

Additional filesystem and JBD semantics move here subsystem by subsystem only
after their implementation is independently rewritten and qualified. The active
Linux adapter under `../linux/` still contains a mixture of project-authored
responsibility units and materially inherited migration implementation; the
authoritative classification is maintained in
`../../../docs/EXT_SOURCE_PROVENANCE.md`.

Do not copy, mechanically transform, rename or recomment migration-era Linux
implementation into this directory. A filesystem rule belongs here because it
is host-neutral EXT3 semantics, not because a Linux file was moved.

Linux VFS/module integration remains in `../linux/`. A future Windows IFS/WDK
adapter belongs in `../windows/` and must consume this same canonical core
rather than implement EXT3 again.
