# Linux SFS adapter

This directory contains the project-authored Linux adapter for the independent
SFS implementation.

Filesystem-format rules belong to `../core/`. This directory owns Linux VFS,
buffer, mount and module integration only. The Linux product is `sfs.ko` and
registers the filesystem name `sfs`.

No alternate or imported filesystem implementation is retained here.
