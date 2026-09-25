# Linux NTFS3 Provider and Reference Ownership

## Classification

`ntfs3` is the Linux kernel provider identity for NTFS. It is not a separate
filesystem format.

Filesystem Support has one canonical ownership point for NTFS semantics:
`native/filesystems/ntfs/`. A future first-party NTFS engine belongs there and
is shared by its Linux and Windows adapters.

The `ntfs3` catalogue entry exists because newer Linux kernels may provide the
native NTFS3 driver independently of NTFS-3G. Provider choice must not create a
second Filesystem Support implementation of NTFS.

## Current implementation state

NTFS3 is currently **reference/import state**.

The source below `reference/linux/` is the pinned upstream Linux NTFS3 driver
copied by `.github/workflows/import-linux-filesystems.yml`. It remains
upstream implementation evidence and is not project-authored Filesystem Support
code.

The former `kernel/` staging directory was misleading because it looked like
a production Filesystem Support native module. The unchanged upstream blobs are
therefore retained under an explicit reference/provenance boundary instead.

## Current directory layout

```text
native/filesystems/ntfs3/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux NTFS3 source, reference only
```

There is deliberately no project-owned `core/`, `linux/`, `windows/` or
`kernel/` implementation in this provider directory.

## One NTFS implementation rule

NTFS3 source is useful evidence for NTFS behaviour, compatibility, edge cases
and tests. It must not become an independent project engine.

When Filesystem Support implements NTFS:

```text
native/filesystems/ntfs/
  core/                  canonical NTFS semantics
  linux/                 thin Linux adapter
  windows/               thin Windows adapter
  reference/             optional preserved implementation evidence
```

Filesystem-defined behaviour such as MFT records, attributes, run lists,
directory indexes, allocation, metadata/security semantics and logfile recovery
belongs only in that canonical NTFS core.

The future Linux adapter may learn from NTFS3's observable behaviour and
platform integration, but it must consume the canonical NTFS engine rather than
copying NTFS3 filesystem algorithms into a second implementation.

## Reference-file rule

Files below `reference/linux/` retain their upstream filenames, SPDX
identifiers, copyright/provenance and implementation boundaries.

Renaming `attrib.c`, `frecord.c`, `fslog.c`, `run.c`, `super.c` or any
other copied unit into project responsibility names would not constitute a
rewrite and must not be used as evidence of project authorship.

## Provider behaviour

The catalogue may report the Linux `ntfs3` module as available when the
running kernel actually provides it. That reports an operating-system provider,
not completion of Filesystem Support's canonical NTFS implementation.

NTFS-3G and NTFS3 remain external providers until the canonical `ntfs/` engine
is independently implemented and qualified.

## Safety

Availability of the Linux NTFS3 driver does not by itself establish that every
NTFS feature combination or damaged volume is safe to modify. Filesystem
Support must keep provider availability distinct from first-party capability
and safety claims.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/ntfs3` into
`reference/linux/`. Refreshing that evidence may replace only the copied
reference tree and must never overwrite future code under the canonical
`native/filesystems/ntfs/` ownership point.
