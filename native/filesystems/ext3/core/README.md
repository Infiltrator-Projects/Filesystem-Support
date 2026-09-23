# EXT3 canonical core

EXT3 is in active rewrite state. The current working implementation still
resides under ../linux/ because its filesystem semantics and Linux VFS glue
have not yet been safely separated.

Project-authored, host-neutral EXT3 format and filesystem semantics move into
this directory subsystem by subsystem as they are rewritten and qualified.

Do not copy or mechanically transform the migration-era Linux implementation
into this directory.
