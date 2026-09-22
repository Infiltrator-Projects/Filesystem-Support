# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux ext4-filesystem routines.
#

obj-m += ext4.o

ext4-y	:= balloc.o bitmap.o block_validity.o dir.o ext4_jbd2.o extents.o \
		extents_status.o file.o fsmap.o fsync.o hash.o ialloc.o \
		indirect.o inline.o inode.o ioctl.o mballoc.o migrate.o \
		mmp.o move_extent.o namei.o page-io.o readpage.o resize.o \
		super.o symlink.o sysfs.o xattr.o xattr_hurd.o xattr_trusted.o \
		xattr_user.o fast_commit.o orphan.o

ext4-$(CONFIG_EXT4_FS_POSIX_ACL)	+= acl.o
ext4-$(CONFIG_EXT4_FS_SECURITY)		+= xattr_security.o
ext4-$(CONFIG_FS_VERITY)		+= verity.o
ext4-$(CONFIG_FS_ENCRYPTION)		+= crypto.o

# Infiltrator: JBD2 and mbcache are linked into ext4.ko.
# No jbd2.ko or mbcache.ko is produced.
ext4-y += mbcache.o jbd2_transaction.o jbd2_commit.o \
    jbd2_recovery.o jbd2_checkpoint.o jbd2_revoke.o jbd2_journal.o
ccflags-y += -I$(src)/include
