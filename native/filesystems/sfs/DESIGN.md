# Amiga SFS canonical implementation

SFS in this repository is the canonical Infiltrator Projects SFS implementation. The source tree is authoritative.

There must be one SFS engine only. No FUSE, vendored, reference, compatibility, or alternate implementation belongs in the active source tree or application catalogue.

SFS is separate from OFS and FFS. The Linux adapter should use project-native IFS_SFS naming consistently as the implementation is normalised.

Qualification requires supported-target builds, destructive read/write verification, independently created test media, and source review confirming that the active implementation is project-authored code.
