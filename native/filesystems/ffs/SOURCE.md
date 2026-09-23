# FFS source base

This filesystem is in active rewrite/migration state.

The Linux migration baseline is the repository's pinned Linux **v6.12.107**
`fs/affs` implementation. The source retains its GPL-2.0-only provenance and
implementation history.

Linux historically combines OFS and FFS variants in one AFFS driver. Filesystem
Support deliberately separates FFS into its own filesystem
implementation. The initial Linux migration tree preserves the proven upstream
code while narrowing its mount acceptance to FFS-family
media only.

Filesystem-defined behaviour will move from `linux/` into `core/`
subsystem by subsystem. Linux VFS, block-device, page-cache and module lifecycle
code remains in `linux/`.

No filesystem algorithm in this migration tree was invented from a prose format
summary. The working base is real upstream filesystem code, and inherited
provenance remains until a unit has genuinely been replaced.
