# EXT3 — Linux adapter build contract
obj-m += ext3.o

ext3-y := canonical.o allocation.o dir.o file.o inode.o namei.o super.o xattr.o \
          journal_durability.o jbd_journal.o jbd_transaction.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT3_FS_XATTR=1
ccflags-y += -DCONFIG_EXT3_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT3_FS_SECURITY=1
ccflags-y += -DCONFIG_EXT3_DEFAULTS_TO_ORDERED=1
