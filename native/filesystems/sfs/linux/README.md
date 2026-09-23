# Linux SFS adapter

This directory is the active Linux port of Marek Szyprowski's real ASFS
implementation pinned in `../SOURCE.md`.

The imported filesystem algorithms are retained while the obsolete Linux 2.6
VFS interfaces are replaced with current Linux interfaces.  The Linux product
identity is `sfs.ko` and the filesystem registration name is `sfs`.

A duplicate live `reference/` tree is intentionally not retained after this
promotion.  The pinned upstream commit, the preserved copyright/licence headers,
`../SOURCE.md`, and Git history provide the immutable source baseline.
