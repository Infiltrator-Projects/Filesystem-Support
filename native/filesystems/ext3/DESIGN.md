# EXT3 canonical implementation

EXT3 in this repository is the canonical Infiltrator Projects EXT3 implementation. The source tree is authoritative.

There must be one EXT3 engine only. No FUSE, vendored, reference, compatibility, or alternate implementation belongs in the active source tree or application catalogue.

The implementation is organised as a portable EXT3 format/core layer plus operating-system adapters. Journal, namespace, inode, allocation, metadata and recovery paths are maintained as project-owned components rather than as a second copy of another EXT3 implementation.

Qualification requires supported-target builds, journal/recovery tests, destructive read/write verification, independently created test media, and source review confirming that the active implementation is project-authored code.
