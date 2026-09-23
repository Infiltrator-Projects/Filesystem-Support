# SFS source base

SFS is a distinct filesystem from SFS2.

The Linux migration baseline is Marek Szyprowski's real ASFS Linux driver:

- upstream repository: `twojstaryzdomu/asfs`
- pinned commit: `4c6fd13a7cc77fb515c7fac1397ea593be7a6fd1`
- imported upstream path: `src/`
- source licence: GPL-2.0-or-later, retained in the imported source

The pinned source has been promoted into the active `linux/` module tree and
is ported there. A second live reference copy is deliberately not retained:
source headers, this provenance record and Git history preserve the imported
baseline without maintaining duplicate implementation trees.

Filesystem semantics move into `core/` as the rewrite proceeds while Linux
VFS and kernel lifecycle code remains in the Linux adapter. SFS2 structures or
rules must not be folded into this filesystem.
