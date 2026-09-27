# EXT4 canonical implementation

EXT4 in this repository is the canonical Infiltrator Projects EXT4 implementation. The source tree is authoritative.

There must be one EXT4 engine only. No FUSE, vendored, reference, compatibility, or alternate implementation belongs in the active source tree or application catalogue.

The implementation is organised as a portable EXT4 format/core layer plus operating-system adapters. Extents, allocation, journalling, inline data, orphan recovery, resize, xattrs, security and writeback are maintained as project-owned components.

Qualification requires supported-target builds, crash/recovery testing, destructive read/write verification, independently created test media, and source review confirming that the active implementation is project-authored code.
