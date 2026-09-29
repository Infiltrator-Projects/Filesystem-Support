# Linux SFS2 adapter

This directory contains the native Linux VFS adapter for the independent SFS2
(`SFS\\2`, structure version 4) implementation.

The active adapter contains allocation, extent/tree mapping, file I/O,
namespace mutation and lifecycle code and is built with its read/write paths
enabled.  It consumes the canonical SFS2 core for the format-specific identity,
checksum, 27-byte object prefix, 48-bit file size and 16-byte extent records
with 32-bit block counts.

Writable mounting is fail-closed. Missing/invalid redundant roots, an
interrupted `TRFA` state or an invalid transaction marker force the volume
read-only.  Runtime qualification, including interrupted-update behaviour, is
the authority for writable-support claims; successful compilation alone is
not.

The historical SFS2 format/tool sources retained under `../reference/` are
non-shipping verification evidence. They are not linked into this module.
