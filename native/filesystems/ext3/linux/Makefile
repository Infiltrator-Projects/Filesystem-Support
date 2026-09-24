# EXT3 — Linux adapter build contract
obj-m += ext3.o

ext3-y := core_bridge.o allocation.o directory_io.o file_io.o inode_adapter.o \
          namespace_mutation.o lifecycle.o extended_metadata.o \
          journal_durability.o journal_core.o journal_transactions.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT3_FS_XATTR=1
ccflags-y += -DCONFIG_EXT3_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT3_FS_SECURITY=1
ccflags-y += -DCONFIG_EXT3_DEFAULTS_TO_ORDERED=1
