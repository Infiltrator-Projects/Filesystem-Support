# EXT2 — Linux adapter build contract
# Filesystem Support responsibility boundaries: canonical core bridge,
# allocation, namespace, I/O/inode integration, metadata and lifecycle.

obj-m += ext2.o
ext2-y := core_bridge.o allocation.o namespace.o io.o metadata.o lifecycle.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT2_FS_XATTR=1
ccflags-y += -DCONFIG_EXT2_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT2_FS_SECURITY=1
