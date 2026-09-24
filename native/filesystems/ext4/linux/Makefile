# EXT4 — Kernel build contract
#
# Defines the single composite filesystem module and the object-level feature composition used by Kbuild.
# This build describes a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
# Correctness: The object list is part of the module boundary: it must not accidentally recreate a helper .ko or omit a filesystem feature engine.
#
# Project rules:
#   - Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.
#   - Preserve every valid EXT4 feature path supported by the pinned implementation.
#   - Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.
#

obj-m += ext4.o

ext4-y := canonical.o balloc.o block_validity.o dir.o ext4_jbd2.o extents.o \
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
