# Canonical FFS core

This directory owns host-neutral FFS format semantics. DOS-type classification
for FFS, international FFS, directory-cache FFS and multi-user variants is
canonical here. Host adapters consume these rules instead of maintaining their
own format switches.

`ffs_disk_layout.h` is the portable on-disk layout contract.  Linux-specific
buffer/VFS objects must remain outside it; host adapters consume these format
structures rather than defining a private copy.
