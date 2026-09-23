# SPDX-License-Identifier: GPL-2.0
# EXT3 — Kernel build contract
#
# Defines the single composite filesystem module and the object-level feature composition used by Kbuild.
# This build describes a standalone EXT3 VFS implementation with its historical JBD engine embedded in ext3.ko.
# Correctness: The object list is part of the module boundary: it must not accidentally recreate a helper .ko or omit a filesystem feature engine.
#
# Project rules:
#   - EXT3 requires its journal semantics; it is not an EXT4 compatibility registration.
#   - Preserve the journal, recovery, ordered/writeback/journal data modes and EXT3 on-disk limits.
#   - JBD and the metadata cache are private implementation code, not separately deployed modules.
#

obj-m += ext3.o

ext3-y := canonical.o balloc.o dir.o file.o ialloc.o inode.o namei.o resize.o super.o \
          xattr.o jbd_checkpoint.o jbd_commit.o jbd_journal.o \
          jbd_recovery.o jbd_revoke.o jbd_transaction.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT3_FS_XATTR=1
ccflags-y += -DCONFIG_EXT3_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT3_FS_SECURITY=1
ccflags-y += -DCONFIG_EXT3_DEFAULTS_TO_ORDERED=1
