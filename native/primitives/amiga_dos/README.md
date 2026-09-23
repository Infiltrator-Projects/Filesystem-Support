# AmigaDOS shared primitives

This directory is not a filesystem implementation.

It contains only low-level rules that are genuinely identical for the
independent OFS and FFS implementations, such as bounded name handling,
case-folding/hash arithmetic, checksum arithmetic, bitmap geometry helpers,
file-block geometry helpers and symlink syntax translation.

OFS and FFS own their filesystem semantics, Linux adapters, Windows adapters,
allocation policy, inode lifecycle, namespace mutation, mount behaviour and
recovery independently under their own filesystem directories.
