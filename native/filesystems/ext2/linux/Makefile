# SPDX-License-Identifier: GPL-2.0
# EXT2 — Kernel build contract
#
# Defines the single composite filesystem module and the object-level feature composition used by Kbuild.
# This build describes a deliberately strict, non-journalled EXT2 VFS implementation.
# Correctness: The object list is part of the module boundary: it must not accidentally recreate a helper .ko or omit a filesystem feature engine.
#
# Project rules:
#   - Do not accept a journalled EXT3 volume as EXT2.
#   - Keep on-disk compatibility fields when they are required to parse or reject media correctly.
#   - Keep xattr/ACL/cache code inside ext2.ko rather than creating helper modules.
#

obj-m += ext2.o

ext2-y := canonical.o balloc.o dir.o file.o ialloc.o inode.o namei.o super.o \
          xattr.o

# These are capabilities of the independently deployable Filesystem Support
# EXT2 module. They must not disappear merely because the host kernel disabled
# its own in-tree EXT2 driver or its per-driver Kconfig switches.
ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT2_FS_XATTR=1
ccflags-y += -DCONFIG_EXT2_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT2_FS_SECURITY=1
