# Linux SFS2 adapter

This directory contains the native Linux VFS adapter for the independent SFS2
(SFS\\2, structure version 4) implementation.

The adapter is deliberately mounted read-only during its first qualification
stage. It decodes SFS2's redundant roots, 27-byte object prefix, 48-bit file
size and 16-byte extent nodes with 32-bit block counts through the canonical
SFS2 core.

Writable support is not enabled merely because mutation helpers compile. It
requires the transaction/recovery path plus destructive independently-created
media and cross-implementation verification.
