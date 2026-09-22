# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT4 kernel module.

obj-m += ext4.o

ext4-y := balloc.o block_validity.o dir.o ext4_jbd2.o extents.o \
          extents_status.o file.o fsmap.o ialloc.o indirect.o inline.o \
          inode.o ioctl.o mballoc.o migrate.o mmp.o move_extent.o namei.o \
          page-io.o readpage.o resize.o super.o sysfs.o xattr.o \
          fast_commit.o orphan.o jbd2_transaction.o jbd2_commit.o \
          jbd2_recovery.o jbd2_checkpoint.o jbd2_revoke.o jbd2_journal.o

ext4-$(CONFIG_FS_VERITY) += verity.o
ext4-$(CONFIG_FS_ENCRYPTION) += crypto.o

ccflags-y += -I$(src)/include
ccflags-y += -DCONFIG_EXT4_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT4_FS_SECURITY=1
