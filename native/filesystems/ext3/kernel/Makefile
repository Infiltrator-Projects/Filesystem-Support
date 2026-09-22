# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT3 kernel module.

obj-m += ext3.o

ext3-y := balloc.o dir.o file.o ialloc.o inode.o namei.o resize.o super.o \
          xattr.o jbd_checkpoint.o jbd_commit.o jbd_journal.o \
          jbd_recovery.o jbd_revoke.o jbd_transaction.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT3_FS_XATTR=1
ccflags-y += -DCONFIG_EXT3_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT3_FS_SECURITY=1
ccflags-y += -DCONFIG_EXT3_DEFAULTS_TO_ORDERED=1
