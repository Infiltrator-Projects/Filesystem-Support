# Canonical OFS core

This directory owns host-neutral OFS format semantics. DOS-type classification
for OFS, international OFS, directory-cache OFS and their multi-user variants
is canonical here. Host adapters consume these rules instead of maintaining
their own format switches.

The canonical on-disk structure contract is `ofs_disk_layout.h`. It uses
host-neutral scalar aliases so Linux, Windows and userspace qualification code
can consume the same OFS media layout without importing Linux VFS types.
