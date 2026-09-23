# SFS source base

SFS is a distinct filesystem from SFS2.

The Linux migration baseline is Marek Szyprowski's real ASFS Linux driver:

- upstream repository: `twojstaryzdomu/asfs`
- pinned commit: `4c6fd13a7cc77fb515c7fac1397ea593be7a6fd1`
- imported upstream path: `src/`
- source licence: GPL-2.0-or-later, retained in the imported source

The exact upstream source is preserved under `linux/reference/`. It targets an
older Linux kernel and is therefore migration input rather than a claim of
current-kernel qualification.

Filesystem semantics will be moved into `core/` while Linux VFS and kernel
lifecycle code is modernised into the Linux adapter. SFS2 structures or rules
must not be folded into this filesystem.
