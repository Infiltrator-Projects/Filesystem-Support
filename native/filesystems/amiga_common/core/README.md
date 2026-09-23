# Shared AmigaDOS primitives

This is not a sixth filesystem implementation. It contains only primitives
whose on-disk semantics are genuinely identical between the independent OFS
and FFS cores: 30-byte name validation, classic/international case folding,
directory hashing, and big-endian block checksum arithmetic.

Filesystem identity, allocation, data layout and mutation remain owned by the
separate OFS and FFS canonical cores.
