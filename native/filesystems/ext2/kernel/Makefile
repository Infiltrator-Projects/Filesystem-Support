# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT2 kernel module.

obj-m += ext2.o

ext2-y := balloc.o dir.o file.o ialloc.o inode.o namei.o super.o
ext2-$(CONFIG_EXT2_FS_XATTR) += xattr.o

ccflags-y += -I$(src)
