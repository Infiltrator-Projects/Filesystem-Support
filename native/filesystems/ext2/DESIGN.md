# EXT2 canonical implementation

EXT2 in this repository is the canonical Infiltrator Projects EXT2 implementation. The source tree is authoritative.

There must be one EXT2 engine only. No FUSE, vendored, reference, compatibility, or alternate implementation belongs in the active source tree or application catalogue.

The implementation is organised as a portable EXT2 format/core layer plus operating-system adapters. EXT2 remains separate from EXT3 and EXT4 even where the formats share concepts.

Qualification requires supported-target builds, destructive read/write verification, independently created test media, and source review confirming that the active implementation is project-authored code.
